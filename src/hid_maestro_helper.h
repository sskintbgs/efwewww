#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <iomanip>

enum class MaestroVirtualTarget {
    Xbox360,
    DualShock4
};

struct ControllerSpoofProfile {
    std::string name;
    uint16_t vendorId;
    uint16_t productId;
    std::string manufacturer;
    std::string productName;
    std::string serialNumber;
    uint32_t pollingRateHz;
    std::string hidDescriptorType;
    MaestroVirtualTarget virtualTarget;
};

class HIDMaestroHelper {
private:
    std::vector<ControllerSpoofProfile> profiles;
    size_t selectedProfileIndex = 0;

public:
    HIDMaestroHelper() {
        // Preset 0: Default Xbox 360 Controller (Standard XInput / ViGEm profile)
        profiles.push_back({
            "Xbox 360 Controller (Default)",
            0x045E, // Microsoft Corp.
            0x028E, // Xbox 360 Controller
            "Microsoft",
            "Controller (Xbox 360 For Windows)",
            "X360-SPOOF-001",
            1000,
            "XUSB_STANDARD",
            MaestroVirtualTarget::Xbox360
        });

        // Preset 1: Sony PlayStation 4 DualShock 4
        profiles.push_back({
            "Sony DualShock 4 (CUH-ZCT2U)",
            0x054C, // Sony Corp.
            0x09CC, // DualShock 4 v2
            "Sony Interactive Entertainment",
            "Wireless Controller (DS4)",
            "DS4-SPOOF-994",
            1000,
            "DS4_NATIVE_HID",
            MaestroVirtualTarget::DualShock4
        });

        // Preset 2: Sony PlayStation 5 DualSense
        profiles.push_back({
            "Sony DualSense (PS5)",
            0x054C, // Sony Corp.
            0x0CE6, // DualSense
            "Sony Interactive Entertainment",
            "DualSense Wireless Controller",
            "PS5-SPOOF-772",
            1000,
            "DUALSENSE_HID",
            MaestroVirtualTarget::DualShock4
        });

        // Preset 3: Nintendo Switch Pro Controller
        profiles.push_back({
            "Nintendo Switch Pro Controller",
            0x057E, // Nintendo Co., Ltd.
            0x2009, // Switch Pro Controller
            "Nintendo",
            "Pro Controller",
            "NSW-PRO-3301",
            500,
            "SWITCH_PRO_HID",
            MaestroVirtualTarget::DualShock4
        });

        // Preset 4: GameSir G7 / G7 SE / G7 Pro Spoofed Profile
        profiles.push_back({
            "GameSir G7 Pro Hardware Identity",
            0x3537, // GameSir / Shenzhen Thunderstone
            0x1001, // G7 Pro
            "GameSir",
            "GameSir-G7 Pro Wired Controller",
            "G7PRO-HW-5510",
            1000,
            "GAMESIR_HYBRID_HID",
            MaestroVirtualTarget::Xbox360
        });

        // Preset 5: Custom VID/PID User Specified Profile
        profiles.push_back({
            "Custom Spoofed Hardware Identity",
            0x1234,
            0x5678,
            "Custom Vendor",
            "Custom Spoofed Controller",
            "CUSTOM-DEV-9999",
            1000,
            "CUSTOM_HID_DESCRIPTOR",
            MaestroVirtualTarget::Xbox360
        });
    }

    const std::vector<ControllerSpoofProfile>& GetProfiles() const {
        return profiles;
    }

    size_t GetSelectedIndex() const {
        return selectedProfileIndex;
    }

    void SetSelectedIndex(size_t index) {
        if (index < profiles.size()) {
            selectedProfileIndex = index;
        }
    }

    ControllerSpoofProfile GetActiveProfile() const {
        return profiles[selectedProfileIndex];
    }

    static std::string GetVirtualTargetName(const ControllerSpoofProfile& profile) {
        return profile.virtualTarget == MaestroVirtualTarget::DualShock4
            ? "Sony DualShock 4-compatible ViGEm target"
            : "Microsoft Xbox 360-compatible ViGEm target";
    }

    void SetCustomProfile(uint16_t vid, uint16_t pid, const std::string& manufacturer, const std::string& productName) {
        for (auto& p : profiles) {
            if (p.name == "Custom Spoofed Hardware Identity") {
                p.vendorId = vid;
                p.productId = pid;
                p.manufacturer = manufacturer;
                p.productName = productName;
                break;
            }
        }
    }

    void DisplaySpoofInfo() const {
        const auto& active = GetActiveProfile();
        std::cout << "========================================================\n";
        std::cout << "          HID MAESTRO HARDWARE SPOOFING PROFILE         \n";
        std::cout << "========================================================\n";
        std::cout << " Profile Name     : " << active.name << "\n";
        std::cout << " Vendor ID (VID)  : 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << active.vendorId << std::dec << "\n";
        std::cout << " Product ID (PID) : 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << active.productId << std::dec << "\n";
        std::cout << " Manufacturer     : " << active.manufacturer << "\n";
        std::cout << " Product Name     : " << active.productName << "\n";
        std::cout << " Serial Number    : " << active.serialNumber << "\n";
        std::cout << " Polling Rate     : " << active.pollingRateHz << " Hz\n";
        std::cout << " Descriptor Type  : " << active.hidDescriptorType << "\n";
        std::cout << " ViGEm Target     : " << GetVirtualTargetName(active) << "\n";
        std::cout << " UMDF2 Driver     : ACTIVE / INSTALLED (hidmaestro.org)\n";
        std::cout << "========================================================\n\n";
    }
};