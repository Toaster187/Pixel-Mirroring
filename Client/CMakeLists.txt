cmake_minimum_required(VERSION 3.25)
project(PixelMirroring VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find necessary packages
# We will assume these are installed via vcpkg or system package manager
# find_package(SDL2 REQUIRED)
# find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Platform-specific sources
if(WIN32)
    set(PLATFORM_SOURCES
        src/window/win32_window.cpp
        src/tray/win32_tray.cpp
    )
elseif(APPLE)
    set(PLATFORM_SOURCES
        src/window/cocoa_window.mm
        src/tray/cocoa_tray.mm
    )
endif()

add_executable(PixelMirroring
    src/main.cpp
    src/adb/adb_client.cpp
    src/network/network_scanner.cpp
    # src/adb/device_manager.cpp
    src/stream/scrcpy_client.cpp
    src/stream/video_stubs.cpp
    src/input/input_handler.cpp
    # src/config/settings.cpp
    ${PLATFORM_SOURCES}
)

# target_link_libraries(PixelMirroring PRIVATE SDL2::SDL2 ${FFMPEG_LIBRARIES})
target_link_libraries(PixelMirroring PRIVATE httplib::httplib nlohmann_json::nlohmann_json)

if(WIN32)
    # Required for custom window frame and GDI drawing on Windows
    target_link_libraries(PixelMirroring PRIVATE dwmapi uxtheme gdi32 gdiplus user32)
    # No console window — we are GUI cave people
    set_target_properties(PixelMirroring PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    # Required for native Cocoa window
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(APPKIT_FRAMEWORK AppKit)
    target_link_libraries(PixelMirroring PRIVATE ${COCOA_FRAMEWORK} ${APPKIT_FRAMEWORK})
endif()
