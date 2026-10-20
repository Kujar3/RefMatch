cmake_minimum_required(VERSION 3.22)

project(RefMatch VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
    add_compile_options(/MP /utf-8)
endif()

set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_PATH}" JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Source lists (globbed per module so new files are picked up on the next configure)
# ---------------------------------------------------------------------------
file(GLOB RM_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
file(GLOB RM_PLUGIN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/*.cpp")
file(GLOB RM_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp")
file(GLOB RM_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

# ---------------------------------------------------------------------------
# Embedded resources (fonts, logo)
# ---------------------------------------------------------------------------
file(GLOB RM_RESOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/images/*.png")
juce_add_binary_data(RefMatchData
    NAMESPACE RefMatchBinary
    HEADER_NAME RefMatchBinary.h
    SOURCES ${RM_RESOURCE_FILES})
set_target_properties(RefMatchData PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(RM_JUCE_MODULES
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_extra)

set(RM_JUCE_DEFINES
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1)

# ---------------------------------------------------------------------------
# The plugin (VST3 + Standalone)
# ---------------------------------------------------------------------------
juce_add_plugin(RefMatch
    COMPANY_NAME "Kujar3"
    PRODUCT_NAME "RefMatch"
    BUNDLE_ID "com.kujar3.refmatch"
    PLUGIN_MANUFACTURER_CODE Kuja
    PLUGIN_CODE Rfmt
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Analyzer
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/logo_512.png"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(RefMatch PRIVATE ${RM_CORE_SOURCES} ${RM_PLUGIN_SOURCES} ${RM_UI_SOURCES})
target_include_directories(RefMatch PRIVATE src)
target_compile_definitions(RefMatch PUBLIC ${RM_JUCE_DEFINES})
target_link_libraries(RefMatch
    PRIVATE RefMatchData ${RM_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Pure C++ core unit tests (no JUCE, compiles in seconds)
# ---------------------------------------------------------------------------
add_executable(CoreTests ${RM_TEST_SOURCES} ${RM_CORE_SOURCES})
target_include_directories(CoreTests PRIVATE src tests)
if(MSVC)
    target_compile_options(CoreTests PRIVATE /W4 /permissive-)
endif()

# ---------------------------------------------------------------------------
# Dev tool: processor self-test + editor snapshot renderer
#   RefMatchDev selftest
#   RefMatchDev snapshot <outDir>
# ---------------------------------------------------------------------------
juce_add_console_app(RefMatchDev PRODUCT_NAME "RefMatchDev")
file(GLOB RM_DEV_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/dev/*.cpp")
target_sources(RefMatchDev PRIVATE ${RM_DEV_SOURCES} ${RM_CORE_SOURCES} ${RM_PLUGIN_SOURCES} ${RM_UI_SOURCES})
target_include_directories(RefMatchDev PRIVATE src)
target_compile_definitions(RefMatchDev PRIVATE
    ${RM_JUCE_DEFINES}
    JucePlugin_Name="RefMatch"
    REFMATCH_DEV_TOOL=1)
target_link_libraries(RefMatchDev
    PRIVATE RefMatchData ${RM_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Optional CLAP format (clap-juce-extensions). Only active when the checkout exists
# (scripts\fetch_clap.bat clones the pinned version).
#   scripts\build.bat build-clap RefMatch_CLAP
#   -> build-clap\RefMatch_artefacts\Release\CLAP\RefMatch.clap
# ---------------------------------------------------------------------------
option(REFMATCH_CLAP "Build the CLAP format (needs third_party/clap-juce-extensions)" ON)
set(REFMATCH_CLAP_EXT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap-juce-extensions"
    CACHE PATH "Path to the clap-juce-extensions checkout (with submodules)")
if(REFMATCH_CLAP)
    if(EXISTS "${REFMATCH_CLAP_EXT_PATH}/CMakeLists.txt"
       AND EXISTS "${REFMATCH_CLAP_EXT_PATH}/clap-libs/clap/CMakeLists.txt"
       AND EXISTS "${REFMATCH_CLAP_EXT_PATH}/clap-libs/clap-helpers/CMakeLists.txt")
        add_subdirectory("${REFMATCH_CLAP_EXT_PATH}" clap-juce-extensions EXCLUDE_FROM_ALL)
        clap_juce_extensions_plugin(TARGET RefMatch
            CLAP_ID "com.kujar3.refmatch"
            CLAP_FEATURES audio-effect analyzer utility stereo)
        message(STATUS "RefMatch: CLAP enabled (target RefMatch_CLAP)")
    else()
        message(STATUS "RefMatch: CLAP disabled, ${REFMATCH_CLAP_EXT_PATH} not found "
                       "(run scripts/fetch_clap.bat to enable it)")
    endif()
endif()
