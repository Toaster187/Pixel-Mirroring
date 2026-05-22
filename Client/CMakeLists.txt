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

# --- Bootstrap Android Platform-Tools (ADB) ---
# Cave man need ADB to talk to fire-box phone. If no ADB, cave man go get it!
if(WIN32)
    set(PM_ADB_EXPECTED_FILE "adb.exe")
    set(PM_PLATFORM_TOOLS_URL "https://dl.google.com/android/repository/platform-tools-latest-windows.zip")
elseif(APPLE)
    set(PM_ADB_EXPECTED_FILE "adb")
    set(PM_PLATFORM_TOOLS_URL "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip")
else()
    set(PM_ADB_EXPECTED_FILE "adb")
    set(PM_PLATFORM_TOOLS_URL "https://dl.google.com/android/repository/platform-tools-latest-linux.zip")
endif()

set(PM_PLATFORM_TOOLS_DIR "${CMAKE_SOURCE_DIR}/vendor/platform-tools")

if(NOT EXISTS "${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_EXPECTED_FILE}")
    message(STATUS "Ugg! ADB not found at ${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_EXPECTED_FILE}. Downloading from Google...")
    set(PM_TEMP_ZIP "${CMAKE_CURRENT_BINARY_DIR}/platform-tools.zip")
    
    file(DOWNLOAD "${PM_PLATFORM_TOOLS_URL}" "${PM_TEMP_ZIP}" SHOW_PROGRESS STATUS PM_DOWNLOAD_STATUS)
    list(GET PM_DOWNLOAD_STATUS 0 PM_DOWNLOAD_ERR)
    if(NOT PM_DOWNLOAD_ERR EQUAL 0)
        list(GET PM_DOWNLOAD_STATUS 1 PM_DOWNLOAD_MSG)
        message(FATAL_ERROR "Ugg! Failed to download ADB: ${PM_DOWNLOAD_MSG}")
    endif()

    message(STATUS "Ugg! Extracting ADB...")
    file(ARCHIVE_EXTRACT INPUT "${PM_TEMP_ZIP}" DESTINATION "${CMAKE_SOURCE_DIR}/vendor")
    file(REMOVE "${PM_TEMP_ZIP}")
    
    # On POSIX, make ADB executable
    if(NOT WIN32)
        execute_process(COMMAND chmod +x "${PM_PLATFORM_TOOLS_DIR}/adb")
    endif()
    message(STATUS "Ugg! ADB bootstrap finished successfully!")
endif()

if(WIN32)
    set(PM_ADB_RUNTIME_FILES
        adb.exe
        AdbWinApi.dll
        AdbWinUsbApi.dll
    )
else()
    set(PM_ADB_RUNTIME_FILES
        adb
    )
endif()

foreach(PM_ADB_RUNTIME_FILE IN LISTS PM_ADB_RUNTIME_FILES)
    if(EXISTS "${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_RUNTIME_FILE}")
        add_custom_command(TARGET PixelMirroring POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_RUNTIME_FILE}"
                "$<TARGET_FILE_DIR:PixelMirroring>/${PM_ADB_RUNTIME_FILE}"
            COMMENT "Copy bundled ADB runtime: ${PM_ADB_RUNTIME_FILE}"
        )
    else()
        message(WARNING "Bundled ADB runtime file missing: ${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_RUNTIME_FILE}")
    endif()
endforeach()

# --- Copy scrcpy-server.jar ---
# Cave man need scrcpy server to throw pictures from phone to PC.
set(PM_SCRCPY_SERVER_CANDIDATES
    "${CMAKE_SOURCE_DIR}/../scrcpy_download/scrcpy-server.jar"
    "${CMAKE_SOURCE_DIR}/scrcpy-server.jar"
)

set(PM_SCRCPY_SERVER_PATH "")
foreach(PM_CANDIDATE IN LISTS PM_SCRCPY_SERVER_CANDIDATES)
    if(EXISTS "${PM_CANDIDATE}")
        set(PM_SCRCPY_SERVER_PATH "${PM_CANDIDATE}")
        break()
    endif()
endforeach()

if(PM_SCRCPY_SERVER_PATH)
    add_custom_command(TARGET PixelMirroring POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PM_SCRCPY_SERVER_PATH}"
            "$<TARGET_FILE_DIR:PixelMirroring>/scrcpy-server.jar"
        COMMENT "Copy scrcpy-server.jar next to desktop client"
    )
else()
    message(WARNING "Ugg! scrcpy-server.jar not found anywhere!")
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

if(DEFINED VCPKG_TARGET_TRIPLET)
    # Ensure FFmpeg and other DLLs that might not be picked up by TARGET_RUNTIME_DLLS are copied
    add_custom_command(TARGET PixelMirroring POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin"
            "$<TARGET_FILE_DIR:PixelMirroring>"
        COMMENT "Copy all vcpkg DLLs next to desktop client"
    )
endif()

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

# --- Installation and Packaging ---
# Install the main executable
install(TARGETS PixelMirroring
    RUNTIME DESTINATION .
)

# Install dependent runtime DLLs (SDL, FFmpeg, vcpkg dependencies)
install(FILES $<TARGET_RUNTIME_DLLS:PixelMirroring>
    DESTINATION .
)

if(DEFINED VCPKG_TARGET_TRIPLET)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/"
        DESTINATION .
        FILES_MATCHING PATTERN "*.dll"
    )
endif()

# Install companion Android APK from build target dir if it exists
install(FILES "$<TARGET_FILE_DIR:PixelMirroring>/PixelMirroring.apk"
    DESTINATION .
    OPTIONAL
)

# Install scrcpy-server.jar from build target dir if it exists
install(FILES "$<TARGET_FILE_DIR:PixelMirroring>/scrcpy-server.jar"
    DESTINATION .
    OPTIONAL
)

# Install ADB runtime files
if(WIN32)
    foreach(PM_ADB_RUNTIME_FILE IN LISTS PM_ADB_RUNTIME_FILES)
        install(FILES "$<TARGET_FILE_DIR:PixelMirroring>/${PM_ADB_RUNTIME_FILE}"
            DESTINATION .
            OPTIONAL
        )
    endforeach()
else()
    install(PROGRAMS "$<TARGET_FILE_DIR:PixelMirroring>/adb"
        DESTINATION .
        OPTIONAL
    )
endif()

# --- CPack Packaging Setup ---
set(CPACK_PACKAGE_NAME "PixelMirroring")
set(CPACK_PACKAGE_VENDOR "PixelMirroring Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Android Screen Mirroring Tool - Apple iPhone Mirroring for Android")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS;WIX")
    
    # Executables to create shortcuts for
    set(CPACK_PACKAGE_EXECUTABLES "PixelMirroring" "Pixel Mirroring")
    
    # NSIS specific settings
    set(CPACK_NSIS_PACKAGE_NAME "Pixel Mirroring")
    set(CPACK_NSIS_DISPLAY_NAME "Pixel Mirroring")
    set(CPACK_NSIS_HELP_LINK "https://github.com/Toasterfritzi/Pixel-Mirroring")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/Toasterfritzi/Pixel-Mirroring")
    set(CPACK_NSIS_MODIFY_PATH ON)
    
    # WiX specific settings (Official Windows Installer)
    set(CPACK_WIX_UPGRADE_GUID "D9F369E9-1A1A-4BE6-B6DF-D7E31BEF8CFD")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Pixel Mirroring")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
