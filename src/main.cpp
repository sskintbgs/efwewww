// main.cpp  —  Controller Passthrough with Spoof Profile Picker
// Reads a selectable XInput slot (0–3), forwards to a ViGEm virtual controller
// spoofed as whichever hardware identity the user picks from the menu.
// ─────────────────────────────────────────────────────────────────────────────

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <xinput.h>
#include <iostream>
#include <iomanip>
#include <conio.h>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <mutex>
#include <atomic>

#include "vigem_loader.h"
#include "hid_maestro_helper.h"
#include "hidhide_cloaker.h"
#include "ds4_hid_reader.h"
#include "report_builders.h"   // ConvertAxis / BuildX360Report / BuildDS4Report

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "xinput.lib")

// ─────────────────────────────────────────────────────────────────────────────
//  XInputGetStateEx — undocumented ordinal #100 export from xinput1_3.dll.
//  Identical to XInputGetState but does NOT mask the guide button bit
//  (XUSB_GAMEPAD_GUIDE / 0x0400) out of XINPUT_GAMEPAD::wButtons.
//  We load it manually so the app still links against xinput.lib normally.
// ─────────────────────────────────────────────────────────────────────────────
typedef DWORD(WINAPI* PFN_XInputGetStateEx)(DWORD, XINPUT_STATE*);
static PFN_XInputGetStateEx g_XInputGetStateEx = nullptr;

static void LoadXInputGetStateEx() {
    // xinput1_3.dll is the only version that exposes ordinal 100.
    HMODULE hXInput = LoadLibraryA("xinput1_3.dll");
    if (hXInput) {
        g_XInputGetStateEx = reinterpret_cast<PFN_XInputGetStateEx>(
            GetProcAddress(hXInput, reinterpret_cast<LPCSTR>(100))
        );
    }
    // Deliberately not freeing the module — we hold the reference for the
    // lifetime of the process so the pointer stays valid.
}

// Wrapper: use the Ex variant if available, fall back to the normal one.
static DWORD XInputGetStateWithGuide(DWORD slot, XINPUT_STATE* state) {
    if (g_XInputGetStateEx) return g_XInputGetStateEx(slot, state);
    return XInputGetState(slot, state);
}


static bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

static void ClearScreen() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { 0, 0 };
    DWORD count;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(h, &csbi);
    FillConsoleOutputCharacter(h, ' ', csbi.dwSize.X * csbi.dwSize.Y, coord, &count);
    SetConsoleCursorPosition(h, coord);
}

// Global runtime override for polling rate (0 = use the active profile's default).
static std::atomic<uint32_t> g_pollOverrideHz{0};

// Whether to apply HidHide controller cloaking when passthrough starts.
static std::atomic<bool> g_hidHideEnabled{true};

// XInput slot to read from (0–3).
static std::atomic<DWORD> g_xinputSlot{0};

// Busy-wait until targetTime for precise polling cadence.
// Sleep(1)/SwitchToThread() both have unreliable wakeup latency on Windows
// (can overshoot by a millisecond or more), so we only use them while we're
// comfortably far from the deadline and fall back to a tight spin for the
// last stretch — that's what actually gets jitter down near the ideal cadence.
static void WaitUntilPrecise(std::chrono::steady_clock::time_point targetTime) {
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= targetTime) return;
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now);
        if (remaining > std::chrono::milliseconds(3)) {
            Sleep(1);
        } else if (remaining > std::chrono::microseconds(400)) {
            SwitchToThread();
        }
        // else: tight spin, no yield — minimizes overshoot for the final <400us
    }
}

// Prompts on stdin for a custom polling rate. Returns 0 (meaning "no change")
// if the input is empty or not a valid number.
static uint32_t PromptPollingRateHz() {
    std::cout << "\n Enter polling rate in Hz (125-1000, e.g. 500), or blank to cancel: ";
    std::string line;
    std::getline(std::cin, line);
    if (line.empty()) return 0;
    try {
        int hz = std::stoi(line);
        if (hz < 125) hz = 125;
        if (hz > 1000) hz = 1000;
        return static_cast<uint32_t>(hz);
    } catch (...) {
        return 0;
    }
}

// ConvertAxis / BuildX360Report / BuildDS4Report now live in report_builders.h
// so they can be shared with the unit tests.

// ─────────────────────────────────────────────────────────────────────────────
//  Dashboard state — the hot polling loop only writes a small POD snapshot
//  under a mutex; all console I/O happens on a separate normal-priority
//  thread so it can never add jitter to input forwarding.
// ─────────────────────────────────────────────────────────────────────────────
struct DashboardSnapshot {
    bool hasInput = false;
    bool lastTx = false;
    uint64_t packets = 0;
    uint64_t fails = 0;
    double hz = 0.0;
    XINPUT_GAMEPAD gamepad{};
    // HidHide cloaking status shown in the dashboard header
    bool  cloakApplied  = false;
    int   cloakCount    = 0;
    DWORD xinputSlot    = 0;
    // Input source for display
    const char* inputSource = "XInput";
};

static std::mutex        g_dashMutex;
static DashboardSnapshot g_dashSnapshot;
static std::atomic<bool> g_dashRunning{false};

static void RenderDashboard(const ControllerSpoofProfile& profile, bool isDS4) {
    DashboardSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(g_dashMutex);
        snap = g_dashSnapshot;
    }

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleCursorPosition(hOut, { 0, 0 });

    std::cout
        << "======================================================================\n"
        << "       CONTROLLER PASSTHROUGH  -  SLOT " << snap.xinputSlot << " (PHYSICAL)\n"
        << "======================================================================\n"
        << " Profile  : " << profile.name << "                              \n"
        << " Identity : " << profile.productName
        << "  VID=0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << profile.vendorId
        << " PID=0x"  << std::setw(4) << profile.productId << std::dec << "  \n"
        << " Type     : " << (isDS4 ? "DualShock 4 (DS4)" : "Xbox 360") << "                              \n"
        << " Rate     : " << std::fixed << std::setprecision(1) << snap.hz
        << " Hz  |  Packets: " << snap.packets << "  Failed: " << snap.fails << "              \n"
        << " Status   : " << (snap.hasInput ? (snap.lastTx ? "[TX OK]" : "[TX ERR]") : "[WAITING FOR CONTROLLER]") << "              \n"
        << " Input    : " << snap.inputSource << "                              \n"
        << " Cloaking : " << (snap.cloakApplied
                              ? ("[ACTIVE — " + std::to_string(snap.cloakCount) + " physical device(s) hidden]")
                              : "[OFF — physical controller visible to all apps]")
        << "              \n"
        << "======================================================================\n\n";

    if (snap.hasInput) {
        auto& gp = snap.gamepad;
        std::cout
            << " Left  Stick : X=" << std::setw(6) << gp.sThumbLX << "  Y=" << std::setw(6) << gp.sThumbLY << "              \n"
            << " Right Stick : X=" << std::setw(6) << gp.sThumbRX << "  Y=" << std::setw(6) << gp.sThumbRY << "              \n"
            << " Triggers    : L=" << std::setw(3) << (int)gp.bLeftTrigger << "  R=" << std::setw(3) << (int)gp.bRightTrigger << "              \n"
            << " Buttons     :"
            << (gp.wButtons & XUSB_GAMEPAD_A              ? " A"  : "  ")
            << (gp.wButtons & XUSB_GAMEPAD_B              ? " B"  : "  ")
            << (gp.wButtons & XUSB_GAMEPAD_X              ? " X"  : "  ")
            << (gp.wButtons & XUSB_GAMEPAD_Y              ? " Y"  : "  ")
            << (gp.wButtons & XUSB_GAMEPAD_LEFT_SHOULDER  ? " LB" : "   ")
            << (gp.wButtons & XUSB_GAMEPAD_RIGHT_SHOULDER ? " RB" : "   ")
            << (gp.wButtons & XUSB_GAMEPAD_START          ? " ST" : "   ")
            << (gp.wButtons & XUSB_GAMEPAD_BACK           ? " BK" : "   ")
            << "              \n"
            << " D-Pad       :"
            << (gp.wButtons & XUSB_GAMEPAD_DPAD_UP    ? " U" : "  ")
            << (gp.wButtons & XUSB_GAMEPAD_DPAD_DOWN  ? " D" : "  ")
            << (gp.wButtons & XUSB_GAMEPAD_DPAD_LEFT  ? " L" : "  ")
            << (gp.wButtons & XUSB_GAMEPAD_DPAD_RIGHT ? " R" : "  ")
            << "                                                    \n\n";
    } else {
        std::cout << " [No controller detected — plug in your controller\n"
                  << "  (HID path: scanning all gamepads; XInput slot " << snap.xinputSlot << " also monitored)]\n\n";
    }

    std::cout << " Press [ESC] or [Q] to stop and return to menu\n";
}

static void DashboardThreadFunc(const ControllerSpoofProfile* profile, bool isDS4) {
    while (g_dashRunning.load(std::memory_order_relaxed)) {
        RenderDashboard(*profile, isDS4);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Passthrough loop
// ─────────────────────────────────────────────────────────────────────────────
static void RunPassthrough(ViGEmLoader& vigem, const ControllerSpoofProfile& profile) {
    bool isDS4 = (profile.virtualTarget == MaestroVirtualTarget::DualShock4);
    VIGEM_TARGET_TYPE vtype = isDS4 ? DualShock4Wired : Xbox360Wired;

    // ── HidHide cloaking ─────────────────────────────────────────────────
    // Apply before creating the virtual target so the physical controller
    // is already hidden by the time games enumerate devices.
    HidHideCloaker cloaker;
    HidHideCloaker::CloakResult cloakResult;
    if (g_hidHideEnabled.load()) {
        cloakResult = cloaker.Apply();
        if (!cloakResult.hidhideAvailable) {
            std::cout << "\n [HidHide] " << cloakResult.statusMessage << "\n"
                      << " Continuing without cloaking — press any key...\n";
            _getch();
        } else {
            std::cout << "\n [HidHide] " << cloakResult.statusMessage << "\n";
        }
    }

    // Capture the physical HID interface before the virtual target exists, so
    // a ViGEm DS4 cannot be selected as this passthrough's own input.
    DS4HidReader hidReader;
    const bool useRawHid = hidReader.Open();
    const char* inputSource = useRawHid
        ? hidReader.DeviceName()
        : "XInput (waiting for physical controller)";

    ViGEmTargetImpl* target = vigem.CreateTarget(vtype, profile.vendorId, profile.productId);
    if (!target || !vigem.AddTarget(target)) {
        std::cout << "\n [ERROR] Could not create virtual controller: "
                  << vigem.GetLastErrorText() << "\n"
                  << " Make sure ViGEmBus is installed (run setup_drivers.bat as Admin).\n"
                  << " Press any key to return...\n";
        if (target) vigem.RemoveTarget(target);
        // cloaker destructor restores HidHide state automatically
        _getch();
        return;
    }

    DWORD ownVirtualXInputSlot = XUSER_MAX_COUNT;
    if (!isDS4) {
        vigem.GetX360UserIndex(target, ownVirtualXInputSlot);
    }

    // Raise timer resolution and both process + thread priority for the
    // tightest possible polling cadence. REALTIME_PRIORITY_CLASS + TIME_CRITICAL
    // is a meaningful step up from HIGHEST; it's safe here because the loop
    // spends nearly all its time blocked in XInputGetState or WaitUntilPrecise
    // rather than burning CPU, so it won't meaningfully starve the rest of the system.
    timeBeginPeriod(1);
    int oldPrio = GetThreadPriority(GetCurrentThread());
    DWORD oldPriorityClass = GetPriorityClass(GetCurrentProcess());
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint32_t configuredHz = g_pollOverrideHz.load() != 0 ? g_pollOverrideHz.load() : profile.pollingRateHz;
    uint32_t targetHz = std::max(125u, std::min(1000u, configuredHz));
    auto pollInterval = std::chrono::nanoseconds(1000000000ull / targetHz);
    auto nextPoll     = std::chrono::steady_clock::now();

    uint64_t polls = 0, packets = 0, fails = 0;
    auto startTime = std::chrono::steady_clock::now();

    XUSB_REPORT xOut;   XUSB_REPORT_INIT(&xOut);
    DS4_REPORT  ds4Out; DS4_REPORT_INIT(&ds4Out);
    bool lastTx = false;
    bool sourceConnected = useRawHid && hidReader.IsOpen();
    DWORD activeXInputSlot = g_xinputSlot.load();
    XINPUT_GAMEPAD lastGamepad{};

    ClearScreen();

    // Rendering happens on its own normal-priority thread so console I/O
    // never blocks — or adds jitter to — the time-critical polling loop below.
    g_dashRunning.store(true);
    std::thread dashThread(DashboardThreadFunc, &profile, isDS4);

    while (true) {
        // ESC or Q → quit
        if (_kbhit()) {
            char k = _getch();
            if (k == 27 || k == 'q' || k == 'Q') break;
        }

        // Poll physical controller — raw HID path for DS4/DualSense,
        // XInput path for everything else.
        bool hasPacket = false;
        PhysicalGamepadState hidState;

        if (useRawHid) {
            if (!hidReader.IsOpen()) hidReader.Open(true);
            if (hidReader.IsOpen()) hasPacket = hidReader.Read(hidState);
            sourceConnected = hidReader.IsOpen();
            inputSource = hidReader.DeviceName()
                ? hidReader.DeviceName()
                : "Raw HID (reconnecting)";
        } else {
            XINPUT_STATE states[XUSER_MAX_COUNT] = {};
            uint8_t connectedMask = 0;
            for (DWORD candidate = 0; candidate < XUSER_MAX_COUNT; ++candidate) {
                if (XInputGetStateWithGuide(candidate, &states[candidate]) == ERROR_SUCCESS)
                    connectedMask |= static_cast<uint8_t>(1u << candidate);
            }

            activeXInputSlot = SelectPhysicalXInputSlot(
                g_xinputSlot.load(), ownVirtualXInputSlot, connectedMask);
            sourceConnected = activeXInputSlot < XUSER_MAX_COUNT;
            hasPacket = sourceConnected;
            if (sourceConnected) lastGamepad = states[activeXInputSlot].Gamepad;
            inputSource = sourceConnected
                ? "XInput"
                : "XInput (waiting for physical controller)";
        }
        polls++;

        if (hasPacket) {
            // Unify into an XINPUT_GAMEPAD-shaped struct plus the two DS4-only
            // buttons (PS home + touchpad click) that have no XInput bit, so we
            // can forward them without clobbering Share/Back.
            XINPUT_GAMEPAD gp{};
            bool psButton = false;
            bool touchpad = false;
            if (useRawHid) {
                gp.sThumbLX      = hidState.leftX;
                gp.sThumbLY      = hidState.leftY;
                gp.sThumbRX      = hidState.rightX;
                gp.sThumbRY      = hidState.rightY;
                gp.bLeftTrigger  = hidState.leftTrigger;
                gp.bRightTrigger = hidState.rightTrigger;
                gp.wButtons      = hidState.buttons;   // Share→BACK, Options→START already mapped
                psButton         = hidState.psButton;
                touchpad         = hidState.touchpad;
            } else {
                gp       = lastGamepad;
                psButton = (gp.wButtons & XUSB_GAMEPAD_GUIDE) != 0;   // Guide/Home → PS
            }

            if (isDS4) {
                // Forward PS + touchpad click faithfully to the virtual DS4.
                BuildDS4Report(gp, ds4Out, psButton, touchpad);
                lastTx = vigem.UpdateDS4(target, ds4Out);
            } else {
                // Xbox 360 target has no home/touchpad distinction: fold the PS
                // button onto Guide and a touchpad click onto Back.
                XINPUT_GAMEPAD gx = gp;
                if (psButton) gx.wButtons |= XUSB_GAMEPAD_GUIDE;
                if (touchpad) gx.wButtons |= XUSB_GAMEPAD_BACK;
                BuildX360Report(gx, xOut);
                lastTx = vigem.UpdateX360(target, xOut);
                gp = gx;   // reflect folded buttons in the dashboard snapshot
            }
            packets++;
            if (!lastTx) fails++;
            lastGamepad = gp;
        }

        // Publish a snapshot for the dashboard thread. This is just an
        // uncontended mutex lock + POD copy (no I/O), so it costs on the
        // order of tens of nanoseconds instead of the hundreds of
        // microseconds console output used to cost on this hot path.
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            std::lock_guard<std::mutex> lock(g_dashMutex);
            g_dashSnapshot.hasInput     = sourceConnected;
            g_dashSnapshot.lastTx       = lastTx;
            g_dashSnapshot.packets      = packets;
            g_dashSnapshot.fails        = fails;
            g_dashSnapshot.hz           = elapsed > 0 ? packets / elapsed : 0.0;
            g_dashSnapshot.cloakApplied = cloaker.IsApplied();
            g_dashSnapshot.cloakCount   = cloakResult.devicesCloaked;
            g_dashSnapshot.xinputSlot   = useRawHid ? g_xinputSlot.load() : activeXInputSlot;
            g_dashSnapshot.inputSource  = inputSource;
            g_dashSnapshot.gamepad      = lastGamepad;
        }

        nextPoll += pollInterval;
        if (nextPoll <= std::chrono::steady_clock::now())
            nextPoll = std::chrono::steady_clock::now() + pollInterval;
        WaitUntilPrecise(nextPoll);
    }

    g_dashRunning.store(false);
    dashThread.join();

    SetThreadPriority(GetCurrentThread(), oldPrio);
    SetPriorityClass(GetCurrentProcess(), oldPriorityClass);
    timeEndPeriod(1);
    vigem.RemoveTarget(target);

    ClearScreen();
    std::cout << "\n Passthrough stopped. Press any key...\n";
    _getch();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Menu
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    // Set both input and output codepages to UTF-8 so that Unicode characters
    // (em-dashes, box-drawing chars, etc.) render correctly instead of
    // appearing as garbage like "ΓÇö" (Windows-1252 misread of U+2014).
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    SetConsoleTitleA("Controller Passthrough — Spoof Profile Picker");
    LoadXInputGetStateEx();

    // Auto-elevate if not admin
    if (!IsRunAsAdmin()) {
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        SHELLEXECUTEINFOA sei = { sizeof(sei) };
        sei.lpVerb = "runas";
        sei.lpFile = path;
        sei.nShow  = SW_NORMAL;
        if (ShellExecuteExA(&sei)) return 0;
        // If elevation was declined just continue in user mode
    }

    ViGEmLoader      vigem;
    HIDMaestroHelper maestro;

    bool running = true;
    while (running) {
        ClearScreen();

        const auto& profiles = maestro.GetProfiles();
        size_t      sel      = maestro.GetSelectedIndex();

        std::cout
            << "======================================================================\n"
            << "         CONTROLLER PASSTHROUGH  —  SPOOF PROFILE PICKER\n"
            << "======================================================================\n"
            << " ViGEmBus : " << (vigem.isLoaded ? "[ONLINE]" : "[OFFLINE — run setup_drivers.bat]") << "              \n"
            << "======================================================================\n\n"
            << " Choose a controller to spoof (virtual device appears on slot 0):\n\n";

        for (size_t i = 0; i < profiles.size(); i++) {
            const auto& p = profiles[i];
            std::cout
                << " [" << (i + 1) << "] "
                << (i == sel ? "* " : "  ")
                << p.name
                << "  (VID=0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << p.vendorId
                << " PID=0x"   << std::setw(4) << p.productId << std::dec
                << ", default " << p.pollingRateHz << " Hz)\n";
        }

        // Show which XInput slots currently have a controller connected.
        // Use the same XInputGetStateWithGuide wrapper as the passthrough loop
        // so results are consistent.  Mark the active slot with an arrow.
        uint32_t effectiveHz = g_pollOverrideHz.load() != 0 ? g_pollOverrideHz.load() : profiles[sel].pollingRateHz;
        DWORD activeSlot = g_xinputSlot.load();
        std::cout << "\n [S] Start  [R] Polling Rate  [C] XInput Slot  [H] HidHide  [0] Exit\n\n"
                  << " Active        : " << profiles[sel].name << "\n"
                  << " Polling Rate  : " << effectiveHz << " Hz"
                  << (g_pollOverrideHz.load() != 0 ? "  (manual override)" : "  (profile default)") << "\n"
                  << " XInput Slot   :\n";
        for (DWORD i = 0; i < 4; i++) {
            XINPUT_STATE xs{};
            bool connected = (XInputGetStateWithGuide(i, &xs) == ERROR_SUCCESS);
            std::cout << (i == activeSlot ? "   --> " : "       ")
                      << "Slot " << i << " : " << (connected ? "[CONNECTED]" : "[no controller]") << "\n";
        }
        std::cout
            << " HidHide Cloak : " << (g_hidHideEnabled.load() ? "[ENABLED]  — physical controller will be hidden from games" : "[DISABLED] — physical controller will remain visible") << "\n"
            << "======================================================================\n"
            << " Enter choice: ";

        char c = _getch();
        std::cout << c << "\n";

        if (c == '0') {
            running = false;
        } else if (c == 's' || c == 'S') {
            if (!vigem.isLoaded) {
                std::cout << "\n [ERROR] ViGEmBus not available. Run setup_drivers.bat first.\n"
                          << " Press any key...\n";
                _getch();
            } else {
                RunPassthrough(vigem, maestro.GetActiveProfile());
            }
        } else if (c == 'r' || c == 'R') {
            uint32_t hz = PromptPollingRateHz();
            if (hz != 0) g_pollOverrideHz.store(hz);
        } else if (c == 'c' || c == 'C') {
            g_xinputSlot.store((g_xinputSlot.load() + 1) % 4);
        } else if (c == 'h' || c == 'H') {
            g_hidHideEnabled.store(!g_hidHideEnabled.load());
        } else {
            int idx = c - '1';
            if (idx >= 0 && idx < (int)profiles.size()) {
                maestro.SetSelectedIndex((size_t)idx);
            }
        }
    }

    std::cout << "\n Goodbye!\n";
    return 0;
}