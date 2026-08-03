#pragma once

// xinput_ex.h
// Loads XInputGetStateEx (undocumented ordinal #100 from xinput1_3.dll), which
// behaves like XInputGetState but does NOT mask out the guide-button bit
// (XUSB_GAMEPAD_GUIDE / 0x0400).  Falls back to the normal XInputGetState when
// the Ex export isn't available.

#include <windows.h>
#include <xinput.h>

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
