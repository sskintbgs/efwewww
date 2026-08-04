#pragma once

// xinput_ex.h
// Loads XInputGetStateEx (undocumented ordinal #100 from xinput1_3.dll), which
// behaves like XInputGetState but does NOT mask out the guide-button bit
// (XUSB_GAMEPAD_GUIDE / 0x0400).  Falls back to the normal XInputGetState when
// the Ex export isn't available.

#include <windows.h>
#include <xinput.h>
#include <cstdint>

typedef DWORD(WINAPI* PFN_XInputGetStateEx)(DWORD, XINPUT_STATE*);

// C++17 inline variable → a single shared instance across all translation units.
inline PFN_XInputGetStateEx g_XInputGetStateEx = nullptr;

inline void LoadXInputGetStateEx() {
    if (g_XInputGetStateEx) return;
    // xinput1_3.dll is the version that exposes ordinal 100.  We deliberately
    // keep the module loaded for the lifetime of the process.
    HMODULE hXInput = LoadLibraryA("xinput1_3.dll");
    if (hXInput) {
        g_XInputGetStateEx = reinterpret_cast<PFN_XInputGetStateEx>(
            GetProcAddress(hXInput, reinterpret_cast<LPCSTR>(100)));
    }
}

inline DWORD XInputGetStateWithGuide(DWORD slot, XINPUT_STATE* state) {
    if (g_XInputGetStateEx) return g_XInputGetStateEx(slot, state);
    return XInputGetState(slot, state);
}

static inline uint8_t ConnectedXInputMask() {
    uint8_t mask = 0;
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        XINPUT_STATE state = {};
        if (XInputGetStateWithGuide(slot, &state) == ERROR_SUCCESS)
            mask |= static_cast<uint8_t>(1u << slot);
    }
    return mask;
}

static inline bool IsVerifiedVirtualXInputSlot(DWORD candidate,
                                               uint8_t beforeTarget,
                                               uint8_t afterTarget) {
    if (candidate >= XUSER_MAX_COUNT) return false;
    const uint8_t bit = static_cast<uint8_t>(1u << candidate);
    return (beforeTarget & bit) == 0 && (afterTarget & bit) != 0;
}

// Pick a connected physical slot without ever feeding this application's own
// ViGEm X360 target back into itself. XUSER_MAX_COUNT is the "none" sentinel.
static inline DWORD SelectPhysicalXInputSlot(DWORD preferredSlot,
                                             DWORD ownVirtualSlot,
                                             uint8_t connectedMask) {
    preferredSlot %= XUSER_MAX_COUNT;
    auto available = [&](DWORD slot) {
        return slot != ownVirtualSlot && (connectedMask & (1u << slot)) != 0;
    };

    if (available(preferredSlot)) return preferredSlot;
    return XUSER_MAX_COUNT;
}
