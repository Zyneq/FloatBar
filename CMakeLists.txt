cmake_minimum_required(VERSION 3.21)
project(FloatBar VERSION 1.0.0 LANGUAGES CXX RC)

# The release workflow passes the tag (e.g. -DFB_VERSION=1.2.0). Its leading
# numbers also become the numeric file version in the version resource.
set(FB_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded in the binary")
if(FB_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
    set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: a single exe that runs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# No WIN32_LEAN_AND_MEAN: UIAutomation.h needs the COM headers it strips out.
add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 FB_VERSION="${FB_VERSION}")
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)

set(FB_ICON "${CMAKE_SOURCE_DIR}/assets/floatbar.ico")
configure_file(res/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(floatbar WIN32 ${SOURCES} res/app.manifest ${CMAKE_BINARY_DIR}/version.rc)
target_include_directories(floatbar PRIVATE src)
# Windows system libraries only.
target_link_libraries(floatbar PRIVATE ole32 oleaut32 uiautomationcore user32 gdi32 advapi32 shcore shell32 comctl32 comdlg32 dwmapi)

# Developer tool (not shipped): per-frame measurement, recorded-reading replay
# and the end-to-end test suite (tools/probe).
option(FLOATBAR_TOOLS "Build developer tools" ON)
if(FLOATBAR_TOOLS)
    file(GLOB PROBE_SOURCES CONFIGURE_DEPENDS tools/probe/*.cpp)
    add_executable(floatbar-probe ${PROBE_SOURCES} src/bounds.cpp src/uia_util.cpp src/region.cpp src/config.cpp src/motion.cpp res/app.manifest)
    target_include_directories(floatbar-probe PRIVATE src)
    target_link_libraries(floatbar-probe PRIVATE d3d11 dxgi ole32 oleaut32 uiautomationcore user32 gdi32 shell32)
endif()
