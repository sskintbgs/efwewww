#pragma once

// gui_theme.h
// Modern dark dashboard theme + small drawing helpers for the ImGui front-end.
// Palette: deep navy background, teal accent, soft panels — tuned to read
// cleanly at a glance (see the reference dashboard).

#include "imgui.h"
#include <algorithm>

namespace theme {

// ── Palette (ImU32 for draw lists) ───────────────────────────────────────────
constexpr ImU32 ACCENT      = IM_COL32( 38, 199, 196, 255);   // teal
constexpr ImU32 ACCENT_DIM  = IM_COL32( 38, 199, 196, 120);
constexpr ImU32 BG_APP      = IM_COL32( 14,  17,  22, 255);
constexpr ImU32 BG_SIDEBAR  = IM_COL32( 11,  14,  18, 255);
constexpr ImU32 BG_CARD     = IM_COL32( 23,  27,  34, 255);
constexpr ImU32 BG_WELL     = IM_COL32( 18,  22,  28, 255);
constexpr ImU32 STROKE      = IM_COL32( 44,  51,  62, 255);
constexpr ImU32 TEXT        = IM_COL32(230, 233, 239, 255);
constexpr ImU32 TEXT_MUTED  = IM_COL32(138, 147, 162, 255);
constexpr ImU32 OK_GREEN    = IM_COL32( 63, 185,  80, 255);
constexpr ImU32 WARN_AMBER  = IM_COL32(227, 179,  65, 255);
constexpr ImU32 ERR_RED     = IM_COL32(248,  81,  73, 255);

inline ImVec4 v4(ImU32 c) {
    return ImVec4(((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                  ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                  ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                  ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
}

inline void Apply() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 10.0f;
    s.FrameRounding     = 8.0f;
    s.GrabRounding      = 8.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 10.0f;
    s.TabRounding       = 8.0f;
    s.WindowPadding     = ImVec2(18, 18);
    s.FramePadding      = ImVec2(12, 8);
    s.ItemSpacing       = ImVec2(12, 10);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.ScrollbarSize     = 12.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]           = v4(BG_APP);
    c[ImGuiCol_ChildBg]            = v4(BG_CARD);
    c[ImGuiCol_PopupBg]            = v4(BG_CARD);
    c[ImGuiCol_Border]             = v4(STROKE);
    c[ImGuiCol_Text]               = v4(TEXT);
    c[ImGuiCol_TextDisabled]       = v4(TEXT_MUTED);
    c[ImGuiCol_FrameBg]            = v4(BG_WELL);
    c[ImGuiCol_FrameBgHovered]     = ImVec4(0.16f, 0.19f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgActive]      = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
    c[ImGuiCol_Button]             = ImVec4(0.16f, 0.19f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonHovered]      = ImVec4(0.20f, 0.24f, 0.30f, 1.0f);
    c[ImGuiCol_ButtonActive]       = ImVec4(0.24f, 0.29f, 0.36f, 1.0f);
    c[ImGuiCol_Header]             = ImVec4(0.15f, 0.78f, 0.77f, 0.22f);
    c[ImGuiCol_HeaderHovered]      = ImVec4(0.15f, 0.78f, 0.77f, 0.32f);
    c[ImGuiCol_HeaderActive]       = ImVec4(0.15f, 0.78f, 0.77f, 0.45f);
    c[ImGuiCol_SliderGrab]         = v4(ACCENT);
    c[ImGuiCol_SliderGrabActive]   = v4(ACCENT);
    c[ImGuiCol_CheckMark]          = v4(ACCENT);
    c[ImGuiCol_Separator]          = v4(STROKE);
    c[ImGuiCol_ScrollbarBg]        = v4(BG_APP);
    c[ImGuiCol_ScrollbarGrab]      = v4(STROKE);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.30f, 0.35f, 0.42f, 1.0f);
}

// ── Small widgets built on the draw list ─────────────────────────────────────

// Rounded status pill at the current cursor; advances the cursor by its size.
inline void Pill(const char* text, ImU32 bg, ImU32 fg) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p  = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(text);
    const float padx = 11.0f, pady = 5.0f;
    ImVec2 size(ts.x + padx * 2, ts.y + pady * 2);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, size.y * 0.5f);
    dl->AddText(ImVec2(p.x + padx, p.y + pady), fg, text);
    ImGui::Dummy(size);
}

// A labelled indicator "LED" that lights up in the accent colour when on.
inline void Led(const char* label, bool on) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p  = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(label);
    float w = std::max(ts.x + 16.0f, 36.0f);
    float h = ts.y + 12.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), on ? ACCENT : IM_COL32(34, 40, 50, 255), 6.0f);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + 6.0f),
                on ? IM_COL32(10, 12, 16, 255) : TEXT_MUTED, label);
    ImGui::Dummy(ImVec2(w, h));
}

// Analog stick widget (nx, ny in [-1,1]; ny positive = up).
inline void Stick(const char* label, float nx, float ny, bool pressed) {
    const float R = 46.0f;
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(TEXT_MUTED));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ctr(p.x + R, p.y + R);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ctr, R, BG_WELL, 48);
    dl->AddCircle(ctr, R, STROKE, 48, 2.0f);
    dl->AddLine(ImVec2(ctr.x - R, ctr.y), ImVec2(ctr.x + R, ctr.y), IM_COL32(40, 46, 56, 160));
    dl->AddLine(ImVec2(ctr.x, ctr.y - R), ImVec2(ctr.x, ctr.y + R), IM_COL32(40, 46, 56, 160));
    nx = std::max(-1.0f, std::min(1.0f, nx));
    ny = std::max(-1.0f, std::min(1.0f, ny));
    ImVec2 knob(ctr.x + nx * R * 0.72f, ctr.y - ny * R * 0.72f);
    dl->AddCircleFilled(knob, 11.0f, pressed ? ACCENT : IM_COL32(210, 215, 222, 255), 24);
    ImGui::Dummy(ImVec2(R * 2, R * 2));
    ImGui::EndGroup();
}

// Horizontal analog trigger bar (v in [0,1]).
inline void TriggerBar(const char* label, float v) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(TEXT_MUTED));
    ImGui::Text("%s  %3d%%", label, (int)(v * 100.0f + 0.5f));
    ImGui::PopStyleColor();
    v = std::max(0.0f, std::min(1.0f, v));
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = 172.0f, h = 14.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), BG_WELL, h * 0.5f);
    if (v > 0.001f)
        dl->AddRectFilled(p, ImVec2(p.x + w * v, p.y + h), ACCENT, h * 0.5f);
    ImGui::Dummy(ImVec2(w, h));
}

} // namespace theme
