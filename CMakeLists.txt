cmake_minimum_required(VERSION 3.15)
project(ControllerPassthrough LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored ViGEmClient (builds the client static library from source and
# publishes its include/ directory as a PUBLIC usage requirement).
add_subdirectory(third_party/ViGEmClient)

# vigem_loader.h refers to the vendored header via a repo-root-relative path,
# so the repo root must be on the include search path in addition to the
# ViGEmClient include dir that the target already exports.
set(PASSTHROUGH_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ViGEmClient/include)

# System import libraries the app uses.  On MSVC the headers also pull these in
# via #pragma comment(lib, ...); listing them here keeps other toolchains happy.
set(PASSTHROUGH_SYS_LIBS setupapi hid cfgmgr32 winmm xinput)

# ── Applications (Windows only) ───────────────────────────────────────────────
if(WIN32)
    # CLI front-end.
    add_executable(controller_passthrough src/main.cpp)
    target_include_directories(controller_passthrough PRIVATE ${PASSTHROUGH_INCLUDES})
    target_link_libraries(controller_passthrough PRIVATE ViGEmClient ${PASSTHROUGH_SYS_LIBS})

    # Dear ImGui (core + Win32/DX11 backends) built as a static library.
    add_library(imgui STATIC
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_win32.cpp
        third_party/imgui/backends/imgui_impl_dx11.cpp)
    target_include_directories(imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends)

    # Modern GUI front-end (windowed).
    add_executable(controller_passthrough_gui WIN32 src/gui_main.cpp)
    target_compile_definitions(controller_passthrough_gui PRIVATE UNICODE _UNICODE)
    target_include_directories(controller_passthrough_gui PRIVATE ${PASSTHROUGH_INCLUDES})
    target_link_libraries(controller_passthrough_gui PRIVATE
        imgui ViGEmClient
        d3d11 dxgi d3dcompiler dwmapi gdi32
        ${PASSTHROUGH_SYS_LIBS})
endif()

# ── Unit tests for the pure parsing / conversion logic ───────────────────────
# These touch no device APIs, so they build and run on any host (including a
# MinGW cross-compile executed under Wine — see scripts/run_tests.sh).
enable_testing()
add_executable(parse_tests tests/parse_tests.cpp)
target_include_directories(parse_tests PRIVATE ${PASSTHROUGH_INCLUDES})
add_test(NAME parse_tests COMMAND parse_tests)
