# ---------------------------------------------------------------------------
# TMPluginScreenshot - optional CMake entry point.
#
#   IMPORTANT: build.bat is the acceptance build.  This file exists so that a
#   machine with cmake on PATH can build the very same DLL with the very same
#   compiler/linker settings.  Keep both files in sync.
#
#   cmake -S . -B build-cmake -A x64
#   cmake --build build-cmake --config Release
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(TMPluginScreenshot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(TMPluginScreenshot SHARED
    src/PluginSettings.cpp
    src/CaptureEngine.cpp
    src/ImageWriter.cpp
    src/RegionSelector.cpp
    src/WindowPicker.cpp
    src/OptionsDialog.cpp
    src/HotkeyManager.cpp
    src/TMPlugin.cpp)

target_include_directories(TMPluginScreenshot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

# /MT static CRT: the plugin must not depend on VCRUNTIME140.dll / MSVCP140.dll
# because it is loaded into the host process.
set_property(TARGET TMPluginScreenshot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

target_compile_definitions(TMPluginScreenshot PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    _CRT_SECURE_NO_WARNINGS)

target_compile_options(TMPluginScreenshot PRIVATE
    /utf-8 /EHsc /W3 /O2)

target_link_options(TMPluginScreenshot PRIVATE
    /SUBSYSTEM:WINDOWS /RELEASE /MACHINE:X64)

target_link_libraries(TMPluginScreenshot PRIVATE
    windowscodecs dwmapi shlwapi shell32 ole32 oleaut32 uuid user32 gdi32 advapi32)

set_target_properties(TMPluginScreenshot PROPERTIES
    PREFIX ""
    OUTPUT_NAME "TMPluginScreenshot"
    RUNTIME_OUTPUT_DIRECTORY           "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE   "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG     "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY           "${CMAKE_CURRENT_SOURCE_DIR}/build"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE   "${CMAKE_CURRENT_SOURCE_DIR}/build"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG     "${CMAKE_CURRENT_SOURCE_DIR}/build")
