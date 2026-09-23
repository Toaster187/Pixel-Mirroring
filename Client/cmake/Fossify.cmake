# Binary identity and source attribution are updated together.
set(PM_FOSSIFY_VERSION "1.10.0")
set(PM_FOSSIFY_VERSION_CODE "16")
set(PM_FOSSIFY_COMMIT "2191e6401681065fcbc1ffdb85e951764e9b1269")
set(PM_FOSSIFY_SHA256 "a603d3d510482feafd73d52a93a1ea9baefd2ca0aae329a14cbf0e21f43638e3")
set(PM_FOSSIFY_URLS
    "https://f-droid.org/repo/org.fossify.home_${PM_FOSSIFY_VERSION_CODE}.apk"
    "https://f-droid.org/archive/org.fossify.home_${PM_FOSSIFY_VERSION_CODE}.apk")

file(READ "${CMAKE_CURRENT_LIST_DIR}/../vendor/licenses/FossifyLauncher.md" PM_FOSSIFY_NOTICE)
foreach(PM_IDENTITY IN ITEMS
        "${PM_FOSSIFY_VERSION} (versionCode ${PM_FOSSIFY_VERSION_CODE})"
        "${PM_FOSSIFY_COMMIT}" "${PM_FOSSIFY_SHA256}")
    string(FIND "${PM_FOSSIFY_NOTICE}" "${PM_IDENTITY}" PM_FOUND)
    if(PM_FOUND EQUAL -1)
        message(FATAL_ERROR "FossifyLauncher.md does not match the pinned APK: ${PM_IDENTITY}")
    endif()
endforeach()
