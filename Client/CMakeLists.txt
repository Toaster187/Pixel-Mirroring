cmake_minimum_required(VERSION 3.25)
project(PixelMirroring VERSION 5.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PM_PORTABLE_BUILD "Build a portable version that does not write to AppData" OFF)
option(PM_BUILD_TESTS "Build the small standalone self-checks" ON)

find_package(FFMPEG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)

# Platform-specific sources
if(WIN32)
    # Need RC compiler for app icon resource
    enable_language(RC)
    set(PLATFORM_SOURCES
        src/window/win32_window.cpp
        src/tray/win32_tray.cpp
        src/app.rc
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
    src/stream/capture_controller.cpp
    src/stream/audio_player.cpp
    src/input/input_handler.cpp
    src/input/hid_keyboard.cpp
    src/util/encoding.cpp
    # src/config/settings.cpp
    ${PLATFORM_SOURCES}
)

if(MSVC)
    # Without this MSVC reads UTF-8 sources as cp1252 and corrupts every "..." and
    # L"..." literal, which is what broke the German UI in release 3.67.
    # /utf-8 sets source AND execution charset to UTF-8. NEVER remove it.
    target_compile_options(PixelMirroring PRIVATE /EHsc /utf-8)
endif()

if(PM_PORTABLE_BUILD)
    target_compile_definitions(PixelMirroring PRIVATE PM_PORTABLE_BUILD)
endif()

# --- Self-checks ---
# No test framework here on purpose. This is a standalone main() with a hand-written
# check(): it needs only hid_keyboard.cpp, so it builds quickly and pulls in neither
# FFmpeg nor SDL. It is NOT installed and NOT packaged.
# Run it with "ctest --test-dir build --output-on-failure", or start the exe directly.
if(PM_BUILD_TESTS)
    enable_testing()

    add_executable(pm_hid_keyboard_test
        tests/hid_keyboard_test.cpp
        src/input/hid_keyboard.cpp
    )
    if(MSVC)
        target_compile_options(pm_hid_keyboard_test PRIVATE /EHsc /utf-8)
    endif()

    add_test(NAME hid_keyboard COMMAND pm_hid_keyboard_test)
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/copy_apk.cmake" "
set(CANDIDATES
    \"${CMAKE_SOURCE_DIR}/../Android/app/build/outputs/apk/debug/app-debug.apk\"
    \"${CMAKE_SOURCE_DIR}/../Android/app/build/intermediates/apk/debug/app-debug.apk\"
)
foreach(CAND IN LISTS CANDIDATES)
    if(EXISTS \"\${CAND}\")
        execute_process(COMMAND \${CMAKE_COMMAND} -E copy_if_different \"\${CAND}\" \"\${DEST}/PixelMirroring.apk\")
        break()
    endif()
endforeach()
")

add_custom_command(TARGET PixelMirroring POST_BUILD
    COMMAND ${CMAKE_COMMAND} -D DEST="$<TARGET_FILE_DIR:PixelMirroring>" -P "${CMAKE_CURRENT_BINARY_DIR}/copy_apk.cmake"
    COMMENT "Copy Android companion APK next to desktop client if it exists"
)

# --- Bootstrap Android Platform-Tools (ADB) ---
# The client ships its own adb, so users never need an Android SDK. If the bundled
# platform-tools are missing, download them from Google at configure time.
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
    message(STATUS "ADB not found at ${PM_PLATFORM_TOOLS_DIR}/${PM_ADB_EXPECTED_FILE}. Downloading from Google...")
    set(PM_TEMP_ZIP "${CMAKE_CURRENT_BINARY_DIR}/platform-tools.zip")
    
    file(DOWNLOAD "${PM_PLATFORM_TOOLS_URL}" "${PM_TEMP_ZIP}" SHOW_PROGRESS STATUS PM_DOWNLOAD_STATUS)
    list(GET PM_DOWNLOAD_STATUS 0 PM_DOWNLOAD_ERR)
    if(NOT PM_DOWNLOAD_ERR EQUAL 0)
        list(GET PM_DOWNLOAD_STATUS 1 PM_DOWNLOAD_MSG)
        message(FATAL_ERROR "Failed to download ADB: ${PM_DOWNLOAD_MSG}")
    endif()

    message(STATUS "Extracting ADB...")
    file(ARCHIVE_EXTRACT INPUT "${PM_TEMP_ZIP}" DESTINATION "${CMAKE_SOURCE_DIR}/vendor")
    file(REMOVE "${PM_TEMP_ZIP}")
    
    # The zip does not preserve the executable bit on POSIX.
    if(NOT WIN32)
        execute_process(COMMAND chmod +x "${PM_PLATFORM_TOOLS_DIR}/adb")
    endif()
    message(STATUS "ADB bootstrap finished successfully")
endif()

# --- Fossify Launcher for the virtual-display experiment ---
# EXPERIMENT. A display of our own needs a home screen, and the phone's own launcher for
# second displays is useless on a Pixel (see VIRTUAL-DISPLAY.md): its app drawer opens
# but tapping an app starts nothing. This one works, so it travels with the client and
# gets pushed onto the phone when the mode is switched on.
#
# Fetched at BUILD time, exactly like ADB above, so the repository stays free of a 5 MB
# binary. Two things to know before this leaves the experiment:
#   - it adds ~5 MB to an installer whose small size is a deliberate feature, and
#   - Fossify Launcher is GPL-3.0; shipping it inside the installer brings that
#     licence's obligations along.
set(PM_FOSSIFY_APK "${CMAKE_SOURCE_DIR}/vendor/FossifyLauncher.apk")
set(PM_FOSSIFY_URL "https://f-droid.org/repo/org.fossify.home_16.apk")
set(PM_FOSSIFY_SHA256 "a603d3d510482feafd73d52a93a1ea9baefd2ca0aae329a14cbf0e21f43638e3")

if(NOT EXISTS "${PM_FOSSIFY_APK}")
    message(STATUS "Ugg! Fossify Launcher not found. Downloading from F-Droid...")
    file(DOWNLOAD "${PM_FOSSIFY_URL}" "${PM_FOSSIFY_APK}"
         SHOW_PROGRESS
         EXPECTED_HASH SHA256=${PM_FOSSIFY_SHA256}
         STATUS PM_FOSSIFY_STATUS)
    list(GET PM_FOSSIFY_STATUS 0 PM_FOSSIFY_ERR)
    if(NOT PM_FOSSIFY_ERR EQUAL 0)
        list(GET PM_FOSSIFY_STATUS 1 PM_FOSSIFY_MSG)
        # Not fatal: everything except the virtual-display experiment works without it.
        file(REMOVE "${PM_FOSSIFY_APK}")
        message(WARNING "Ugg! Could not download Fossify Launcher: ${PM_FOSSIFY_MSG}")
    endif()
endif()

if(EXISTS "${PM_FOSSIFY_APK}")
    add_custom_command(TARGET PixelMirroring POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PM_FOSSIFY_APK}"
            "$<TARGET_FILE_DIR:PixelMirroring>/FossifyLauncher.apk"
        COMMENT "Copy Fossify Launcher next to desktop client"
    )
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
# The scrcpy server jar is pushed to the device at runtime, so it has to sit next to
# the executable.
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
    message(WARNING "scrcpy-server.jar not found in any of the expected locations")
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

# --- Runtime DLLs we actually load ---
# Copying the whole vcpkg bin directory shipped debug symbols (.pdb), the build tool
# pkgconf, and avfilter/avdevice which nothing ever loads — about 4 MB of dead weight
# in every installer. Only what the EXE (and the FFmpeg libs themselves) actually
# import is copied. Patterns rather than fixed names, so a version bump
# (avcodec-62 -> avcodec-63) does not silently drop a library.
set(PM_RUNTIME_DLL_PATTERNS
    "avcodec-*.dll"     # H.264/H.265/AV1 decode + BMP/x264 encode
    "avformat-*.dll"    # MP4 muxing for screen recordings
    "avutil-*.dll"
    "swscale-*.dll"     # pixel format conversion for captures
    "swresample-*.dll"  # pulled in by avcodec
    "libx264-*.dll"     # pulled in by avcodec
    "SDL2.dll"
    "brotlienc.dll"     # pulled in by cpp-httplib
    "brotlidec.dll"
    "brotlicommon.dll"
)

if(DEFINED VCPKG_TARGET_TRIPLET)
    set(PM_VCPKG_BIN "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
    set(PM_RUNTIME_DLLS "")
    foreach(PM_PATTERN IN LISTS PM_RUNTIME_DLL_PATTERNS)
        file(GLOB PM_MATCHED "${PM_VCPKG_BIN}/${PM_PATTERN}")
        list(APPEND PM_RUNTIME_DLLS ${PM_MATCHED})
    endforeach()

    if(PM_RUNTIME_DLLS)
        add_custom_command(TARGET PixelMirroring POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${PM_RUNTIME_DLLS}
                "$<TARGET_FILE_DIR:PixelMirroring>"
            COMMAND_EXPAND_LISTS
            COMMENT "Copy required vcpkg DLLs next to desktop client"
        )
    else()
        message(WARNING "No vcpkg runtime DLLs matched in ${PM_VCPKG_BIN}")
    endif()
endif()

if(WIN32)
    # Required for custom window frame and GDI drawing on Windows.
    # shell32 brings DragAcceptFiles/DragQueryFileW for files dropped onto the window.
    target_link_libraries(PixelMirroring PRIVATE dwmapi uxtheme gdi32 gdiplus user32 shell32)
    # GUI subsystem: no console window.
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

# Same short list as above — the installer ships exactly what gets loaded.
if(DEFINED VCPKG_TARGET_TRIPLET AND PM_RUNTIME_DLLS)
    install(FILES ${PM_RUNTIME_DLLS}
        DESTINATION .
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

# The home screen for the virtual-display experiment (see above).
install(FILES "$<TARGET_FILE_DIR:PixelMirroring>/FossifyLauncher.apk"
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
set(CPACK_PACKAGE_VENDOR "Pixel Mirroring")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Pixel Mirroring - Android-Bildschirm nativ auf dem PC")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Toaster187/Pixel-Mirroring")
set(CPACK_PACKAGE_CONTACT "https://github.com/Toaster187/Pixel-Mirroring/issues")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Pixel Mirroring")

# Without these, CPack's placeholder text shows up in the installer ("This is an
# installer created using CPack ... No license provided") and no licence at all.
# Point every page at the real texts.
set(PM_LICENSE_FOR_INSTALLER "${CMAKE_CURRENT_BINARY_DIR}/installer/License.txt")

# The repo LICENSE is UTF-8 and full of German umlauts (the FFmpeg LGPL statement).
# CPack builds an ANSI NSIS installer — it puts "Unicode true" nowhere, and the
# defines hook lands after OutFile, which NSIS rejects. So an ANSI installer reads
# the licence in the system code page: handing it UTF-8 would show "KonformitÃ¤t".
# Transcode once at configure time. This is fixing the cause, not spelling around it.
set(PM_LICENSE_TRANSCODED FALSE)
if(WIN32)
    execute_process(
        COMMAND powershell -NoProfile -NonInteractive -Command
            "[IO.File]::WriteAllText('${PM_LICENSE_FOR_INSTALLER}', [IO.File]::ReadAllText('${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE', [Text.Encoding]::UTF8), [Text.Encoding]::GetEncoding(1252))"
        RESULT_VARIABLE PM_LICENSE_RC
        ERROR_QUIET
    )
    if(PM_LICENSE_RC EQUAL 0 AND EXISTS "${PM_LICENSE_FOR_INSTALLER}")
        set(PM_LICENSE_TRANSCODED TRUE)
    else()
        message(WARNING "Could not transcode LICENSE to cp1252 - installer may show broken umlauts.")
    endif()
endif()

if(NOT PM_LICENSE_TRANSCODED)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" "${PM_LICENSE_FOR_INSTALLER}" COPYONLY)
endif()

set(CPACK_RESOURCE_FILE_LICENSE "${PM_LICENSE_FOR_INSTALLER}")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/installer/Welcome.txt")
set(CPACK_RESOURCE_FILE_WELCOME "${CMAKE_CURRENT_SOURCE_DIR}/installer/Welcome.txt")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/installer/Description.txt")

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS;WIX")

    # LZMA in solid mode compresses all files as a single block, which pays off here
    # because the APK and the FFmpeg libraries compress well together.
    set(CPACK_NSIS_COMPRESSOR "/SOLID lzma")
    set(CPACK_ARCHIVE_THREADS 0)

    # Executables to create shortcuts for
    set(CPACK_PACKAGE_EXECUTABLES "PixelMirroring" "Pixel Mirroring")
    
    # NSIS specific settings
    set(CPACK_NSIS_PACKAGE_NAME "Pixel Mirroring")
    set(CPACK_NSIS_DISPLAY_NAME "Pixel Mirroring ${PROJECT_VERSION}")
    # These pointed at a user that does not own this repo — the "Hilfe" links in
    # Windows' installed-apps list went nowhere.
    set(CPACK_NSIS_HELP_LINK "https://github.com/Toaster187/Pixel-Mirroring")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/Toaster187/Pixel-Mirroring")
    set(CPACK_NSIS_CONTACT "https://github.com/Toaster187/Pixel-Mirroring/issues")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    # Installer + Start menu shortcut wear the same icon as the app
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "PixelMirroring.exe")
    set(CPACK_NSIS_BRANDING_TEXT "Pixel Mirroring ${PROJECT_VERSION}")

    # Titles and body text for the welcome/finish pages. CPack has no variable for
    # the page BODY, so it goes in through the defines hook, which lands in the .nsi
    # before the pages are declared.
    set(CPACK_NSIS_WELCOME_TITLE "Pixel Mirroring installieren")
    set(CPACK_NSIS_FINISH_TITLE "Pixel Mirroring ist installiert")

    # This value passes through two parsers before NSIS sees it: CPack writes it into
    # CPackConfig.cmake WITHOUT escaping, and CMake then re-parses that file. So
    # everything has to be escaped for the SECOND pass:
    #   \\\"     -> lands as \" in CPackConfig -> becomes " in the .nsi
    #   $\\\\r   -> lands as $\\r             -> becomes $\r in the .nsi (NSIS newline)
    # Do not "simplify" the backslashes here without repackaging and looking at the
    # generated project.nsi — a wrong count silently breaks the whole installer.
    #
    # NOTE: these strings end up in an ANSI .nsi, so they are worded so that no
    # umlaut is needed at all. That is word choice. Do NOT "fix" German text
    # anywhere else by spelling ue/oe/ae — see the encoding rules in CLAUDE.md.
    set(CPACK_NSIS_DEFINES "
!define MUI_WELCOMEPAGE_TEXT \\\"Dieser Assistent installiert Pixel Mirroring auf diesem PC.$\\\\r$\\\\n$\\\\r$\\\\nDie Android-App ist bereits enthalten und wird beim ersten Verbinden per USB automatisch auf dem Handy installiert. Ein Android-SDK ist nicht notwendig.$\\\\r$\\\\n$\\\\r$\\\\nNach der Ersteinrichtung verbindet sich der Client automatisch per WLAN.\\\"
!define MUI_FINISHPAGE_TEXT \\\"Pixel Mirroring wurde installiert.$\\\\r$\\\\n$\\\\r$\\\\nVerbinden Sie das Handy per USB-Kabel, starten Sie Pixel Mirroring und klicken Sie auf Verbinden. Das Kabel wird nur einmal zur Einrichtung gebraucht.\\\"
")

    # Offer a desktop shortcut, and let the uninstaller take its own leftovers with it.
    set(CPACK_CREATE_DESKTOP_LINKS "PixelMirroring")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$DESKTOP\\\\Pixel Mirroring.lnk'")

    # NOTE: deliberately no CPACK_NSIS_MUI_FINISHPAGE_RUN. The installer runs
    # elevated, so launching the app from it would write the first-run setup state
    # into the ADMINISTRATOR's %LOCALAPPDATA% — and the user's own launch would then
    # not find it. Better to let people start it themselves.

    # WiX specific settings (Official Windows Installer)
    set(CPACK_WIX_UPGRADE_GUID "D9F369E9-1A1A-4BE6-B6DF-D7E31BEF8CFD")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Pixel Mirroring")
    set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.ico")
    set(CPACK_WIX_PROPERTY_ARPHELPLINK "https://github.com/Toaster187/Pixel-Mirroring")
    set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/Toaster187/Pixel-Mirroring")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
