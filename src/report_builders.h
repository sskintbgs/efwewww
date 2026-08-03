#pragma once

// report_builders.h
// Pure, dependency-free conversion helpers that turn a unified XINPUT_GAMEPAD
// snapshot into a ViGEm XUSB (Xbox 360) or DS4 (DualShock 4) report.
//
// These functions perform no I/O and touch no global state, so they are shared
// by main.cpp and exercised directly by the unit tests.

#include <windows.h>
#include <xinput.h>
#include <cstdint>

#include "ViGEm/Common.h"

// ─────────────────────────────────────────────────────────────────────────────
//  XInput → DS4 byte conversion  (128 = centre for DS4 axes)
// ─────────────────────────────────────────────────────────────────────────────
static inline uint8_t ConvertAxis(int16_t v) {
    // XInput -32768..32767 → DS4 0..255.  Shifting the unsigned 0..65535 value
    // right by 8 maps the centre (0 → 32768) exactly onto 128 and the extremes
    // onto 0 and 255, which the older "* 255 / 65535" form got slightly wrong
    // (centre landed on 127).
    return static_cast<uint8_t>((static_cast<int>(v) + 32768) >> 8);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build an Xbox 360 (XUSB) report from a raw XINPUT_GAMEPAD
// ─────────────────────────────────────────────────────────────────────────────
static inline void BuildX360Report(const XINPUT_GAMEPAD& gp, XUSB_REPORT& r) {
    XUSB_REPORT_INIT(&r);
    r.wButtons      = gp.wButtons;   // XUSB button layout matches XInput 1:1,
                                      // including the guide bit when present
    r.bLeftTrigger  = gp.bLeftTrigger;
    r.bRightTrigger = gp.bRightTrigger;
    r.sThumbLX      = gp.sThumbLX;
    r.sThumbLY      = gp.sThumbLY;
    r.sThumbRX      = gp.sThumbRX;
    r.sThumbRY      = gp.sThumbRY;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build a DualShock 4 report from a unified XINPUT_GAMEPAD
//
//  psButton / touchpadClick carry the two DS4-specific inputs that have no
//  XInput button bit, so callers can forward a physical controller's PS and
//  touchpad-click faithfully instead of losing them.  For an Xbox source,
//  callers typically pass psButton = (guide bit set) and touchpadClick = false.
// ─────────────────────────────────────────────────────────────────────────────
static inline void BuildDS4Report(const XINPUT_GAMEPAD& gp, DS4_REPORT& r,
                                  bool psButton = false, bool touchpadClick = false) {
    DS4_REPORT_INIT(&r);

    // Face buttons: XInput A/B/X/Y → DS4 Cross/Circle/Square/Triangle
    if (gp.wButtons & XUSB_GAMEPAD_A)              r.wButtons |= DS4_BUTTON_CROSS;
    if (gp.wButtons & XUSB_GAMEPAD_B)              r.wButtons |= DS4_BUTTON_CIRCLE;
    if (gp.wButtons & XUSB_GAMEPAD_X)              r.wButtons |= DS4_BUTTON_SQUARE;
    if (gp.wButtons & XUSB_GAMEPAD_Y)              r.wButtons |= DS4_BUTTON_TRIANGLE;

    // Shoulders / thumbs
    if (gp.wButtons & XUSB_GAMEPAD_LEFT_SHOULDER)  r.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
    if (gp.wButtons & XUSB_GAMEPAD_RIGHT_SHOULDER) r.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
    if (gp.wButtons & XUSB_GAMEPAD_LEFT_THUMB)     r.wButtons |= DS4_BUTTON_THUMB_LEFT;
    if (gp.wButtons & XUSB_GAMEPAD_RIGHT_THUMB)    r.wButtons |= DS4_BUTTON_THUMB_RIGHT;

    // Start/Back → Options/Share (the natural DS4 equivalents).  Back was
    // previously funnelled onto the touchpad click, which collided with a real
    // touchpad press and left Share unreachable.
    if (gp.wButtons & XUSB_GAMEPAD_START)          r.wButtons |= DS4_BUTTON_OPTIONS;
    if (gp.wButtons & XUSB_GAMEPAD_BACK)           r.wButtons |= DS4_BUTTON_SHARE;

    // DS4 specials live outside wButtons.  PS (home) and the touchpad click are
    // distinct buttons and are forwarded independently.
    if (psButton)                                  r.bSpecial |= DS4_SPECIAL_BUTTON_PS;
    if (touchpadClick)                             r.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;

    // D-Pad via hat value.
    // DS4_BUTTON_DPAD_* values live in the low 4 bits of wButtons as a hat
    // (0=N, 1=NE, … 7=NW, 8=none).  DS4_REPORT_INIT sets the nibble to
    // DS4_BUTTON_DPAD_NONE (0x08).  Because the face/shoulder buttons above
    // already ORed bits into wButtons we must mask the low nibble to zero
    // before writing the hat value — otherwise e.g. CROSS (0x20) leaks into
    // the nibble and produces a bogus hat direction.
    bool du = (gp.wButtons & XUSB_GAMEPAD_DPAD_UP)    != 0;
    bool dd = (gp.wButtons & XUSB_GAMEPAD_DPAD_DOWN)  != 0;
    bool dl = (gp.wButtons & XUSB_GAMEPAD_DPAD_LEFT)  != 0;
    bool dr = (gp.wButtons & XUSB_GAMEPAD_DPAD_RIGHT) != 0;

    DS4_DPAD_DIRECTIONS hat;
    if      (du && dr)  hat = DS4_BUTTON_DPAD_NORTHEAST;
    else if (du && dl)  hat = DS4_BUTTON_DPAD_NORTHWEST;
    else if (dd && dr)  hat = DS4_BUTTON_DPAD_SOUTHEAST;
    else if (dd && dl)  hat = DS4_BUTTON_DPAD_SOUTHWEST;
    else if (du)        hat = DS4_BUTTON_DPAD_NORTH;
    else if (dd)        hat = DS4_BUTTON_DPAD_SOUTH;
    else if (dr)        hat = DS4_BUTTON_DPAD_EAST;
    else if (dl)        hat = DS4_BUTTON_DPAD_WEST;
    else                hat = DS4_BUTTON_DPAD_NONE;

    r.wButtons = (r.wButtons & ~static_cast<USHORT>(0x000Fu)) | static_cast<USHORT>(hat & 0x000Fu);

    // Analog
    r.bTriggerL = gp.bLeftTrigger;
    r.bTriggerR = gp.bRightTrigger;
    r.bThumbLX  = ConvertAxis(gp.sThumbLX);
    r.bThumbLY  = 255 - ConvertAxis(gp.sThumbLY);   // DS4 Y is inverted
    r.bThumbRX  = ConvertAxis(gp.sThumbRX);
    r.bThumbRY  = 255 - ConvertAxis(gp.sThumbRY);
}
