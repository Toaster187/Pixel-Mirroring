cmake_minimum_required(VERSION 3.25)
project(PixelMirroring VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(FFMPEG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)

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
    src/settings.h
    src/settings.cpp
    src/adb/adb_client.cpp
    src/network/network_scanner.cpp
    # src/adb/device_manager.cpp
    src/stream/scrcpy_client.cpp
    src/stream/video_decoder.cpp
    src/stream/video_renderer.cpp
    src/input/input_handler.cpp
    # src/config/settings.cpp
    ${PLATFORM_SOURCES}
)

if(MSVC)
    target_compile_options(PixelMirroring PRIVATE /EHsc)
endif()

set(PM_ANDROID_APK_CANDIDATES
    "${CMAKE_SOURCE_DIR}/../Android/app/build/outputs/apk/debug/app-debug.apk"
    "${CMAKE_SOURCE_DIR}/../Android/app/build/intermediates/apk/debug/app-debug.apk"
)

foreach(PM_ANDROID_APK_CANDIDATE IN LISTS PM_ANDROID_APK_CANDIDATES)
    if(EXISTS "${PM_ANDROID_APK_CANDIDATE}")
        set(PM_ANDROID_APK "${PM_ANDROID_APK_CANDIDATE}")
        break()
    endif()
endforeach()

if(PM_ANDROID_APK)
    add_custom_command(TARGET PixelMirroring POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PM_ANDROID_APK}"
            "$<TARGET_FILE_DIR:PixelMirroring>/PixelMirroring.apk"
        COMMENT "Copy Android companion APK next to desktop client"
    )
endif()

if(WIN32)
    set(PM_PLATFORM_TOOLS_DIR "${CMAKE_SOURCE_DIR}/vendor/platform-tools")
    set(PM_ADB_RUNTIME_FILES
        adb.exe
        AdbWinApi.dll
        AdbWinUsbApi.dll
    )

    foreach(PM_ADB_RUNTIME_FILE IN LISTS PM_ADB_RUNTIME_FILES)
        if(EXISTS "${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_RUNTIME_FILE}")
            add_custom_command(TARGET PixelMirroring POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_RUNTIME_FILE}"
                    "$<TARGET_FILE_DIR:PixelMirroring>/${PM_ADB_RUNTIME_FILE}"
                COMMENT "Copy bundled ADB runtime"
            )
        else()
            message(WARNING "Bundled ADB runtime file missing: ${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_RUNTIME_FILE}")
        endif()
    endforeach()
endif()

target_include_directories(PixelMirroring PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(PixelMirroring PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(PixelMirroring PRIVATE
    ${FFMPEG_LIBRARIES}
    httplib::httplib
    nlohmann_json::nlohmann_json
    SDL2::SDL2
)

add_custom_command(TARGET PixelMirroring POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:PixelMirroring>
        $<TARGET_FILE_DIR:PixelMirroring>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy runtime DLLs next to desktop client"
)

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
