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
