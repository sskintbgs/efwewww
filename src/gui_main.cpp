// gui_main.cpp
// Modern dark dashboard front-end for the controller passthrough tool.
//
// Uses Dear ImGui with a Win32 + Direct3D 11 backend to present a clean,
// real-time GUI: a left nav rail, a header with live status pills + a Start/Stop
// button, selectable spoof-profile cards, a live controller visualisation and a
// settings view.  All forwarding runs on PassthroughEngine's worker thread; the
// UI just renders the engine's thread-safe snapshot each frame.

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "vigem_loader.h"
#include "hid_maestro_helper.h"
#include "passthrough_engine.h"
#include "gui_theme.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Direct3D 11 scaffolding (standard Dear ImGui example boilerplate)
// ─────────────────────────────────────────────────────────────────────────────
static ID3D11Device*            g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain        = nullptr;
static bool                     g_SwapChainOccluded  = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}
static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount       = 2;
    sd.BufferDesc.Width  = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags        = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed     = TRUE;
    sd.SwapEffect   = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)   // fall back to WARP (software) device
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth  = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // disable ALT app menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Elevation (ViGEmBus/HidHide operations need administrator rights)
// ─────────────────────────────────────────────────────────────────────────────
static bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  UI
// ─────────────────────────────────────────────────────────────────────────────
struct AppState {
    int  view    = 0;      // 0 = Dashboard, 1 = Profiles, 2 = Settings
    int  pollHz  = 1000;
    int  xslot   = 0;
    bool hidHide = true;
};

static bool BeginCard(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::v4(theme::BG_CARD));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    return open;
}
static void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

static void StatCard(const char* label, const char* value, const char* sub, ImU32 valueColor) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    BeginCard(label, ImVec2(avail.x, 92));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(valueColor));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    if (sub && sub[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }
    EndCard();
}

static void DrawHeader(AppState& app, ViGEmLoader& vigem, HIDMaestroHelper& maestro,
                       PassthroughEngine& engine, const EngineLiveState& s) {
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextUnformatted("Controller Passthrough");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::Text("   %s", maestro.GetActiveProfile().name.c_str());
    ImGui::PopStyleColor();

    // Right-aligned cluster: status pills + Start/Stop.
    const float btnW = 132.0f;
    float clusterW = btnW + 340.0f;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - clusterW + ImGui::GetCursorPosX());

    theme::Pill(vigem.isLoaded ? "ViGEmBus  ONLINE" : "ViGEmBus  OFFLINE",
                vigem.isLoaded ? IM_COL32(28, 60, 38, 255) : IM_COL32(60, 30, 30, 255),
                vigem.isLoaded ? theme::OK_GREEN : theme::ERR_RED);
    ImGui::SameLine();
    bool cloaking = s.running && s.cloakApplied;
    theme::Pill(cloaking ? "HidHide  CLOAKING" : "HidHide  OFF",
                cloaking ? IM_COL32(28, 55, 55, 255) : IM_COL32(34, 40, 50, 255),
                cloaking ? theme::ACCENT : theme::TEXT_MUTED);
    ImGui::SameLine();

    const bool running = engine.IsRunning();
    ImGui::PushStyleColor(ImGuiCol_Button,        running ? theme::v4(theme::ERR_RED) : theme::v4(theme::ACCENT));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, running ? ImVec4(0.90f, 0.36f, 0.33f, 1) : ImVec4(0.22f, 0.83f, 0.81f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  running ? ImVec4(0.80f, 0.30f, 0.28f, 1) : ImVec4(0.18f, 0.72f, 0.70f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.05f, 0.06f, 0.08f, 1));
    ImGui::BeginDisabled(!vigem.isLoaded);
    if (ImGui::Button(running ? "  Stop  " : "  Start  ", ImVec2(btnW, 0))) {
        if (running) {
            engine.Stop();
        } else {
            PassthroughEngine::Options o;
            o.hidHideEnabled = app.hidHide;
            o.pollHz         = (uint32_t)app.pollHz;
            o.xinputSlot     = (DWORD)app.xslot;
            engine.Start(vigem, maestro.GetActiveProfile(), o);
        }
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
}

static void DrawDashboard(HIDMaestroHelper& maestro, const EngineLiveState& s, bool running) {
    const auto& prof = maestro.GetActiveProfile();

    // ── Stat cards row ────────────────────────────────────────────────────────
    char buf[64];
    if (ImGui::BeginTable("stats", 4, ImGuiTableFlags_SizingStretchSame, ImVec2(0, 0))) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        StatCard("STATUS", running ? (s.hasInput ? (s.lastTx ? "LIVE" : "TX ERR") : "WAITING") : "IDLE",
                 running ? (s.hasInput ? "forwarding input" : "no controller yet") : "press Start",
                 running ? (s.lastTx || !s.hasInput ? theme::OK_GREEN : theme::ERR_RED) : theme::TEXT_MUTED);
        ImGui::TableNextColumn();
        std::snprintf(buf, sizeof(buf), "%.0f Hz", s.hz);
        char sub[48]; std::snprintf(sub, sizeof(sub), "target %u Hz", prof.pollingRateHz);
        StatCard("RATE", running ? buf : "-", running ? sub : "idle", theme::TEXT);
        ImGui::TableNextColumn();
        std::snprintf(buf, sizeof(buf), "%llu", s.packets);
        char sub2[48]; std::snprintf(sub2, sizeof(sub2), "failed: %llu", s.fails);
        StatCard("PACKETS", running ? buf : "0", sub2, theme::TEXT);
        ImGui::TableNextColumn();
        StatCard("INPUT", running ? (s.usingRawHid ? "Raw HID" : "XInput") : "-",
                 running ? s.inputSource : "not started", theme::TEXT);
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 4));

    // ── Active identity card ───────────────────────────────────────────────────
    BeginCard("identity", ImVec2(0, 78));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::TextUnformatted("ACTIVE VIRTUAL IDENTITY");
    ImGui::PopStyleColor();
    ImGui::Text("%s", prof.productName.c_str());
    ImGui::SameLine();
    std::snprintf(buf, sizeof(buf), "VID 0x%04X   PID 0x%04X", prof.vendorId, prof.productId);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::ACCENT));
    ImGui::Text("     %s", buf);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::Text("     %s", prof.virtualTarget == MaestroVirtualTarget::DualShock4 ? "DualShock 4 target" : "Xbox 360 target");
    ImGui::PopStyleColor();
    EndCard();

    ImGui::Dummy(ImVec2(0, 4));

    // ── Live controller visualisation ──────────────────────────────────────────
    BeginCard("live", ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::TextUnformatted("LIVE CONTROLLER");
    ImGui::PopStyleColor();
    ImGui::Separator();

    if (!running) {
        ImGui::Dummy(ImVec2(0, 20));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
        ImGui::TextUnformatted("  Press Start to begin forwarding and watch inputs here.");
        ImGui::PopStyleColor();
        EndCard();
        return;
    }

    const XINPUT_GAMEPAD& gp = s.gamepad;
    const float N = 32767.0f;

    // Sticks
    theme::Stick("LEFT STICK",  gp.sThumbLX / N, gp.sThumbLY / N, (gp.wButtons & XUSB_GAMEPAD_LEFT_THUMB) != 0);
    ImGui::SameLine(0, 40);
    theme::Stick("RIGHT STICK", gp.sThumbRX / N, gp.sThumbRY / N, (gp.wButtons & XUSB_GAMEPAD_RIGHT_THUMB) != 0);
    ImGui::SameLine(0, 60);

    // Triggers
    ImGui::BeginGroup();
    theme::TriggerBar("L2 / LT", gp.bLeftTrigger  / 255.0f);
    ImGui::Dummy(ImVec2(0, 6));
    theme::TriggerBar("R2 / RT", gp.bRightTrigger / 255.0f);
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    // Face + shoulder + system buttons
    auto led = [&](const char* l, unsigned mask) { theme::Led(l, (gp.wButtons & mask) != 0); ImGui::SameLine(); };
    led("A", XUSB_GAMEPAD_A); led("B", XUSB_GAMEPAD_B); led("X", XUSB_GAMEPAD_X); led("Y", XUSB_GAMEPAD_Y);
    ImGui::Dummy(ImVec2(14, 0)); ImGui::SameLine();
    led("LB", XUSB_GAMEPAD_LEFT_SHOULDER); led("RB", XUSB_GAMEPAD_RIGHT_SHOULDER);
    theme::Led("LT", gp.bLeftTrigger  > 30); ImGui::SameLine();
    theme::Led("RT", gp.bRightTrigger > 30); ImGui::SameLine();
    ImGui::Dummy(ImVec2(14, 0)); ImGui::SameLine();
    led("Back", XUSB_GAMEPAD_BACK); led("Start", XUSB_GAMEPAD_START);
    led("L3", XUSB_GAMEPAD_LEFT_THUMB); led("R3", XUSB_GAMEPAD_RIGHT_THUMB);
    theme::Led("PS", s.psButton); ImGui::SameLine();
    theme::Led("Pad", s.touchpad);

    ImGui::Dummy(ImVec2(0, 10));
    // D-Pad
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::TextUnformatted("D-PAD");
    ImGui::PopStyleColor();
    led("Up",    XUSB_GAMEPAD_DPAD_UP);
    led("Down",  XUSB_GAMEPAD_DPAD_DOWN);
    led("Left",  XUSB_GAMEPAD_DPAD_LEFT);
    theme::Led("Right", (gp.wButtons & XUSB_GAMEPAD_DPAD_RIGHT) != 0);
    EndCard();
}

static void DrawProfiles(HIDMaestroHelper& maestro, bool running) {
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::WARN_AMBER));
        ImGui::TextUnformatted("Stop passthrough to switch the active identity.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
    }
    const auto& profiles = maestro.GetProfiles();
    size_t sel = maestro.GetSelectedIndex();
    const int cols = 3;
    if (ImGui::BeginTable("profiles", cols, ImGuiTableFlags_SizingStretchSame)) {
        for (size_t i = 0; i < profiles.size(); ++i) {
            ImGui::TableNextColumn();
            const auto& p = profiles[i];
            const bool active = (i == sel);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, active ? ImVec4(0.10f, 0.24f, 0.24f, 1) : theme::v4(theme::BG_CARD));
            ImGui::PushStyleColor(ImGuiCol_Border,  active ? theme::v4(theme::ACCENT) : theme::v4(theme::STROKE));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, active ? 2.0f : 1.0f);
            ImGui::PushID((int)i);
            ImGui::BeginChild("card", ImVec2(0, 132), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(active ? theme::ACCENT : theme::TEXT));
            ImGui::TextWrapped("%s", p.name.c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
            ImGui::Text("VID 0x%04X   PID 0x%04X", p.vendorId, p.productId);
            ImGui::Text("%s", p.virtualTarget == MaestroVirtualTarget::DualShock4 ? "DualShock 4" : "Xbox 360");
            ImGui::Text("default %u Hz", p.pollingRateHz);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::BeginDisabled(running);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::ACCENT));
                ImGui::TextUnformatted("  ACTIVE");
                ImGui::PopStyleColor();
            } else if (ImGui::Button("Select", ImVec2(-1, 0))) {
                maestro.SetSelectedIndex(i);
            }
            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::PopID();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
        }
        ImGui::EndTable();
    }
}

static void DrawSettings(AppState& app, ViGEmLoader& vigem, PassthroughEngine& engine, bool running) {
    BeginCard("settings", ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::TextUnformatted("POLLING RATE");
    ImGui::PopStyleColor();
    if (ImGui::SliderInt("##hz", &app.pollHz, 125, 1000, "%d Hz")) {
        if (running) engine.SetPollHz((uint32_t)app.pollHz);
    }
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::TextUnformatted("XINPUT SLOT (fallback source when no HID gamepad is found)");
    ImGui::PopStyleColor();
    const char* slots[] = { "Slot 0", "Slot 1", "Slot 2", "Slot 3" };
    ImGui::SetNextItemWidth(180);
    if (ImGui::Combo("##slot", &app.xslot, slots, 4)) {
        if (running) engine.SetXInputSlot((DWORD)app.xslot);
    }
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::BeginDisabled(running);
    ImGui::Checkbox(" Hide physical controller with HidHide while running", &app.hidHide);
    ImGui::EndDisabled();
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
        ImGui::TextUnformatted("  (cloaking is applied at Start; stop to change)");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::TEXT_MUTED));
    ImGui::Text("ViGEmBus driver: ");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(vigem.isLoaded ? theme::OK_GREEN : theme::ERR_RED));
    ImGui::TextUnformatted(vigem.isLoaded ? "connected" : "not found - install ViGEmBus");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Reconnect ViGEmBus")) vigem.Reconnect();
    ImGui::EndDisabled();
    EndCard();
}

static void DrawSidebar(AppState& app, const EngineLiveState& s) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::v4(theme::BG_SIDEBAR));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 18));
    ImGui::BeginChild("sidebar", ImVec2(196, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::ACCENT));
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted("Passthrough");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 18));

    const char* items[] = { "Dashboard", "Profiles", "Settings" };
    for (int i = 0; i < 3; ++i) {
        if (ImGui::Selectable(items[i], app.view == i, 0, ImVec2(0, 34)))
            app.view = i;
    }

    // Bottom: compact live indicator.
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 60);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    theme::Pill(s.running ? "RUNNING" : "STOPPED",
                s.running ? IM_COL32(28, 55, 55, 255) : IM_COL32(34, 40, 50, 255),
                s.running ? theme::ACCENT : theme::TEXT_MUTED);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void RenderFrame(AppState& app, ViGEmLoader& vigem, HIDMaestroHelper& maestro, PassthroughEngine& engine) {
    const EngineLiveState s = engine.Snapshot();
    const bool running = engine.IsRunning();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    DrawSidebar(app, s);
    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 20));
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawHeader(app, vigem, maestro, engine, s);
    if (s.errorMessage[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(theme::ERR_RED));
        ImGui::TextWrapped("%s", s.errorMessage);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 8));

    switch (app.view) {
        case 0: DrawDashboard(maestro, s, running); break;
        case 1: DrawProfiles(maestro, running);     break;
        case 2: DrawSettings(app, vigem, engine, running); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Self-elevate so ViGEm/HidHide operations succeed.
    if (!IsRunAsAdmin()) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = path;
        sei.nShow  = SW_NORMAL;
        if (ShellExecuteExW(&sei)) return 0;   // relaunched elevated
        // otherwise continue unelevated
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance, nullptr, nullptr, nullptr, nullptr,
                       L"ControllerPassthroughGui", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Controller Passthrough", WS_OVERLAPPEDWINDOW,
                              100, 100, 1180, 740, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"Controller Passthrough", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr;   // don't litter an imgui.ini next to the exe
    io.FontGlobalScale = 1.15f;
    theme::Apply();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ViGEmLoader      vigem;
    HIDMaestroHelper maestro;
    PassthroughEngine engine;
    AppState         app;

    const ImVec4 clear = theme::v4(theme::BG_APP);
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Handle window occlusion / minimise.
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderFrame(app, vigem, maestro, engine);

        ImGui::Render();
        const float c[4] = { clear.x, clear.y, clear.z, clear.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, c);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);   // vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    engine.Stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
