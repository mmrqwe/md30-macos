cmake_minimum_required(VERSION 3.28)
project(md30 VERSION 0.1.8 LANGUAGES C CXX)

if(NOT APPLE)
    message(FATAL_ERROR "md30 requires macOS and IOKit")
endif()

find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
find_library(CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(CORE_AUDIO_FRAMEWORK CoreAudio REQUIRED)

add_library(md30-transport STATIC src/usb_device.c src/usb_runtime.c)
target_include_directories(md30-transport PUBLIC src)
target_compile_features(md30-transport PRIVATE c_std_17)
target_compile_options(md30-transport PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations)
target_link_libraries(md30-transport PUBLIC "${IOKIT_FRAMEWORK}" "${CORE_FOUNDATION_FRAMEWORK}")
set_target_properties(md30-transport PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(md30-usb src/usb_probe.c src/wav_pcm.c src/exclusive_play.c src/exclusive_owner.c)
target_compile_features(md30-usb PRIVATE c_std_17)
target_compile_options(md30-usb PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations)
target_link_libraries(md30-usb PRIVATE md30-transport "${CORE_AUDIO_FRAMEWORK}")
install(TARGETS md30-usb RUNTIME DESTINATION bin)

include(FetchContent)
set(BUILD_TESTING OFF CACHE BOOL "Build libASPL tests")
FetchContent_Declare(libaspl
    GIT_REPOSITORY https://github.com/gavv/libASPL.git
    GIT_TAG 47f688ed6bb637ab8b7f4b36864734b2b1f69b6b
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libaspl)

add_library(MD30 MODULE src/hal/driver.cpp src/hal/playback.cpp src/hal/exclusive.cpp src/hal/bulk_output.cpp)
target_compile_features(MD30 PRIVATE cxx_std_17)
target_compile_options(MD30 PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
target_link_libraries(MD30 PRIVATE libASPL md30-transport)
configure_file(config/DriverInfo.plist.in DriverInfo.plist @ONLY)
set_target_properties(MD30 PROPERTIES
    BUNDLE TRUE BUNDLE_EXTENSION driver PREFIX "" SUFFIX ""
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/DriverInfo.plist"
    LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/DriverInfo.plist"
    CXX_VISIBILITY_PRESET hidden)
add_custom_command(TARGET MD30 POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:MD30>/Contents/Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy "${libaspl_SOURCE_DIR}/LICENSE" "$<TARGET_BUNDLE_DIR:MD30>/Contents/Resources/libASPL-LICENSE.txt"
    COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:MD30>"
    VERBATIM)
install(DIRECTORY "$<TARGET_BUNDLE_DIR:MD30>" DESTINATION . USE_SOURCE_PERMISSIONS)

set(PANEL_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/MD30 Panel.app")
set(PANEL_MACOS "${PANEL_BUNDLE}/Contents/MacOS")
set(PANEL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/panel/MD30Panel.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/panel/AudioOutput.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/panel/TestTone.swift")
configure_file(config/PanelInfo.plist.in PanelInfo.plist @ONLY)
install(FILES config/local.md30.autolaunch.plist DESTINATION launchd)
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE MD30_SDK OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND uname -m OUTPUT_VARIABLE MD30_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
add_custom_command(OUTPUT "${PANEL_MACOS}/MD30Panel"
    BYPRODUCTS "${PANEL_MACOS}/md30-watch"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PANEL_MACOS}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/PanelInfo.plist" "${PANEL_BUNDLE}/Contents/Info.plist"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:md30-usb>" "${PANEL_MACOS}/md30-usb"
    COMMAND xcrun swiftc -sdk "${MD30_SDK}" -target "${MD30_ARCH}-apple-macos13.0"
        $<IF:$<CONFIG:Debug>,-g,-O> -parse-as-library ${PANEL_SOURCES} -o "${PANEL_MACOS}/MD30Panel"
    COMMAND xcrun swiftc -sdk "${MD30_SDK}" -target "${MD30_ARCH}-apple-macos13.0"
        $<IF:$<CONFIG:Debug>,-g,-O> -parse-as-library
        "${CMAKE_CURRENT_SOURCE_DIR}/src/panel/AudioOutput.swift"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/autolaunch/USBWatch.swift" -o "${PANEL_MACOS}/md30-watch"
    COMMAND /usr/bin/codesign --force --sign - "${PANEL_BUNDLE}"
    DEPENDS ${PANEL_SOURCES} src/autolaunch/USBWatch.swift "${CMAKE_CURRENT_BINARY_DIR}/PanelInfo.plist" md30-usb
    VERBATIM)
add_custom_target(MD30Panel ALL DEPENDS "${PANEL_MACOS}/MD30Panel")
install(DIRECTORY "${PANEL_BUNDLE}" DESTINATION . USE_SOURCE_PERMISSIONS)

add_executable(md30-hal-smoke EXCLUDE_FROM_ALL tests/hal_smoke.cpp)
target_compile_features(md30-hal-smoke PRIVATE cxx_std_17)
target_link_libraries(md30-hal-smoke PRIVATE "${CORE_FOUNDATION_FRAMEWORK}")

add_executable(md30-mixer-smoke EXCLUDE_FROM_ALL tests/mixer_smoke.c)
target_compile_features(md30-mixer-smoke PRIVATE c_std_17)
target_compile_options(md30-mixer-smoke PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
target_link_libraries(md30-mixer-smoke PRIVATE md30-transport)
