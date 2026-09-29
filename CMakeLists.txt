cmake_minimum_required(VERSION 3.21)
project(IslandBar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT so the exes run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# No WIN32_LEAN_AND_MEAN: UIAutomation.h needs the COM headers it strips out.
add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

set(APP_MANIFEST ${CMAKE_SOURCE_DIR}/res/app.manifest)

# Shared code: UIA helpers, island bounds, regions, config, log.
add_library(islandbar_core STATIC
    src/common/uia_util.cpp
    src/core/bounds.cpp
    src/core/region.cpp
    src/core/config.cpp
    src/core/log.cpp
)
target_include_directories(islandbar_core PUBLIC src third_party)
target_link_libraries(islandbar_core PUBLIC ole32 oleaut32 uiautomationcore user32 gdi32 advapi32 shcore shell32)

# The app: tray icon, settings window, change tracking.
add_executable(islandbar WIN32
    src/islandbar/main.cpp
    src/islandbar/engine.cpp
    src/islandbar/settings_window.cpp
    src/islandbar/app_icon.cpp
    ${APP_MANIFEST}
)
target_link_libraries(islandbar PRIVATE islandbar_core comctl32 dwmapi)

# Diagnostic tool: dumps the taskbar UIA tree (and the computed islands with --islands).
add_executable(uia-dump
    src/uia-dump/main.cpp
    ${APP_MANIFEST}
)
target_link_libraries(uia-dump PRIVATE islandbar_core)
