#pragma once

// ds4_hid_reader.h
// Opens ANY HID gamepad/joystick directly via the HID API and parses its
// USB input report into a PhysicalGamepadState.
//
// Detection strategy (brand-agnostic):
//   1. Enumerate all HID interfaces present.
//   2. Filter by HID Usage Page 0x01 (Generic Desktop) and
//      Usage 0x04 (Joystick) or 0x05 (Gamepad).
//   3. Try to open with GENERIC_READ | GENERIC_WRITE; if that fails
//      (e.g. the device is hidden by HidHide but this process is
//      whitelisted) fall back to GENERIC_READ only.
//   4. Parse report 0x01 for Sony DS4/DualSense layout, or attempt a
//      best-effort generic parse for other devices.
//
// Sony-specific parsing (DS4 v1, DS4 v2, DualSense):
//   The full structured layout documented below is applied.
//
// Generic HID parsing:
//   Uses HidP_GetUsageValue / HidP_GetUsages to read axes and buttons
//   from the preparsed data, regardless of report layout.
//
// Usage:
//   DS4HidReader reader;
//   if (reader.Open()) { ... }
//   PhysicalGamepadState state;
//   if (reader.Read(state)) { /* use state */ }
//   reader.Close();

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// The HID API headers must be given C linkage.  The Windows SDK wraps them in
// extern "C" internally (so this is a no-op there), but MinGW-w64's copies do
// not, which otherwise leaves the HidD_*/HidP_* symbols C++-mangled and
// unresolved at link time.
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <setupapi.h>
#include <devguid.h>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

// ViGEm Common.h defines the XUSB_GAMEPAD_* button constants used by the
// parsers below.  Including it here (rather than relying on the includer to
// pull it in first) makes this header self-contained and order-independent.
#include "ViGEm/Common.h"

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// ── Normalised gamepad state ──────────────────────────────────────────────────
// Axes: -32768..32767 matching XINPUT_GAMEPAD conventions.
// Triggers: 0..255.
// Button flags match XUSB_GAMEPAD_* so BuildX360Report / BuildDS4Report work
// unmodified, with two extras for the PS-specific buttons.
struct PhysicalGamepadState {
    int16_t  leftX        = 0;
    int16_t  leftY        = 0;
    int16_t  rightX       = 0;
    int16_t  rightY       = 0;
    uint8_t  leftTrigger  = 0;
    uint8_t  rightTrigger = 0;
    uint16_t buttons      = 0;   // XUSB_GAMEPAD_* flags
    bool     psButton     = false;
    bool     touchpad     = false;
    bool     valid        = false;
};

// ── Known Sony VID/PID pairs (for structured DS4/DualSense parsing) ───────────
enum class SonyDeviceType {
    DS4,         // DualShock 4 v1/v2  — report 0x01 only (USB)
    DualSense,   // DualSense / Edge   — report 0x01 (USB) or 0x31 (BT)
};
struct SonyPidEntry { uint16_t vid; uint16_t pid; SonyDeviceType type; const char* name; };
static const SonyPidEntry kSonyControllers[] = {
    { 0x054C, 0x05C4, SonyDeviceType::DS4,       "DualShock 4 v1"       },
    { 0x054C, 0x09CC, SonyDeviceType::DS4,       "DualShock 4 v2"       },
    { 0x054C, 0x0CE6, SonyDeviceType::DualSense, "DualSense (PS5)"      },
    { 0x054C, 0x0DF2, SonyDeviceType::DualSense, "DualSense Edge (PS5)" },
    { 0x054C, 0x0E5F, SonyDeviceType::DualSense, "DualSense PC Edition" },
    { 0x054C, 0x0297, SonyDeviceType::DualSense, "PS Access Controller" },
};

// ── DS4 USB report byte layout (report ID 0x01, USB mode) ────────────────────
// Byte  0: Report ID (0x01)
// Byte  1: Left  stick X  (0=left, 128=centre, 255=right)
// Byte  2: Left  stick Y  (0=up,   128=centre, 255=down)
// Byte  3: Right stick X
// Byte  4: Right stick Y
// Byte  5: low nibble = dpad hat (0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW,8=none)
//          high nibble = Square(bit4) Cross(5) Circle(6) Triangle(7)
// Byte  6: L1(0) R1(1) L2(2) R2(3) Share(4) Options(5) L3(6) R3(7)
// Byte  7: PS(0) Touchpad(1) ...
// Byte  8: L2 axis
// Byte  9: R2 axis
// Byte positions of the fields we care about inside a Sony input report.
// DS4 and DualSense share the SAME bit meanings inside the three button bytes,
// but they place the stick/trigger/button bytes at DIFFERENT offsets, and each
// has a distinct USB vs Bluetooth framing.  GetSonyOffsets() resolves the right
// set of offsets from the device type + report ID.
struct SonyReportOffsets {
    int lx, ly, rx, ry;   // stick byte positions
    int l2, r2;           // analog trigger byte positions
    int b0, b1, b2;       // button bytes: hat+face / shoulders+share+opts+thumbs / ps+touchpad
};

// ── Helper: map 0..255 axis to -32768..32767 ─────────────────────────────────
static inline int16_t HidAxisToXInput(uint8_t v) {
    // (v-128)*257 spans -32896..32639, so the negative end overflows a signed
    // 16-bit value: byte 0 would wrap to +32640 (wrong sign!).  Compute in a
    // wide int and clamp to the int16 range so the extremes stay correct.
    int scaled = (static_cast<int>(v) - 128) * 257;
    if (scaled < -32768) scaled = -32768;
    if (scaled >  32767) scaled =  32767;
    return static_cast<int16_t>(scaled);
}

// ── Helper: map a raw HID logical value to -32768..32767 ─────────────────────
// logMin/logMax come from the HID report descriptor for this axis.
static inline int16_t HidLogicalToXInput(LONG value, LONG logMin, LONG logMax) {
    if (logMax <= logMin) return 0;
    // Normalise to 0..65535, then shift to -32768..32767.
    double norm = static_cast<double>(value - logMin) / static_cast<double>(logMax - logMin);
    int32_t scaled = static_cast<int32_t>(norm * 65535.0 + 0.5);
    scaled = std::max(0, std::min(65535, scaled));
    return static_cast<int16_t>(scaled - 32768);
}

// ── Candidate device info collected during enumeration ───────────────────────
struct HidGamepadCandidate {
    std::wstring   devicePath;
    uint16_t       vid            = 0;
    uint16_t       pid            = 0;
    bool           isSony         = false;
    SonyDeviceType sonyType       = SonyDeviceType::DS4;
    const char*    friendlyName   = "Generic HID Gamepad";
    // How good this HID collection is for our purposes.  A single physical
    // device (e.g. DualSense Edge) exposes several HID collections under the
    // same VID/PID; we keep the highest-scoring one:
    //   3 = Sony device + gamepad/joystick collection (richest structured parse)
    //   2 = generic gamepad/joystick collection
    //   1 = Sony device, non-gamepad collection (audio/vendor/touchpad)
    int            score          = 0;
};

// ── Main class ───────────────────────────────────────────────────────────────
class DS4HidReader {
public:
    DS4HidReader()  = default;
    ~DS4HidReader() { Close(); }

    DS4HidReader(const DS4HidReader&)            = delete;
    DS4HidReader& operator=(const DS4HidReader&) = delete;

    const char* DeviceName() const { return deviceName_; }
    bool        IsOpen()     const { return hDev_ != INVALID_HANDLE_VALUE; }

    // Scan all HID devices and open the first HID gamepad or joystick found.
    // Prefers Sony devices (structured DS4 parse) but accepts any gamepad.
    // Returns true if a supported device was found and opened.
    bool Open() {
        Close();

        auto candidates = EnumerateCandidates();
        if (candidates.empty()) return false;

        // Prefer Sony devices so we get the richest parse; fall back to any.
        HidGamepadCandidate* chosen = nullptr;
        for (auto& c : candidates) {
            if (c.isSony) { chosen = &c; break; }
        }
        if (!chosen) chosen = &candidates[0];

        // Try read+write first (needed to send output reports), fall back to
        // read-only. HidHide-hidden devices that whitelist this process will
        // still be accessible — we just may not be able to write to them.
        HANDLE h = CreateFileW(
            chosen->devicePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(
                chosen->devicePath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                nullptr
            );
        }
        if (h == INVALID_HANDLE_VALUE) return false;

        hDev_       = h;
        isSony_     = chosen->isSony;
        sonyType_   = chosen->sonyType;
        deviceName_ = chosen->friendlyName;
        hEvent_     = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        // Cache the preparsed data (and value caps) for generic HID parsing so
        // the hot Read() path never has to re-query them.
        if (!isSony_) {
            HidD_GetPreparsedData(hDev_, &ppd_);
            if (ppd_) {
                HidP_GetCaps(ppd_, &hidCaps_);
                USHORT n = hidCaps_.NumberInputValueCaps;
                if (n) {
                    valueCaps_.resize(n);
                    if (HidP_GetValueCaps(HidP_Input, valueCaps_.data(), &n, ppd_) == HIDP_STATUS_SUCCESS)
                        valueCaps_.resize(n);
                    else
                        valueCaps_.clear();
                }
            }
        }

        return true;
    }

    void Close() {
        if (ppd_) {
            HidD_FreePreparsedData(ppd_);
            ppd_ = nullptr;
        }
        if (hDev_ != INVALID_HANDLE_VALUE) {
            CancelIo(hDev_);
            CloseHandle(hDev_);
            hDev_ = INVALID_HANDLE_VALUE;
        }
        if (hEvent_ != nullptr) {
            CloseHandle(hEvent_);
            hEvent_ = nullptr;
        }
        deviceName_ = nullptr;
        isSony_     = false;
        sonyType_   = SonyDeviceType::DS4;
        ZeroMemory(&hidCaps_, sizeof(hidCaps_));
        valueCaps_.clear();
    }

    // Non-blocking read: issues an overlapped ReadFile then waits up to
    // timeoutMs milliseconds.  Returns true and fills state on success.
    bool Read(PhysicalGamepadState& out, DWORD timeoutMs = 4) {
        if (!IsOpen()) return false;

        uint8_t buf[256] = {};
        OVERLAPPED ov    = {};
        ov.hEvent        = hEvent_;
        ResetEvent(hEvent_);

        DWORD bytesRead = 0;
        BOOL  ok        = ReadFile(hDev_, buf, sizeof(buf), &bytesRead, &ov);

        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) { Close(); return false; }
            DWORD wait = WaitForSingleObject(hEvent_, timeoutMs);
            if (wait != WAIT_OBJECT_0) {
                CancelIo(hDev_);
                return false;  // timeout — no new data yet
            }
            if (!GetOverlappedResult(hDev_, &ov, &bytesRead, FALSE)) {
                Close();
                return false;
            }
        }

        if (bytesRead == 0) return false;

        if (isSony_) {
            SonyReportOffsets off;
            if (!GetSonyOffsets(sonyType_, buf, bytesRead, off)) {
                // Unrecognised or too-short report (e.g. a DualSense Bluetooth
                // "minimal" report before full mode is enabled).  Skip silently
                // so a single odd packet doesn't tear down the device handle.
                return false;
            }
            return ParseSonyReport(buf, off, out);
        } else {
            // Generic HID gamepad: use HID parser API.
            return ParseGeneric(buf, bytesRead, out);
        }
    }

    // ── Resolve Sony report byte offsets from device type + report ID ─────────
    // DS4 and DualSense share button-bit meanings but differ in byte layout and
    // USB/Bluetooth framing.  Returns false if the report ID/length isn't a
    // full input report we know how to decode.
    static bool GetSonyOffsets(SonyDeviceType type, const uint8_t* buf, DWORD len, SonyReportOffsets& o) {
        const uint8_t id = buf[0];
        if (type == SonyDeviceType::DS4) {
            if (id == 0x01) {                 // DS4 USB
                o = { 1,2,3,4, 8,9, 5,6,7 };
                return len >= 10;
            }
            if (id == 0x11) {                 // DS4 Bluetooth (2 header bytes 0xC0 0x00 → body +2)
                o = { 3,4,5,6, 10,11, 7,8,9 };
                return len >= 12;
            }
            return false;
        }
        // DualSense / DualSense Edge
        if (id == 0x01) {                     // DualSense USB
            o = { 1,2,3,4, 5,6, 8,9,10 };
            return len >= 11;
        }
        if (id == 0x31) {                     // DualSense Bluetooth (1 header byte → body +1)
            o = { 2,3,4,5, 6,7, 9,10,11 };
            return len >= 12;
        }
        return false;
    }

    // ── Structured Sony DS4 / DualSense parse (USB + Bluetooth) ───────────────
    // Reads sticks, analog triggers, dpad hat, face/shoulder/thumb buttons and
    // the Share/Options/PS/touchpad specials into the normalised state.  Static
    // and dependency-free so it can be unit-tested with synthetic buffers.
    static bool ParseSonyReport(const uint8_t* b, const SonyReportOffsets& o, PhysicalGamepadState& s) {
        s = {};

        // Sticks (DS/DualSense Y grows downward, so invert to XInput's up-positive).
        s.leftX  =  HidAxisToXInput(b[o.lx]);
        s.leftY  = -HidAxisToXInput(b[o.ly]) - 1;
        s.rightX =  HidAxisToXInput(b[o.rx]);
        s.rightY = -HidAxisToXInput(b[o.ry]) - 1;

        // Analog triggers
        s.leftTrigger  = b[o.l2];
        s.rightTrigger = b[o.r2];

        // Button byte 0: dpad hat (low nibble) + face buttons (high nibble)
        uint8_t dpad = b[o.b0] & 0x0F;
        bool du = (dpad == 0 || dpad == 1 || dpad == 7);
        bool dr = (dpad == 1 || dpad == 2 || dpad == 3);
        bool dd = (dpad == 3 || dpad == 4 || dpad == 5);
        bool dl = (dpad == 5 || dpad == 6 || dpad == 7);
        if (du) s.buttons |= XUSB_GAMEPAD_DPAD_UP;
        if (dr) s.buttons |= XUSB_GAMEPAD_DPAD_RIGHT;
        if (dd) s.buttons |= XUSB_GAMEPAD_DPAD_DOWN;
        if (dl) s.buttons |= XUSB_GAMEPAD_DPAD_LEFT;

        if (b[o.b0] & 0x10) s.buttons |= XUSB_GAMEPAD_X;   // Square   → X
        if (b[o.b0] & 0x20) s.buttons |= XUSB_GAMEPAD_A;   // Cross    → A
        if (b[o.b0] & 0x40) s.buttons |= XUSB_GAMEPAD_B;   // Circle   → B
        if (b[o.b0] & 0x80) s.buttons |= XUSB_GAMEPAD_Y;   // Triangle → Y

        // Button byte 1: L1 R1 L2 R2 Share/Create Options L3 R3
        if (b[o.b1] & 0x01) s.buttons |= XUSB_GAMEPAD_LEFT_SHOULDER;
        if (b[o.b1] & 0x02) s.buttons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
        if (b[o.b1] & 0x10) s.buttons |= XUSB_GAMEPAD_BACK;    // Share/Create → Back/View
        if (b[o.b1] & 0x20) s.buttons |= XUSB_GAMEPAD_START;   // Options      → Start/Menu
        if (b[o.b1] & 0x40) s.buttons |= XUSB_GAMEPAD_LEFT_THUMB;
        if (b[o.b1] & 0x80) s.buttons |= XUSB_GAMEPAD_RIGHT_THUMB;

        // Button byte 2: PS (bit0) + touchpad click (bit1)
        s.psButton = (b[o.b2] & 0x01) != 0;
        s.touchpad = (b[o.b2] & 0x02) != 0;

        s.valid = true;
        return true;
    }

private:
    HANDLE               hDev_       = INVALID_HANDLE_VALUE;
    HANDLE               hEvent_     = nullptr;
    const char*          deviceName_ = nullptr;
    bool                 isSony_     = false;
    SonyDeviceType       sonyType_   = SonyDeviceType::DS4;
    PHIDP_PREPARSED_DATA ppd_        = nullptr;
    HIDP_CAPS            hidCaps_    = {};
    // Value caps cached at Open() so the generic-parse hot path never re-queries
    // them (HidP_GetValueCaps + a heap allocation on every poll otherwise).
    std::vector<HIDP_VALUE_CAPS> valueCaps_;

    // ── Enumerate all HID gamepads/joysticks currently attached ──────────────
    // Uses HID usage page / usage to identify game controllers regardless of
    // vendor.  For known Sony VID/PIDs we accept the device even if the usage
    // caps don't say "Gamepad" — the DualSense Edge exposes several HID
    // collections (audio, vendor-specific, touchpad, gamepad) under the same
    // VID/PID; we want the one with Usage==0x05, but we must not reject the
    // device just because we happened to open a non-gamepad collection first.
    static std::vector<HidGamepadCandidate> EnumerateCandidates() {
        std::vector<HidGamepadCandidate> out;

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);

        HDEVINFO devInfo = SetupDiGetClassDevsW(
            &hidGuid, nullptr, nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        );
        if (devInfo == INVALID_HANDLE_VALUE) return out;

        SP_DEVICE_INTERFACE_DATA ifaceData = {};
        ifaceData.cbSize = sizeof(ifaceData);

        for (DWORD i = 0;
             SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifaceData);
             ++i)
        {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &needed, nullptr);
            std::vector<BYTE> detailBuf(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail, needed, nullptr, nullptr))
                continue;

            std::wstring path = detail->DevicePath;

            // Open the device to read its attributes and capabilities.
            // Try read+write first; fall back to read-only (no FILE_FLAG_OVERLAPPED
            // here — this is just a capability query handle, not the I/O handle).
            HANDLE h = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr
            );
            if (h == INVALID_HANDLE_VALUE) {
                h = CreateFileW(
                    path.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr
                );
            }
            if (h == INVALID_HANDLE_VALUE) continue;

            HIDD_ATTRIBUTES attr = {};
            attr.Size = sizeof(attr);
            bool gotAttr = HidD_GetAttributes(h, &attr) == TRUE;
            if (!gotAttr) { CloseHandle(h); continue; }

            // ── Step 1: Check if this is a known Sony device ──────────────────
            // Do this BEFORE the usage-page check.  The DualSense Edge exposes
            // multiple HID interfaces under the same VID/PID; we need to keep
            // every Sony interface in a candidate list and then pick the best
            // one (Usage==Gamepad preferred) rather than dropping the device
            // because we happened to enumerate a non-gamepad collection first.
            const SonyPidEntry* sonyEntry = nullptr;
            for (const auto& e : kSonyControllers) {
                if (attr.VendorID == e.vid && attr.ProductID == e.pid) {
                    sonyEntry = &e;
                    break;
                }
            }

            // ── Step 2: Read HID capabilities ────────────────────────────────
            PHIDP_PREPARSED_DATA ppd = nullptr;
            HIDP_CAPS caps = {};
            bool gotCaps = false;
            if (HidD_GetPreparsedData(h, &ppd) && ppd) {
                gotCaps = (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS);
                HidD_FreePreparsedData(ppd);
            }
            CloseHandle(h);

            // Usage Page 0x01 = Generic Desktop Controls
            // Usage 0x04 = Joystick, 0x05 = Gamepad
            bool isStandardGamepad = gotCaps &&
                                     caps.UsagePage == 0x01 &&
                                     (caps.Usage == 0x04 || caps.Usage == 0x05);

            // Accept the interface if:
            //   (a) it reports itself as a standard gamepad/joystick, OR
            //   (b) it's a known Sony VID/PID (we handle multi-collection selection below)
            if (!isStandardGamepad && !sonyEntry) continue;

            HidGamepadCandidate c;
            c.devicePath = path;
            c.vid        = attr.VendorID;
            c.pid        = attr.ProductID;

            if (sonyEntry) {
                c.isSony       = true;
                c.sonyType     = sonyEntry->type;
                c.friendlyName = sonyEntry->name;
            } else {
                c.friendlyName = "Generic HID Gamepad";
            }

            // Tag each candidate with how "good" it is so we can pick the
            // right collection when the same device has multiple interfaces.
            //   3 = Sony device + gamepad/joystick collection
            //   2 = generic gamepad/joystick collection
            //   1 = Sony device, non-gamepad collection
            if (c.isSony && isStandardGamepad)       c.score = 3;
            else if (!c.isSony && isStandardGamepad) c.score = 2;
            else if (c.isSony)                       c.score = 1;

            // Collapse the multiple HID collections a single physical device may
            // expose (same VID/PID) down to the highest-scoring one, so we end
            // up talking to the gamepad collection rather than, say, the
            // DualSense's audio or vendor-specific interface.
            bool replaced = false;
            for (auto& existing : out) {
                if (existing.vid == c.vid && existing.pid == c.pid) {
                    if (c.score > existing.score) existing = std::move(c);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                out.push_back(std::move(c));
            }
        }

        SetupDiDestroyDeviceInfoList(devInfo);
        return out;
    }

    // ── Generic HID gamepad parse using preparsed data ────────────────────────
    // Maps HID axis usages → XInput axes as best we can, and reads all buttons
    // in order (button 1→A, 2→B, 3→X, 4→Y, 5→LB, 6→RB, 7→Back, 8→Start,
    // 9→LThumb, 10→RThumb).
    bool ParseGeneric(const uint8_t* buf, DWORD len, PhysicalGamepadState& s) const {
        if (!ppd_) return false;
        s = {};

        // ── Axes ───────────────────────────────────────────────────────────────
        // Generic Desktop axis usages:
        //   0x30 = X,  0x31 = Y,  0x32 = Z,  0x33 = Rx, 0x34 = Ry, 0x35 = Rz
        //   0x39 = Hat switch
        struct AxisMap { USAGE usage; int16_t* dest; bool invertY; };
        AxisMap axisMap[] = {
            { 0x30, &s.leftX,  false },
            { 0x31, &s.leftY,  true  },
            { 0x32, nullptr,   false },  // Z — used as left trigger on some pads
            { 0x33, &s.rightX, false },
            { 0x34, &s.rightY, true  },
            { 0x35, nullptr,   false },  // Rz — used as right trigger on some pads
        };

        for (auto& am : axisMap) {
            ULONG logValue = 0;
            NTSTATUS st = HidP_GetUsageValue(
                HidP_Input, 0x01, 0, am.usage,
                &logValue, ppd_,
                reinterpret_cast<PCHAR>(const_cast<uint8_t*>(buf)), len
            );
            if (st != HIDP_STATUS_SUCCESS) continue;

            // Logical min/max come from the value caps cached at Open(), so the
            // hot path does no per-poll re-query or heap allocation.
            LONG logMin = 0, logMax = 255;
            for (const auto& vc : valueCaps_) {
                if (!vc.IsRange && vc.UsagePage == 0x01 && vc.NotRange.Usage == am.usage) {
                    logMin = vc.LogicalMin;
                    logMax = vc.LogicalMax;
                    break;
                }
            }

            int16_t mapped = HidLogicalToXInput(static_cast<LONG>(logValue), logMin, logMax);
            if (am.invertY) mapped = static_cast<int16_t>(-mapped - 1);

            if (am.dest) {
                *am.dest = mapped;
            } else {
                // Z/Rz → triggers (0..255 range from the axis value)
                uint8_t trig = static_cast<uint8_t>((static_cast<int>(logValue) - logMin) * 255 / std::max(1L, logMax - logMin));
                if (am.usage == 0x32) s.leftTrigger  = trig;
                if (am.usage == 0x35) s.rightTrigger = trig;
            }
        }

        // Some gamepads use separate trigger axes for L2/R2 instead of Z/Rz.
        // Usage 0xC4 = Brake, 0xC5 = Accelerator (common on some XInput-style HID pads).
        {
            ULONG v = 0;
            if (HidP_GetUsageValue(HidP_Input, 0x02, 0, 0xC5,
                    &v, ppd_, reinterpret_cast<PCHAR>(const_cast<uint8_t*>(buf)), len)
                == HIDP_STATUS_SUCCESS && s.leftTrigger == 0) {
                s.leftTrigger = static_cast<uint8_t>(v & 0xFF);
            }
            if (HidP_GetUsageValue(HidP_Input, 0x02, 0, 0xC4,
                    &v, ppd_, reinterpret_cast<PCHAR>(const_cast<uint8_t*>(buf)), len)
                == HIDP_STATUS_SUCCESS && s.rightTrigger == 0) {
                s.rightTrigger = static_cast<uint8_t>(v & 0xFF);
            }
        }

        // ── Hat switch → D-Pad ────────────────────────────────────────────────
        {
            ULONG hat = 0;
            NTSTATUS st = HidP_GetUsageValue(
                HidP_Input, 0x01, 0, 0x39,
                &hat, ppd_,
                reinterpret_cast<PCHAR>(const_cast<uint8_t*>(buf)), len
            );
            if (st == HIDP_STATUS_SUCCESS) {
                // Common hat values: 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW; 8+ = centred.
                bool du = (hat == 0 || hat == 1 || hat == 7);
                bool dr = (hat == 1 || hat == 2 || hat == 3);
                bool dd = (hat == 3 || hat == 4 || hat == 5);
                bool dl = (hat == 5 || hat == 6 || hat == 7);
                if (du) s.buttons |= XUSB_GAMEPAD_DPAD_UP;
                if (dr) s.buttons |= XUSB_GAMEPAD_DPAD_RIGHT;
                if (dd) s.buttons |= XUSB_GAMEPAD_DPAD_DOWN;
                if (dl) s.buttons |= XUSB_GAMEPAD_DPAD_LEFT;
            }
        }

        // ── Buttons (up to 32, mapped in XInput order) ────────────────────────
        static const uint16_t kButtonMap[] = {
            XUSB_GAMEPAD_A,
            XUSB_GAMEPAD_B,
            XUSB_GAMEPAD_X,
            XUSB_GAMEPAD_Y,
            XUSB_GAMEPAD_LEFT_SHOULDER,
            XUSB_GAMEPAD_RIGHT_SHOULDER,
            XUSB_GAMEPAD_BACK,
            XUSB_GAMEPAD_START,
            XUSB_GAMEPAD_LEFT_THUMB,
            XUSB_GAMEPAD_RIGHT_THUMB,
            XUSB_GAMEPAD_GUIDE,  // button 11 → Guide (uncommon on generic pads)
        };
        constexpr int kMapCount = sizeof(kButtonMap) / sizeof(kButtonMap[0]);

        USAGE usages[128] = {};
        ULONG usageCount  = 128;
        NTSTATUS bst = HidP_GetUsages(
            HidP_Input, 0x09, 0,
            usages, &usageCount,
            ppd_,
            reinterpret_cast<PCHAR>(const_cast<uint8_t*>(buf)), len
        );
        if (bst == HIDP_STATUS_SUCCESS) {
            for (ULONG j = 0; j < usageCount; j++) {
                int btnIdx = static_cast<int>(usages[j]) - 1;  // HID buttons are 1-based
                if (btnIdx >= 0 && btnIdx < kMapCount) {
                    s.buttons |= kButtonMap[btnIdx];
                }
            }
        }

        s.valid = true;
        return true;
    }
};