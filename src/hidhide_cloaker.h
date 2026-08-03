#pragma once

// hidhide_cloaker.h
// Wraps the HidHide kernel driver IOCTL interface to:
//   1. Add this process's executable to the HidHide whitelist.
//   2. Enumerate attached HID game controllers and cloak them so only
//      whitelisted processes (i.e. us + ViGEmBus) see the physical device.
//   3. Restore the original cloak/whitelist state on destruction.
//
// HidHide must be installed: https://github.com/nefarius/HidHide
// The driver exposes a control device at \\.\HidHide.
//
// All IOCTL codes and structures match the HidHide public API as of v1.x.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>       // HidD_GetAttributes, HidD_GetProductString
#include <setupapi.h>
#include <devguid.h>

#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// ─── HidHide IOCTL codes ──────────────────────────────────────────────────────
// These MUST match nefarius/HidHide's Shared/HidHideIoctlContract.h byte-for-byte
// or DeviceIoControl fails with ERROR_INVALID_FUNCTION and cloaking silently
// does nothing.  Two things are easy to get wrong (and were, previously):
//   • the custom device type is 32769 (0x8001), NOT 0x8000, and
//   • EVERY code — including the "set" ones — uses FILE_READ_DATA access.
#define HIDHIDE_DEVICE_TYPE 32769
#define IOCTL_HIDHIDE_GET_WHITELIST   CTL_CODE(HIDHIDE_DEVICE_TYPE, 2048, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_HIDHIDE_SET_WHITELIST   CTL_CODE(HIDHIDE_DEVICE_TYPE, 2049, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_HIDHIDE_GET_BLACKLIST   CTL_CODE(HIDHIDE_DEVICE_TYPE, 2050, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_HIDHIDE_SET_BLACKLIST   CTL_CODE(HIDHIDE_DEVICE_TYPE, 2051, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_HIDHIDE_GET_ACTIVE      CTL_CODE(HIDHIDE_DEVICE_TYPE, 2052, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_HIDHIDE_SET_ACTIVE      CTL_CODE(HIDHIDE_DEVICE_TYPE, 2053, METHOD_BUFFERED, FILE_READ_DATA)

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Convert a narrow string to wstring.
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

// Convert a wstring to UTF-8.
static std::string ToNarrow(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// HidHide stores its whitelist/blacklist as a double-null-terminated wide
// string block (like REG_MULTI_SZ).  Parse it into a vector of strings.
static std::vector<std::wstring> ParseMultiSz(const std::vector<BYTE>& buf) {
    std::vector<std::wstring> out;
    if (buf.size() < 2) return out;
    const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
    while (*p) {
        out.emplace_back(p);
        p += out.back().size() + 1;
    }
    return out;
}

// Serialize a vector of wstrings back into a double-null-terminated block.
static std::vector<BYTE> SerializeMultiSz(const std::vector<std::wstring>& v) {
    std::vector<BYTE> out;
    for (const auto& s : v) {
        const BYTE* raw = reinterpret_cast<const BYTE*>(s.c_str());
        out.insert(out.end(), raw, raw + (s.size() + 1) * sizeof(wchar_t));
    }
    // Trailing null terminator.
    out.push_back(0); out.push_back(0);
    return out;
}

// ─── Main class ──────────────────────────────────────────────────────────────

class HidHideCloaker {
public:
    // Result of an attempt to cloak controllers.
    struct CloakResult {
        bool        hidhideAvailable = false;
        bool        wasAlreadyActive = false;
        int         devicesCloaked   = 0;
        bool        whitelistAdded   = false;
        std::string statusMessage;
    };

    HidHideCloaker() = default;
    ~HidHideCloaker() { Restore(); }

    // Disable copy.
    HidHideCloaker(const HidHideCloaker&) = delete;
    HidHideCloaker& operator=(const HidHideCloaker&) = delete;

    // Apply cloaking.  Call once before starting the passthrough loop.
    // Returns a result struct describing what happened; check statusMessage
    // and hidhideAvailable for diagnostics.
    CloakResult Apply() {
        CloakResult r;

        hDevice_ = OpenHidHide();
        if (hDevice_ == INVALID_HANDLE_VALUE) {
            r.hidhideAvailable = false;
            DWORD err = GetLastError();
            std::ostringstream ss;
            ss << "HidHide driver not found (LastError=0x" << std::hex << err << std::dec << ") — physical controller will remain visible.\n"
               << "Install HidHide from https://github.com/nefarius/HidHide if you want cloaking.";
            r.statusMessage = ss.str();
            return r;
        }
        r.hidhideAvailable = true;

        // ── 1. Save current state ──────────────────────────────────────────
        savedActive_    = GetActive();
        savedWhitelist_ = GetWhitelist();
        savedBlacklist_ = GetBlacklist();
        r.wasAlreadyActive = savedActive_;

        // ── 2. Whitelist this executable ──────────────────────────────────
        std::wstring exePath = GetOwnExePath();
        auto wl = savedWhitelist_;
        if (!exePath.empty()) {
            bool already = std::find(wl.begin(), wl.end(), exePath) != wl.end();
            if (!already) {
                wl.push_back(exePath);
                SetWhitelist(wl);
                ownExeAddedToWhitelist_ = true;
                r.whitelistAdded = true;
            }
        }

        // ── 3. Cloak physical HID game controllers ────────────────────────
        auto physicalPaths = EnumerateHidGameControllers();
        auto bl = savedBlacklist_;

        for (const auto& path : physicalPaths) {
            bool already = std::find(bl.begin(), bl.end(), path) != bl.end();
            if (!already) {
                bl.push_back(path);
                addedToBlacklist_.push_back(path);
                r.devicesCloaked++;
            }
        }

        if (!addedToBlacklist_.empty()) {
            SetBlacklist(bl);
        }

        // ── 4. Enable cloaking ────────────────────────────────────────────
        SetActive(true);
        applied_ = true;

        std::ostringstream ss;
        ss << "HidHide cloaking active.\n"
           << "  Whitelisted : " << ToNarrow(GetOwnExePath()) << "\n"
           << "  Cloaked     : " << r.devicesCloaked << " physical HID controller(s)";
        r.statusMessage = ss.str();
        return r;
    }

    // Undo everything Apply() did.  Called automatically by the destructor.
    void Restore() {
        if (!applied_ || hDevice_ == INVALID_HANDLE_VALUE) return;

        // Remove entries we added to the blacklist.
        if (!addedToBlacklist_.empty()) {
            auto bl = GetBlacklist();
            for (const auto& path : addedToBlacklist_) {
                bl.erase(std::remove(bl.begin(), bl.end(), path), bl.end());
            }
            SetBlacklist(bl);
            addedToBlacklist_.clear();
        }

        // Remove this exe from the whitelist if we added it.
        if (ownExeAddedToWhitelist_) {
            std::wstring exePath = GetOwnExePath();
            auto wl = GetWhitelist();
            wl.erase(std::remove(wl.begin(), wl.end(), exePath), wl.end());
            SetWhitelist(wl);
            ownExeAddedToWhitelist_ = false;
        }

        // Restore the active flag to whatever it was before.
        SetActive(savedActive_);

        applied_ = false;
        CloseHandle(hDevice_);
        hDevice_ = INVALID_HANDLE_VALUE;
    }

    bool IsApplied() const { return applied_; }

private:
    HANDLE                   hDevice_                 = INVALID_HANDLE_VALUE;
    bool                     applied_                 = false;
    bool                     savedActive_             = false;
    bool                     ownExeAddedToWhitelist_  = false;
    std::vector<std::wstring> savedWhitelist_;
    std::vector<std::wstring> savedBlacklist_;
    std::vector<std::wstring> addedToBlacklist_;

    // ── Driver handle ──────────────────────────────────────────────────────
    // HidHide v1.2+ exposes the control device as HidHideDeviceConfiguration.
    // Older builds used the bare HidHide name.  Try the modern path first.
    static HANDLE OpenHidHide() {
        static const wchar_t* kPaths[] = {
            L"\\\\.\\HidHide",                       // documented control-device name
            L"\\\\.\\HidHideDeviceConfiguration",    // legacy fallback
        };
        for (const wchar_t* p : kPaths) {
            HANDLE h = CreateFileW(
                p,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (h != INVALID_HANDLE_VALUE) return h;
        }
        return INVALID_HANDLE_VALUE;
    }

    // ── IOCTL helpers ──────────────────────────────────────────────────────
    std::vector<BYTE> IoctlGet(DWORD code) {
        // First call with a tiny buffer to learn the required size.
        std::vector<BYTE> buf(4096);
        DWORD returned = 0;
        while (true) {
            BOOL ok = DeviceIoControl(hDevice_, code,
                nullptr, 0,
                buf.data(), static_cast<DWORD>(buf.size()),
                &returned, nullptr);
            if (ok) {
                buf.resize(returned);
                return buf;
            }
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                buf.resize(buf.size() * 2);
            } else {
                return {};
            }
        }
    }

    void IoctlSet(DWORD code, const std::vector<BYTE>& data) {
        DWORD returned = 0;
        DeviceIoControl(hDevice_, code,
            const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()),
            nullptr, 0,
            &returned, nullptr);
    }

    // ── Active flag ────────────────────────────────────────────────────────
    bool GetActive() {
        auto buf = IoctlGet(IOCTL_HIDHIDE_GET_ACTIVE);
        return !buf.empty() && buf[0] != 0;
    }

    void SetActive(bool enable) {
        std::vector<BYTE> buf = { enable ? (BYTE)1 : (BYTE)0 };
        IoctlSet(IOCTL_HIDHIDE_SET_ACTIVE, buf);
    }

    // ── Whitelist ──────────────────────────────────────────────────────────
    std::vector<std::wstring> GetWhitelist() {
        return ParseMultiSz(IoctlGet(IOCTL_HIDHIDE_GET_WHITELIST));
    }

    void SetWhitelist(const std::vector<std::wstring>& v) {
        IoctlSet(IOCTL_HIDHIDE_SET_WHITELIST, SerializeMultiSz(v));
    }

    // ── Blacklist (cloaked device instance paths) ──────────────────────────
    std::vector<std::wstring> GetBlacklist() {
        return ParseMultiSz(IoctlGet(IOCTL_HIDHIDE_GET_BLACKLIST));
    }

    void SetBlacklist(const std::vector<std::wstring>& v) {
        IoctlSet(IOCTL_HIDHIDE_SET_BLACKLIST, SerializeMultiSz(v));
    }

    // ── Own executable path ────────────────────────────────────────────────
    static std::wstring GetOwnExePath() {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return buf;
    }

    // ── Enumerate HID game controllers ────────────────────────────────────
    // Returns device instance paths (not symbolic link paths) for all HID
    // devices that report themselves as joysticks/gamepads.  These are the
    // strings HidHide expects in its blacklist.
    //
    // Important: HidHide-hidden devices are still visible at the Setup API
    // enumeration level — the filter only blocks CreateFile from unauthorised
    // processes.  Because HidHide itself hasn't started cloaking yet when
    // Apply() calls us, and because this process will be whitelisted before
    // we reach this point, we should be able to open them normally.  But to
    // be safe we also fall back to opening read-only, and we emit the
    // instance path even for devices we couldn't open, as long as the HID
    // symbolic-link path contains "VID_" (a reliable sign of a real HID
    // device interface, not a system pseudo-device).
    static std::vector<std::wstring> EnumerateHidGameControllers() {
        std::vector<std::wstring> out;

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);

        HDEVINFO devInfo = SetupDiGetClassDevsW(
            &hidGuid, nullptr, nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        );
        if (devInfo == INVALID_HANDLE_VALUE) return out;

        SP_DEVICE_INTERFACE_DATA ifaceData = {};
        ifaceData.cbSize = sizeof(ifaceData);

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifaceData); ++i) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &needed, nullptr);

            std::vector<BYTE> detailBuf(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            SP_DEVINFO_DATA devInfoData = {};
            devInfoData.cbSize = sizeof(devInfoData);

            if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail, needed, nullptr, &devInfoData))
                continue;

            // Try to open the device once — prefer read+write, fall back to
            // read-only.  A single open is enough for both attribute and
            // capability queries; don't open twice.
            HANDLE hDev = CreateFileW(
                detail->DevicePath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr
            );
            if (hDev == INVALID_HANDLE_VALUE) {
                hDev = CreateFileW(
                    detail->DevicePath,
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr
                );
            }

            bool isGamepad = false;

            if (hDev != INVALID_HANDLE_VALUE) {
                // Filter by HID Usage Page 0x01 (Generic Desktop) and
                // Usage 0x04 (Joystick) or 0x05 (Gamepad).
                PHIDP_PREPARSED_DATA ppd = nullptr;
                if (HidD_GetPreparsedData(hDev, &ppd) && ppd) {
                    HIDP_CAPS caps = {};
                    if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                        isGamepad = (caps.UsagePage == 0x01 &&
                                     (caps.Usage == 0x04 || caps.Usage == 0x05));
                    }
                    HidD_FreePreparsedData(ppd);
                }
                CloseHandle(hDev);
            } else {
                // Could not open the device (e.g. it's hidden by HidHide
                // and we haven't been whitelisted yet for this path).
                // Fall back: any HID interface whose symbolic path contains
                // "VID_" and "PID_" is very likely a real hardware controller.
                // We add it to the blacklist speculatively; HidHide will
                // silently ignore paths it doesn't recognise as a HID device.
                std::wstring dp = detail->DevicePath;
                auto toUpper = [](std::wstring s) {
                    for (auto& c : s) c = towupper(c);
                    return s;
                };
                std::wstring dpUp = toUpper(dp);
                isGamepad = (dpUp.find(L"VID_") != std::wstring::npos &&
                             dpUp.find(L"PID_") != std::wstring::npos);
            }

            if (!isGamepad) continue;

            // Get the device instance path (what HidHide's blacklist expects).
            wchar_t instPath[MAX_DEVICE_ID_LEN] = {};
            if (CM_Get_Device_IDW(devInfoData.DevInst, instPath, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
                std::wstring inst = instPath;
                // Avoid duplicates.
                bool already = std::find(out.begin(), out.end(), inst) != out.end();
                if (!already) out.emplace_back(std::move(inst));
            }
        }

        SetupDiDestroyDeviceInfoList(devInfo);
        return out;
    }
};