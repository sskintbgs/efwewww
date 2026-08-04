#pragma once

// passthrough_engine.h
// Headless controller-passthrough engine.  Runs the physical→virtual forwarding
// loop on its own worker thread and publishes a thread-safe live-state snapshot,
// so a GUI (or any other front-end) can drive it without owning the hot loop.
//
// It reuses the same building blocks as the CLI: raw-HID reading (DS4/DualSense
// structured + generic HID), XInput fallback, the report builders, optional
// HidHide cloaking, and a ViGEm virtual target.

#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>

#include "vigem_loader.h"
#include "hid_maestro_helper.h"
#include "hidhide_cloaker.h"
#include "ds4_hid_reader.h"
#include "report_builders.h"
#include "xinput_ex.h"

// Immutable-per-frame snapshot the UI thread reads.  Plain POD so copying it out
// from under the lock is cheap.
struct EngineLiveState {
    bool               running       = false;
    bool               hasInput      = false;
    bool               lastTx        = false;
    unsigned long long packets       = 0;
    unsigned long long fails         = 0;
    double             hz            = 0.0;
    XINPUT_GAMEPAD     gamepad       = {};    // unified snapshot for visualisation
    bool               psButton      = false;
    bool               touchpad      = false;
    bool               cloakApplied  = false;
    int                cloakCount    = 0;
    unsigned int       xinputSlot    = 0;
    bool               usingRawHid   = false;
    char               inputSource[128]  = "-";
    char               cloakMessage[256]  = "";
    char               errorMessage[256]  = "";
};

class PassthroughEngine {
public:
    struct Options {
        bool     hidHideEnabled = true;
        uint32_t pollHz         = 1000;
        DWORD    xinputSlot     = 0;
    };

    PassthroughEngine() { LoadXInputGetStateEx(); }
    ~PassthroughEngine() { Stop(); }

    PassthroughEngine(const PassthroughEngine&)            = delete;
    PassthroughEngine& operator=(const PassthroughEngine&) = delete;

    // Begins forwarding on a worker thread.  `vigem` must stay alive and unused
    // by other threads until Stop() returns.  Returns false if already running
    // or ViGEm isn't connected.
    bool Start(ViGEmLoader& vigem, const ControllerSpoofProfile& profile, const Options& opt) {
        if (running_.load()) return false;
        // A prior worker may have exited asynchronously after publishing an
        // error. It must be joined before assigning a new std::thread.
        if (worker_.joinable()) worker_.join();
        if (!vigem.isLoaded)  return false;
        stop_.store(false);
        pollHz_.store(std::max(125u, std::min(1000u, opt.pollHz)));
        slot_.store(opt.xinputSlot % 4);
        running_.store(true);
        {
            std::lock_guard<std::mutex> l(mtx_);
            state_ = EngineLiveState{};
            state_.running = true;
        }
        worker_ = std::thread(&PassthroughEngine::Run, this, &vigem, profile, opt);
        return true;
    }

    // Signals the worker to stop and joins it (restoring HidHide + removing the
    // virtual target happens on the worker as it unwinds).
    void Stop() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        running_.store(false);
    }

    bool IsRunning() const { return running_.load(); }

    EngineLiveState Snapshot() const {
        std::lock_guard<std::mutex> l(mtx_);
        return state_;
    }

    // Runtime-adjustable while running.
    void SetPollHz(uint32_t hz)     { pollHz_.store(std::max(125u, std::min(1000u, hz))); }
    void SetXInputSlot(DWORD slot)  { slot_.store(slot % 4); }
    uint32_t PollHz() const         { return pollHz_.load(); }
    DWORD    XInputSlot() const     { return slot_.load(); }

private:
    // Busy-wait to targetTime with progressively tighter waits, matching the
    // CLI's precise pacing.  Yields while comfortably early, spins for the last
    // stretch to minimise jitter.
    static void WaitUntilPrecise(std::chrono::steady_clock::time_point targetTime) {
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (now >= targetTime) return;
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now);
            if (remaining > std::chrono::milliseconds(3))       Sleep(1);
            else if (remaining > std::chrono::microseconds(400)) SwitchToThread();
            // else: tight spin
        }
    }

    void publishError(const char* msg) {
        std::lock_guard<std::mutex> l(mtx_);
        std::strncpy(state_.errorMessage, msg, sizeof(state_.errorMessage) - 1);
        state_.running = false;
    }

    void Run(ViGEmLoader* vigem, ControllerSpoofProfile profile, Options opt) {
        const bool isDS4 = (profile.virtualTarget == MaestroVirtualTarget::DualShock4);
        const VIGEM_TARGET_TYPE vtype = isDS4 ? DualShock4Wired : Xbox360Wired;

        // Optional HidHide cloaking (RAII: destructor restores prior state).
        HidHideCloaker cloaker;
        HidHideCloaker::CloakResult cloakResult;
        if (opt.hidHideEnabled) {
            cloakResult = cloaker.Apply();
            std::lock_guard<std::mutex> l(mtx_);
            std::strncpy(state_.cloakMessage, cloakResult.statusMessage.c_str(),
                         sizeof(state_.cloakMessage) - 1);
        }

        // Select the physical raw-HID source before ViGEm creates a device that
        // intentionally looks like a real controller. Otherwise a virtual DS4
        // can win enumeration and the passthrough reads its own output.
        DS4HidReader hidReader;
        const bool useRawHid = hidReader.Open();
        const uint8_t xinputMaskBeforeTarget =
            (!isDS4 && !useRawHid) ? ConnectedXInputMask() : 0;

        ViGEmTargetImpl* target = vigem->CreateTarget(vtype, profile.vendorId, profile.productId);
        if (!target || !vigem->AddTarget(target)) {
            std::string err = "Could not create virtual controller: " + vigem->GetLastErrorText() +
                              " (is ViGEmBus installed?)";
            if (target) vigem->RemoveTarget(target);
            publishError(err.c_str());
            running_.store(false);
            return;
        }

        DWORD ownVirtualXInputSlot = XUSER_MAX_COUNT;
        if (!isDS4 && !useRawHid) {
            // The XUSB PDO can take a moment to acquire its XInput user index.
            // Never poll XInput until our own slot is known unequivocally.
            for (int attempt = 0; attempt < 50 && ownVirtualXInputSlot >= XUSER_MAX_COUNT; ++attempt) {
                DWORD index = XUSER_MAX_COUNT;
                const bool gotIndex = vigem->GetX360UserIndex(target, index);
                const uint8_t currentMask = ConnectedXInputMask();
                if (gotIndex &&
                    IsVerifiedVirtualXInputSlot(index, xinputMaskBeforeTarget, currentMask))
                    ownVirtualXInputSlot = index;
                else
                    Sleep(10);
            }
            if (ownVirtualXInputSlot >= XUSER_MAX_COUNT) {
                publishError("Could not identify the virtual XInput slot; forwarding was stopped to prevent an input feedback loop.");
                vigem->RemoveTarget(target);
                running_.store(false);
                return;
            }
        }

        // Raise timer resolution + THIS worker thread's priority (not the whole
        // process, so the UI thread keeps running smoothly).
        timeBeginPeriod(1);
        const int oldPrio = GetThreadPriority(GetCurrentThread());
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        XUSB_REPORT xOut;   XUSB_REPORT_INIT(&xOut);
        DS4_REPORT  ds4Out; DS4_REPORT_INIT(&ds4Out);

        auto nextPoll   = std::chrono::steady_clock::now();
        auto startTime  = nextPoll;
        unsigned long long packets = 0, fails = 0;
        bool sourceConnected = useRawHid && hidReader.IsOpen();
        bool lastTx = false;
        DWORD activeXInputSlot = slot_.load();
        XINPUT_GAMEPAD lastGamepad{};
        bool lastPsButton = false, lastTouchpad = false;
        bool wasConnected = sourceConnected;
        bool neutralPending = false;
        auto nextHidReconnect = std::chrono::steady_clock::now();
        auto nextNeutralRetry = nextHidReconnect;

        while (!stop_.load(std::memory_order_relaxed)) {
            PhysicalGamepadState hidState;
            bool hasPacket = false;

            if (useRawHid) {
                auto now = std::chrono::steady_clock::now();
                if (!hidReader.IsOpen() && now >= nextHidReconnect) {
                    // ViGEm descendants are filtered during enumeration, so a
                    // replugged/re-paired physical controller can be selected.
                    if (hidReader.Open() && cloaker.IsApplied())
                        cloakResult.devicesCloaked += cloaker.Refresh();
                    nextHidReconnect = now + std::chrono::milliseconds(500);
                }
                if (hidReader.IsOpen()) hasPacket = hidReader.Read(hidState);
                sourceConnected = hidReader.IsOpen();
            } else {
                activeXInputSlot = slot_.load() % XUSER_MAX_COUNT;
                XINPUT_STATE state = {};
                sourceConnected =
                    activeXInputSlot != ownVirtualXInputSlot &&
                    XInputGetStateWithGuide(activeXInputSlot, &state) == ERROR_SUCCESS;
                hasPacket = sourceConnected;
                if (sourceConnected) lastGamepad = state.Gamepad;
            }

            if (hasPacket) {
                bool psButton = false, touchpad = false;
                XINPUT_GAMEPAD gp{};
                if (useRawHid) {
                    gp.sThumbLX      = hidState.leftX;
                    gp.sThumbLY      = hidState.leftY;
                    gp.sThumbRX      = hidState.rightX;
                    gp.sThumbRY      = hidState.rightY;
                    gp.bLeftTrigger  = hidState.leftTrigger;
                    gp.bRightTrigger = hidState.rightTrigger;
                    gp.wButtons      = hidState.buttons;
                    psButton         = hidState.psButton;
                    touchpad         = hidState.touchpad;
                } else {
                    gp       = lastGamepad;
                    psButton = (gp.wButtons & XUSB_GAMEPAD_GUIDE) != 0;
                }

                if (isDS4) {
                    BuildDS4Report(gp, ds4Out, psButton, touchpad);
                    lastTx = vigem->UpdateDS4(target, ds4Out);
                } else {
                    XINPUT_GAMEPAD gx = gp;
                    if (psButton) gx.wButtons |= XUSB_GAMEPAD_GUIDE;
                    if (touchpad) gx.wButtons |= XUSB_GAMEPAD_BACK;
                    BuildX360Report(gx, xOut);
                    lastTx = vigem->UpdateX360(target, xOut);
                    gp = gx;
                }
                lastGamepad = gp;
                lastPsButton = psButton;
                lastTouchpad = touchpad;
                ++packets;
                if (!lastTx) ++fails;
            }

            if (wasConnected && !sourceConnected) {
                neutralPending = true;
                lastGamepad = {};
                lastPsButton = false;
                lastTouchpad = false;
            }
            if (hasPacket) neutralPending = false;

            auto now = std::chrono::steady_clock::now();
            if (neutralPending && now >= nextNeutralRetry) {
                // ViGEm retains the last report indefinitely. Retry a neutral
                // frame until it succeeds so inputs cannot remain held.
                if (isDS4) {
                    BuildDS4Report(lastGamepad, ds4Out);
                    lastTx = vigem->UpdateDS4(target, ds4Out);
                } else {
                    BuildX360Report(lastGamepad, xOut);
                    lastTx = vigem->UpdateX360(target, xOut);
                }
                ++packets;
                if (lastTx) {
                    neutralPending = false;
                } else {
                    ++fails;
                    nextNeutralRetry = now + std::chrono::milliseconds(50);
                }
            }
            wasConnected = sourceConnected;

            {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - startTime).count();
                std::lock_guard<std::mutex> l(mtx_);
                state_.running      = true;
                state_.hasInput     = sourceConnected;
                state_.lastTx       = lastTx;
                state_.packets      = packets;
                state_.fails        = fails;
                state_.hz           = elapsed > 0 ? packets / elapsed : 0.0;
                state_.gamepad      = lastGamepad;
                state_.psButton     = lastPsButton;
                state_.touchpad     = lastTouchpad;
                state_.cloakApplied = cloaker.IsApplied();
                state_.cloakCount   = cloakResult.devicesCloaked;
                state_.xinputSlot   = useRawHid ? slot_.load() : activeXInputSlot;
                state_.usingRawHid  = useRawHid;
                const char* src = useRawHid
                    ? (hidReader.DeviceName() ? hidReader.DeviceName() : "Raw HID (reconnecting)")
                    : (sourceConnected ? "XInput" : "XInput (waiting for physical controller)");
                std::strncpy(state_.inputSource, src ? src : "-", sizeof(state_.inputSource) - 1);
            }

            uint32_t hz = pollHz_.load();
            auto interval = std::chrono::nanoseconds(1000000000ull / (hz ? hz : 1000));
            nextPoll += interval;
            auto now = std::chrono::steady_clock::now();
            if (nextPoll <= now) nextPoll = now + interval;
            WaitUntilPrecise(nextPoll);
        }

        SetThreadPriority(GetCurrentThread(), oldPrio);
        timeEndPeriod(1);
        vigem->RemoveTarget(target);
        // cloaker destructor restores HidHide state here.

        std::lock_guard<std::mutex> l(mtx_);
        state_.running = false;
    }

    std::thread             worker_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       stop_{false};
    std::atomic<uint32_t>   pollHz_{1000};
    std::atomic<DWORD>      slot_{0};
    mutable std::mutex      mtx_;
    EngineLiveState         state_;
};
