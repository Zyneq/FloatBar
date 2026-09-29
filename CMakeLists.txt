cmake_minimum_required(VERSION 3.21)
project(IslandBar VERSION 1.0.0 LANGUAGES CXX RC)

# The release workflow passes the tag (e.g. -DIB_VERSION=1.2.0). Its leading
# numbers also become the numeric file version in the version resource.
set(IB_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded in the binaries")
if(IB_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
    set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT so the exes run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# No WIN32_LEAN_AND_MEAN: UIAutomation.h needs the COM headers it strips out.
add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 IB_VERSION="${IB_VERSION}")
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)

set(APP_MANIFEST ${CMAKE_SOURCE_DIR}/res/app.manifest)
configure_file(res/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)

# Shared code: UIA helpers, island bounds, regions, config, log, tree dump.
add_library(islandbar_core STATIC
    src/common/uia_util.cpp
    src/core/bounds.cpp
    src/core/region.cpp
    src/core/config.cpp
    src/core/log.cpp
    src/core/tree_dump.cpp
)
target_include_directories(islandbar_core PUBLIC src third_party)
target_link_libraries(islandbar_core PUBLIC ole32 oleaut32 uiautomationcore user32 gdi32 advapi32 shcore shell32)

# The app: tray icon, settings window, change tracking, backdrop.
add_executable(islandbar WIN32
    src/islandbar/main.cpp
    src/islandbar/engine.cpp
    src/islandbar/backdrop.cpp
    src/islandbar/settings_window.cpp
    src/islandbar/debug_report.cpp
    src/islandbar/app_icon.cpp
    ${APP_MANIFEST}
    ${CMAKE_BINARY_DIR}/version.rc
)
target_link_libraries(islandbar PRIVATE islandbar_core comctl32 comdlg32 dwmapi)

# Diagnostic tool: dumps the taskbar UIA tree (and the computed islands with --islands).
add_executable(uia-dump
    src/uia-dump/main.cpp
    ${APP_MANIFEST}
)
target_link_libraries(uia-dump PRIVATE islandbar_core)
