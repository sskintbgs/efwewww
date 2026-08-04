#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <cwctype>
#include <string>

// ViGEm's child HID interfaces intentionally use genuine-looking controller
// VID/PIDs. The parent device chain is the reliable distinction.
static inline bool HidDeviceInstanceIsViGEm(std::wstring instanceId) {
    std::transform(instanceId.begin(), instanceId.end(), instanceId.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return instanceId.find(L"VIGEMBUS") != std::wstring::npos;
}

static inline bool HidDeviceTreeIsViGEm(DEVINST device) {
    DEVINST current = device;
    for (int depth = 0; depth < 8; ++depth) {
        wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(current, instanceId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
            HidDeviceInstanceIsViGEm(instanceId)) {
            return true;
        }

        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS) break;
        current = parent;
    }
    return false;
}
