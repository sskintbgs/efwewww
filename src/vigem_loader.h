#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "third_party/ViGEmClient/include/ViGEm/Client.h"

// Compatibility aliases for the app's original DS4 hat names.
#define DS4_DPAD_HAT_NONE      DS4_BUTTON_DPAD_NONE
#define DS4_DPAD_HAT_NORTH     DS4_BUTTON_DPAD_NORTH
#define DS4_DPAD_HAT_NORTHEAST DS4_BUTTON_DPAD_NORTHEAST
#define DS4_DPAD_HAT_EAST      DS4_BUTTON_DPAD_EAST
#define DS4_DPAD_HAT_SOUTHEAST DS4_BUTTON_DPAD_SOUTHEAST
#define DS4_DPAD_HAT_SOUTH     DS4_BUTTON_DPAD_SOUTH
#define DS4_DPAD_HAT_SOUTHWEST DS4_BUTTON_DPAD_SOUTHWEST
#define DS4_DPAD_HAT_WEST      DS4_BUTTON_DPAD_WEST
#define DS4_DPAD_HAT_NORTHWEST DS4_BUTTON_DPAD_NORTHWEST

struct ViGEmTargetImpl {
    PVIGEM_TARGET target = nullptr;
    VIGEM_TARGET_TYPE type = Xbox360Wired;
    ULONG serialNo = 0;
    bool attached = false;
};

class ViGEmLoader {
private:
    PVIGEM_CLIENT client = nullptr;
    VIGEM_ERROR lastError = VIGEM_ERROR_NONE;

    static const char* ErrorName(VIGEM_ERROR error) {
        switch (error) {
        case VIGEM_ERROR_NONE: return "VIGEM_ERROR_NONE";
        case VIGEM_ERROR_BUS_NOT_FOUND: return "VIGEM_ERROR_BUS_NOT_FOUND";
        case VIGEM_ERROR_NO_FREE_SLOT: return "VIGEM_ERROR_NO_FREE_SLOT";
        case VIGEM_ERROR_INVALID_TARGET: return "VIGEM_ERROR_INVALID_TARGET";
        case VIGEM_ERROR_REMOVAL_FAILED: return "VIGEM_ERROR_REMOVAL_FAILED";
        case VIGEM_ERROR_ALREADY_CONNECTED: return "VIGEM_ERROR_ALREADY_CONNECTED";
        case VIGEM_ERROR_TARGET_UNINITIALIZED: return "VIGEM_ERROR_TARGET_UNINITIALIZED";
        case VIGEM_ERROR_TARGET_NOT_PLUGGED_IN: return "VIGEM_ERROR_TARGET_NOT_PLUGGED_IN";
        case VIGEM_ERROR_BUS_VERSION_MISMATCH: return "VIGEM_ERROR_BUS_VERSION_MISMATCH";
        case VIGEM_ERROR_BUS_ACCESS_FAILED: return "VIGEM_ERROR_BUS_ACCESS_FAILED";
        case VIGEM_ERROR_BUS_ALREADY_CONNECTED: return "VIGEM_ERROR_BUS_ALREADY_CONNECTED";
        case VIGEM_ERROR_BUS_INVALID_HANDLE: return "VIGEM_ERROR_BUS_INVALID_HANDLE";
        case VIGEM_ERROR_INVALID_PARAMETER: return "VIGEM_ERROR_INVALID_PARAMETER";
        case VIGEM_ERROR_WINAPI: return "VIGEM_ERROR_WINAPI";
        default: return "VIGEM_ERROR_UNKNOWN";
        }
    }

public:
    bool isLoaded = false;

    ViGEmLoader() {
        Reconnect();
    }

    ~ViGEmLoader() {
        Disconnect();
    }

    bool Reconnect() {
        Disconnect();

        client = vigem_alloc();
        if (!client) {
            lastError = VIGEM_ERROR_BUS_INVALID_HANDLE;
            isLoaded = false;
            return false;
        }

        lastError = vigem_connect(client);
        isLoaded = VIGEM_SUCCESS(lastError);
        return isLoaded;
    }

    void Disconnect() {
        if (client) {
            vigem_disconnect(client);
            vigem_free(client);
            client = nullptr;
        }
        isLoaded = false;
    }

    std::string GetLastErrorText() const {
        std::ostringstream ss;
        ss << ErrorName(lastError) << " (0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(lastError) << ")";
        return ss.str();
    }

    ViGEmTargetImpl* CreateTarget(VIGEM_TARGET_TYPE type, USHORT vid = 0, USHORT pid = 0) {
        auto wrapper = new ViGEmTargetImpl();
        wrapper->type = type;
        wrapper->target = (type == DualShock4Wired)
            ? vigem_target_ds4_alloc()
            : vigem_target_x360_alloc();

        if (!wrapper->target) {
            delete wrapper;
            lastError = VIGEM_ERROR_INVALID_TARGET;
            return nullptr;
        }

        if (vid != 0) {
            vigem_target_set_vid(wrapper->target, vid);
        }
        if (pid != 0) {
            vigem_target_set_pid(wrapper->target, pid);
        }

        return wrapper;
    }

    bool AddTarget(ViGEmTargetImpl* wrapper) {
        if (!isLoaded || !client || !wrapper || !wrapper->target) {
            lastError = VIGEM_ERROR_INVALID_TARGET;
            return false;
        }

        lastError = vigem_target_add(client, wrapper->target);
        if (!VIGEM_SUCCESS(lastError)) {
            return false;
        }

        wrapper->attached = true;
        wrapper->serialNo = vigem_target_get_index(wrapper->target);
        return true;
    }

    bool GetX360UserIndex(ViGEmTargetImpl* wrapper, DWORD& index) {
        if (!isLoaded || !client || !wrapper || !wrapper->target ||
            !wrapper->attached || wrapper->type != Xbox360Wired) {
            lastError = VIGEM_ERROR_INVALID_TARGET;
            return false;
        }

        ULONG userIndex = XUSER_MAX_COUNT;
        lastError = vigem_target_x360_get_user_index(client, wrapper->target, &userIndex);
        if (!VIGEM_SUCCESS(lastError)) return false;
        index = static_cast<DWORD>(userIndex);
        return true;
    }

    bool RemoveTarget(ViGEmTargetImpl* wrapper) {
        if (!wrapper) {
            lastError = VIGEM_ERROR_INVALID_TARGET;
            return false;
        }

        bool ok = true;
        if (client && wrapper->target && wrapper->attached) {
            lastError = vigem_target_remove(client, wrapper->target);
            ok = VIGEM_SUCCESS(lastError);
        }

        if (wrapper->target) {
            vigem_target_free(wrapper->target);
        }

        delete wrapper;
        return ok;
    }

    bool UpdateX360(ViGEmTargetImpl* wrapper, const XUSB_REPORT& report) {
        if (!isLoaded || !client || !wrapper || !wrapper->target) {
            lastError = VIGEM_ERROR_INVALID_TARGET;
            return false;
        }

        lastError = vigem_target_x360_update(client, wrapper->target, report);
        return VIGEM_SUCCESS(lastError);
    }

    bool UpdateDS4(ViGEmTargetImpl* wrapper, const DS4_REPORT& report) {
        if (!isLoaded || !client || !wrapper || !wrapper->target) {
            lastError = VIGEM_ERROR_INVALID_TARGET;
            return false;
        }

        lastError = vigem_target_ds4_update(client, wrapper->target, report);
        return VIGEM_SUCCESS(lastError);
    }
};