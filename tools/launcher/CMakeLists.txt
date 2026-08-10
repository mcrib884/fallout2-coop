# Fallout 2 co-op launcher (tools/launcher)
#
# Standalone installer/launcher UI rendered with SDL2 + stb_truetype.
# Reuses the SDL2 wiring from the root project (vendored or system).

set(LAUNCHER_TARGET fallout2coop_launcher)

# Embed the Pixelify Sans font (SIL OFL) as a C array so the launcher is a single binary.
set(LAUNCHER_FONT_IN "${CMAKE_CURRENT_SOURCE_DIR}/fonts/PixelifySans.ttf")
set(LAUNCHER_FONT_OUT "${CMAKE_CURRENT_BINARY_DIR}/font_data.h")
add_custom_command(
    OUTPUT "${LAUNCHER_FONT_OUT}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT_FILE=${LAUNCHER_FONT_IN}"
        "-DOUTPUT_FILE=${LAUNCHER_FONT_OUT}"
        "-DARRAY_NAME=g_launcher_font_ttf"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2header.cmake"
    DEPENDS "${LAUNCHER_FONT_IN}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2header.cmake"
    COMMENT "Embedding Pixelify Sans font into ${LAUNCHER_TARGET}"
    VERBATIM
)

add_executable(${LAUNCHER_TARGET} WIN32)
target_sources(${LAUNCHER_TARGET} PRIVATE
    "launcher_main.cc"
    "ui.cc"
    "ui.h"
    "game_config_editor.cc"
    "game_config_editor.h"
    "detect.cc"
    "detect.h"
    "install.cc"
    "install.h"
    "launcher_config.cc"
    "launcher_config.h"
    "${CMAKE_SOURCE_DIR}/third_party/tinyfiledialogs/tinyfiledialogs.c"
    "${LAUNCHER_FONT_OUT}"
)
target_include_directories(${LAUNCHER_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party/stb_truetype"
    "${CMAKE_SOURCE_DIR}/third_party/tinyfiledialogs"
    ${SDL2_INCLUDE_DIRS}
)
target_compile_features(${LAUNCHER_TARGET} PRIVATE cxx_std_17)
target_link_libraries(${LAUNCHER_TARGET} PRIVATE
    ${SDL2_LIBRARIES}
    ${SDL2_MAIN_LIBRARIES}
)

find_package(Threads REQUIRED)
target_link_libraries(${LAUNCHER_TARGET} PRIVATE Threads::Threads)

if(WIN32)
    # Native file/message dialogs used by tinyfiledialogs.
    target_link_libraries(${LAUNCHER_TARGET} PRIVATE comdlg32 ole32 shell32)
endif()
