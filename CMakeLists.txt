cmake_minimum_required(VERSION 3.22)
project(OpenRack VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(OpenRack
    PRODUCT_NAME "OpenRack"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "OpenRack needs audio input to process your signal.")

target_sources(OpenRack PRIVATE Source/Main.cpp)

target_compile_definitions(OpenRack PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OpenRack,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OpenRack,JUCE_VERSION>")

if(APPLE)
    target_compile_definitions(OpenRack PRIVATE JUCE_PLUGINHOST_AU=1)
endif()

target_link_libraries(OpenRack PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

#------------------------------------------------------------------------------
# Plugin version: OpenRack as an effect plugin that runs inside a DAW.
#   VST3 for FL Studio / Ableton (Windows, macOS, Linux)
#   AU   for Logic Pro (macOS only; Logic does not load VST3)
set(OPENRACK_PLUGIN_FORMATS VST3)
if(APPLE)
    list(APPEND OPENRACK_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(OpenRackPlugin
    PRODUCT_NAME "OpenRack"
    COMPANY_NAME "OpenRack"
    BUNDLE_ID "com.openrack.OpenRack"
    PLUGIN_MANUFACTURER_CODE Orck
    PLUGIN_CODE Orak
    FORMATS ${OPENRACK_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(OpenRackPlugin PRIVATE Source/PluginProcessor.cpp)

target_compile_definitions(OpenRackPlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=1)

if(APPLE)
    target_compile_definitions(OpenRackPlugin PUBLIC JUCE_PLUGINHOST_AU=1)
endif()

target_link_libraries(OpenRackPlugin PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

#------------------------------------------------------------------------------
# OpenRack Bridge: a virtual audio device driver for macOS (CoreAudio
# AudioServerPlugIn). Install the built OpenRackBridge.driver into
# /Library/Audio/Plug-Ins/HAL/ and restart coreaudiod.
if(APPLE)
    add_library(OpenRackBridge MODULE Driver/OpenRackBridge.c)
    set_target_properties(OpenRackBridge PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION driver
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Driver/Info.plist"
        C_STANDARD 11)
    target_link_libraries(OpenRackBridge PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation")
endif()
