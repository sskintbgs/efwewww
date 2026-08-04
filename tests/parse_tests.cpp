// parse_tests.cpp
// Host-runnable unit tests for the pure parsing / conversion logic that the
// controller passthrough relies on.  These functions do no device I/O, so the
// test executable can be cross-compiled with MinGW and run under Wine with
// synthetic HID report buffers — giving real runtime evidence for the fixes to
// axis conversion, DS4/DualSense report decoding (USB + Bluetooth) and the
// XInput→DS4 report mapping.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>

#include "ds4_hid_reader.h"     // HidAxisToXInput, SonyReportOffsets, DS4HidReader::ParseSonyReport/GetSonyOffsets
#include "report_builders.h"    // ConvertAxis, BuildX360Report, BuildDS4Report
#include "hidhide_cloaker.h"    // HidHideComposeDosDevicePath
#include "xinput_ex.h"           // SelectPhysicalXInputSlot

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("  [FAIL] %s\n", what); }
}
static void check_eq(long long got, long long want, const char* what) {
    bool ok = (got == want);
    if (ok) { ++g_pass; std::printf("  [PASS] %s (=%lld)\n", what, got); }
    else    { ++g_fail; std::printf("  [FAIL] %s (got %lld, want %lld)\n", what, got, want); }
}

// Convenience: build a PhysicalGamepadState from a Sony buffer + explicit offsets.
static PhysicalGamepadState parseSony(const uint8_t* buf, const SonyReportOffsets& off) {
    PhysicalGamepadState s;
    DS4HidReader::ParseSonyReport(buf, off, s);
    return s;
}

int main() {
    std::printf("== Axis conversion ==\n");
    // HidAxisToXInput must NOT overflow: byte 0 (fully one direction) has to map
    // to the negative extreme, not wrap to a large positive value.
    check_eq(HidAxisToXInput(0),   -32768, "HidAxisToXInput(0) = -32768 (no overflow)");
    check(HidAxisToXInput(0)  < 0,          "HidAxisToXInput(0) is negative (sign correct)");
    check_eq(HidAxisToXInput(128), 0,       "HidAxisToXInput(128) = 0 (centre)");
    check_eq(HidAxisToXInput(255), 32639,   "HidAxisToXInput(255) ~= +max");
    check(HidAxisToXInput(64)  < 0,         "HidAxisToXInput(64) is negative");

    // ConvertAxis: XInput -> DS4 byte, centre must land on 128.
    check_eq(ConvertAxis(0),       128, "ConvertAxis(0) = 128 (centre)");
    check_eq(ConvertAxis(32767),   255, "ConvertAxis(+max) = 255");
    check_eq(ConvertAxis(-32768),  0,   "ConvertAxis(-max) = 0");

    std::printf("\n== Generic HID descriptor helpers ==\n");
    {
        HIDP_VALUE_CAPS ranged = {};
        ranged.UsagePage = 0x01;
        ranged.IsRange = TRUE;
        ranged.Range.UsageMin = 0x30;
        ranged.Range.UsageMax = 0x35;
        check(HidValueCapsContainsUsage(ranged, 0x01, 0x30),
              "ranged value cap contains first axis usage");
        check(HidValueCapsContainsUsage(ranged, 0x01, 0x34),
              "ranged value cap contains interior axis usage");
        check(!HidValueCapsContainsUsage(ranged, 0x01, 0x39),
              "ranged value cap rejects unrelated hat usage");
        ranged.ReportID = 2;
        check(HidValueCapsAppliesToReport(ranged, 2),
              "value cap applies to its report ID");
        check(!HidValueCapsAppliesToReport(ranged, 1),
              "value cap rejects a different report ID");

        HIDP_VALUE_CAPS single = {};
        single.UsagePage = 0x01;
        single.IsRange = FALSE;
        single.NotRange.Usage = 0x39;
        single.ReportID = 0;
        check(HidValueCapsContainsUsage(single, 0x01, 0x39),
              "single value cap contains exact usage");
        check(HidValueCapsAppliesToReport(single, 7),
              "unnumbered value cap applies without reading buf[0] as an ID");
    }

    std::printf("\n== Generic HID hat normalization ==\n");
    {
        // Many descriptors encode directions as 1..8 and use 0 as the null
        // (centred) value. Treating the raw value as a zero-based direction
        // turns that centred 0 into a permanently pressed D-pad Up.
        check_eq(HidHatToXInputButtons(0, 1, 8), 0,
                 "1-based hat null value 0 -> centred");
        check_eq(HidHatToXInputButtons(1, 1, 8), XUSB_GAMEPAD_DPAD_UP,
                 "1-based hat value 1 -> up");
        check_eq(HidHatToXInputButtons(2, 1, 8),
                 XUSB_GAMEPAD_DPAD_UP | XUSB_GAMEPAD_DPAD_RIGHT,
                 "1-based hat value 2 -> up-right");
        check_eq(HidHatToXInputButtons(8, 1, 8),
                 XUSB_GAMEPAD_DPAD_UP | XUSB_GAMEPAD_DPAD_LEFT,
                 "1-based hat value 8 -> up-left");
        check_eq(HidHatToXInputButtons(8, 0, 7), 0,
                 "zero-based hat out-of-range null value 8 -> centred");
    }

    std::printf("\n== Physical XInput slot selection ==\n");
    {
        check_eq(SelectPhysicalXInputSlot(0, 1, 0x03), 0,
                 "preferred physical slot remains selected");
        check_eq(SelectPhysicalXInputSlot(0, 0, 0x03), XUSER_MAX_COUNT,
                 "own virtual slot is rejected without switching controllers");
        check_eq(SelectPhysicalXInputSlot(0, 0, 0x01), XUSER_MAX_COUNT,
                 "own virtual pad is never accepted as the only input");
        check_eq(SelectPhysicalXInputSlot(2, 3, 0x02), XUSER_MAX_COUNT,
                 "disconnected preferred slot does not switch controllers");
    }

    std::printf("\n== ViGEm device ancestry detection ==\n");
    {
        check(HidDeviceInstanceIsViGEm(L"NEFARIUS\\VIGEMBUS\\GEN1"),
              "modern ViGEmBus instance ID is virtual");
        check(HidDeviceInstanceIsViGEm(L"ROOT\\VIGEMBUS\\0000"),
              "legacy ViGEmBus instance ID is virtual");
        check(!HidDeviceInstanceIsViGEm(L"USB\\VID_054C&PID_0CE6\\ABC"),
              "physical USB controller instance is not ViGEm");
    }

    std::printf("\n== DS4 USB report parse ==\n");
    {
        uint8_t b[64] = {0};
        b[0] = 0x01;
        b[1] = 255;           // LX right
        b[2] = 0;             // LY up
        b[3] = 128; b[4] = 128;
        b[5] = 0x20 | 0x02;   // face Cross (0x20) + hat East (nibble 2)
        b[6] = 0x10 | 0x01;   // Share (0x10) + L1 (0x01)
        b[7] = 0x03;          // PS + touchpad
        b[8] = 200;           // L2 axis
        b[9] = 100;           // R2 axis

        SonyReportOffsets off;
        check(DS4HidReader::GetSonyOffsets(SonyDeviceType::DS4, b, 64, off), "GetSonyOffsets DS4 USB -> true");
        PhysicalGamepadState s = parseSony(b, off);
        check(s.leftX > 30000,  "LX maps near +max");
        check(s.leftY > 30000,  "LY up maps near +max (inverted)");
        check(s.buttons & XUSB_GAMEPAD_A,            "Cross -> A");
        check(s.buttons & XUSB_GAMEPAD_DPAD_RIGHT,   "hat East -> DPAD_RIGHT");
        check(s.buttons & XUSB_GAMEPAD_BACK,         "Share -> BACK");
        check(s.buttons & XUSB_GAMEPAD_LEFT_SHOULDER,"L1 -> LEFT_SHOULDER");
        check(s.psButton,                            "PS button set");
        check(s.touchpad,                            "touchpad set");
        check_eq(s.leftTrigger,  200, "L2 axis -> leftTrigger");
        check_eq(s.rightTrigger, 100, "R2 axis -> rightTrigger");
    }

    std::printf("\n== DualSense USB report parse (regression: layout differs from DS4) ==\n");
    {
        uint8_t b[64] = {0};
        b[0] = 0x01;
        b[1] = 128; b[2] = 128; b[3] = 128; b[4] = 128;
        b[5] = 200;           // L2 axis  (DualSense: triggers at 5/6)
        b[6] = 100;           // R2 axis
        b[7] = 0xFF;          // seq counter (must be ignored, NOT read as buttons)
        b[8] = 0x20;          // buttons0: Cross
        b[9] = 0x01;          // buttons1: L1
        b[10] = 0x01;         // buttons2: PS

        SonyReportOffsets off;
        check(DS4HidReader::GetSonyOffsets(SonyDeviceType::DualSense, b, 64, off), "GetSonyOffsets DualSense USB -> true");
        PhysicalGamepadState s = parseSony(b, off);
        check_eq(s.leftTrigger,  200, "L2 read from byte 5 (DualSense)");
        check_eq(s.rightTrigger, 100, "R2 read from byte 6 (DualSense)");
        check(s.buttons & XUSB_GAMEPAD_A,            "Cross (byte 8) -> A");
        check(s.buttons & XUSB_GAMEPAD_LEFT_SHOULDER,"L1 (byte 9) -> LEFT_SHOULDER");
        check(s.psButton,                            "PS (byte 10) set");

        // Demonstrate the old bug: reusing DS4 USB offsets on the SAME buffer
        // misreads triggers and buttons entirely.
        SonyReportOffsets ds4Off = { 1,2,3,4, 8,9, 5,6,7 };
        PhysicalGamepadState wrong = parseSony(b, ds4Off);
        check(wrong.leftTrigger != 200, "DS4 offsets on DualSense buffer -> wrong triggers (bug reproduced)");
    }

    std::printf("\n== DualSense Bluetooth (0x31) parse ==\n");
    {
        uint8_t b[78] = {0};
        b[0] = 0x31;
        b[1] = 0x00;          // BT header/tag
        b[2] = 128; b[3] = 128; b[4] = 128; b[5] = 128;
        b[6] = 200;           // L2
        b[7] = 100;           // R2
        b[9] = 0x20;          // buttons0: Cross
        SonyReportOffsets off;
        check(DS4HidReader::GetSonyOffsets(SonyDeviceType::DualSense, b, 78, off), "GetSonyOffsets DualSense BT -> true");
        PhysicalGamepadState s = parseSony(b, off);
        check_eq(s.leftTrigger, 200, "BT L2 read from byte 6");
        check(s.buttons & XUSB_GAMEPAD_A, "BT Cross (byte 9) -> A");
    }

    std::printf("\n== DS4 Bluetooth (0x11) parse ==\n");
    {
        uint8_t b[78] = {0};
        b[0] = 0x11;
        b[1] = 0xC0; b[2] = 0x00;   // BT header
        b[3] = 128; b[4] = 128; b[5] = 128; b[6] = 128;
        b[7] = 0x20;          // buttons0: Cross
        b[10] = 200;          // L2 axis
        SonyReportOffsets off;
        check(DS4HidReader::GetSonyOffsets(SonyDeviceType::DS4, b, 78, off), "GetSonyOffsets DS4 BT -> true");
        PhysicalGamepadState s = parseSony(b, off);
        check(s.buttons & XUSB_GAMEPAD_A, "BT Cross (byte 7) -> A");
        check_eq(s.leftTrigger, 200, "BT L2 read from byte 10");
    }

    std::printf("\n== GetSonyOffsets length / id guards ==\n");
    {
        uint8_t b[64] = {0};
        SonyReportOffsets off;
        b[0] = 0x01;
        check(!DS4HidReader::GetSonyOffsets(SonyDeviceType::DS4, b, 9, off),  "DS4 USB len 9 -> false");
        check( DS4HidReader::GetSonyOffsets(SonyDeviceType::DS4, b, 10, off), "DS4 USB len 10 -> true");
        check(!DS4HidReader::GetSonyOffsets(SonyDeviceType::DualSense, b, 10, off), "DualSense USB len 10 -> false");
        check( DS4HidReader::GetSonyOffsets(SonyDeviceType::DualSense, b, 11, off), "DualSense USB len 11 -> true");
        b[0] = 0x77;
        check(!DS4HidReader::GetSonyOffsets(SonyDeviceType::DS4, b, 64, off), "unknown report id -> false");
    }

    std::printf("\n== BuildDS4Report mapping ==\n");
    {
        XINPUT_GAMEPAD gp = {};
        gp.wButtons = XUSB_GAMEPAD_A | XUSB_GAMEPAD_BACK | XUSB_GAMEPAD_START | XUSB_GAMEPAD_DPAD_UP;
        DS4_REPORT r;
        BuildDS4Report(gp, r, /*psButton*/ true, /*touchpadClick*/ true);
        check(r.wButtons & DS4_BUTTON_CROSS,  "A -> Cross");
        check(r.wButtons & DS4_BUTTON_SHARE,  "BACK -> Share (not touchpad)");
        check(r.wButtons & DS4_BUTTON_OPTIONS,"START -> Options");
        check(r.bSpecial & DS4_SPECIAL_BUTTON_PS,       "psButton -> DS4 PS special");
        check(r.bSpecial & DS4_SPECIAL_BUTTON_TOUCHPAD, "touchpadClick -> DS4 touchpad special");
        check_eq(r.wButtons & 0x000F, DS4_BUTTON_DPAD_NORTH, "DPAD_UP -> hat NORTH");
        check_eq(r.bThumbLX, 128, "centre LX -> 128");
        check_eq(r.bThumbLY, 127, "centre LY -> 255-128 = 127");
    }

    std::printf("\n== BuildX360Report fidelity ==\n");
    {
        XINPUT_GAMEPAD gp = {};
        gp.wButtons = XUSB_GAMEPAD_A | XUSB_GAMEPAD_GUIDE;
        gp.bLeftTrigger = 55; gp.sThumbRX = 12345;
        XUSB_REPORT r;
        BuildX360Report(gp, r);
        check_eq(r.wButtons, gp.wButtons, "buttons copied verbatim (incl. GUIDE)");
        check_eq(r.bLeftTrigger, 55, "left trigger copied");
        check_eq(r.sThumbRX, 12345, "right stick X copied");
    }

    std::printf("\n== HidHide whitelist path (DOS device notation) ==\n");
    {
        auto narrow = [](const std::wstring& w) { std::string s; for (wchar_t c : w) s += (char)(c & 0x7F); return s; };
        auto check_ws = [&](const std::wstring& got, const std::wstring& want, const char* what) {
            if (got == want) { ++g_pass; std::printf("  [PASS] %s\n", what); }
            else { ++g_fail; std::printf("  [FAIL] %s (got '%s')\n", what, narrow(got).c_str()); }
        };
        check_ws(HidHideComposeDosDevicePath(L"C:\\Games\\app.exe", L"\\Device\\HarddiskVolume3"),
                 L"\\Device\\HarddiskVolume3\\Games\\app.exe",
                 "C: path -> DOS device notation");
        check_ws(HidHideComposeDosDevicePath(L"D:\\a\\b\\pass.exe", L"\\Device\\HarddiskVolume7"),
                 L"\\Device\\HarddiskVolume7\\a\\b\\pass.exe",
                 "D: path -> DOS device notation");
        check(HidHideComposeDosDevicePath(L"C:\\x.exe", L"").empty(),
              "empty drive device -> empty");
        check(HidHideComposeDosDevicePath(L"relative\\x.exe", L"\\Device\\HarddiskVolume1").empty(),
              "non-drive path -> empty");
    }

    std::printf("\n==============================\n");
    std::printf("Total: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
