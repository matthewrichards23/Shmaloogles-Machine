cmake_minimum_required(VERSION 3.22)

# Universal binary: runs natively on Apple Silicon (M1-M4) AND Intel Macs.
# These must be set before project().
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal build")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")

project(SmilingBuildup VERSION 3.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_binary_data(SmileArt SOURCES
    Artwork/Pim.png Artwork/Charlie.png Artwork/Allan.png
    Artwork/Glep.png Artwork/MrBoss.png Artwork/MrFrog.png
    Artwork/Logo.png)

juce_add_plugin(SmilingBuildup
    COMPANY_NAME "DJ MJ"
    PLUGIN_MANUFACTURER_CODE Djmj
    PLUGIN_CODE Sbld
    FORMATS VST3 AU
    PRODUCT_NAME "Smiling Buildup"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(SmilingBuildup PRIVATE Source/Plugin.cpp Source/Dsp.h)

target_compile_definitions(SmilingBuildup PRIVATE
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SmilingBuildup
    PRIVATE
        SmileArt
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
