# Example GUI App CMakeLists.txt

# To get started on a new GUI app, copy this entire folder (containing this file and C++ sources) to
# a convenient location, and then start making modifications. For other examples of CMakeLists for
# GUI apps, check `extras/Projucer` and `examples/DemoRunner` in the JUCE repo.

# The first line of any CMake project should be a call to `cmake_minimum_required`, which checks
# that the installed CMake will be able to understand the following CMakeLists, and ensures that
# CMake's behaviour is compatible with the named version. This is a standard CMake command, so more
# information can be found in the CMake docs.

cmake_minimum_required(VERSION 3.21)

project(GUI_APP_EXAMPLE VERSION 0.0.1)

# Build acceleration: use ccache if present
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache for faster compilation")
endif()

# Leverage all logical cores unless the user already configured a limit
include(ProcessorCount)
ProcessorCount(PROCESSOR_COUNT)
if(PROCESSOR_COUNT GREATER 0 AND NOT DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    set(CMAKE_BUILD_PARALLEL_LEVEL "${PROCESSOR_COUNT}")
    message(STATUS "Using ${PROCESSOR_COUNT} parallel build jobs")
endif()

# Compiler configuration
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG -march=native")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -DNDEBUG -march=native")
    message(STATUS "Optimized build flags enabled")
endif()

# Backend: JUCE provides audio and DSP functionality
add_subdirectory(JUCE)

# Keep JUCE's GUI modules disabled; we only need the audio stack
set(JUCE_RECOMMENDED_GLOBAL_OPTIONS
    "JUCE_WEB_BROWSER=0;JUCE_USE_CURL=1;JUCE_USE_XINERAMA=0;JUCE_USE_XRANDR=0;JUCE_USE_XRENDER=0;JUCE_USE_XCURSOR=0")

# Frontend: require Qt6 (Qt6 is well-established; simplifies build)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Sql)
message(STATUS "Using Qt6 with OpenGLWidgets")

find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)

# Platform-specific dependencies
if(UNIX AND NOT APPLE)
    # Linux: GTK3 required for JUCE gui_extra (if ever needed)
    pkg_check_modules(GTK3 gtk+-3.0)
    if(GTK3_FOUND)
        message(STATUS "GTK3 found on Linux")
    endif()
endif()

# Cross-platform optional dependencies
pkg_check_modules(AUBIO QUIET aubio)
pkg_check_modules(RUBBERBAND QUIET rubberband)
pkg_check_modules(TAGLIB QUIET taglib)

juce_add_console_app(David
    PRODUCT_NAME "David")

juce_generate_juce_header(David)

target_compile_definitions(David PRIVATE
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_INCLUDE_ZLIB_CODE=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1
    JUCE_USE_XINERAMA=0
    JUCE_USE_XRANDR=0
    JUCE_USE_XRENDER=0
    JUCE_USE_XCURSOR=0)

set(DAVID_SOURCES
    src/Main.cpp
    src/MainWindow.cpp
    src/MainWindowDeckState.cpp
    src/MainWindowScratch.cpp
    src/MainWindowWindowing.cpp
    src/MainWindowDeckSettings.cpp
    src/MainWindow.h
    src/DJKnob.cpp
    src/DJKnob.h
    src/VUMeter.cpp
    src/VUMeter.h
    src/CustomFader.cpp
    src/CustomFader.h
    src/DeckWidget.cpp
    src/DeckWidget.h
    src/TurntableWidget.cpp
    src/TurntableWidget.h
    src/BeatIndicator.cpp
    src/BeatIndicator.h
    src/DraggableListWidget.h
    src/WaveformDisplay.cpp
    src/WaveformDisplay.h
    src/DeckWaveformOverview.cpp
    src/DeckWaveformOverview.h
    src/PerformancePads.cpp
    src/PerformancePads.h
    src/ScratchEngine.cpp
    src/ScratchEngine.h
    src/DJAudioPlayer.cpp
    src/WaveformGenerator.cpp
    src/WaveformGenerator.h
    src/StereoAudioCallback.cpp
    src/StereoAudioCallback.h
    src/MainWindowTasks.cpp
    src/MainWindowTasks.h
    src/BpmAnalyzer.cpp
    src/AudioFormatGuard.cpp
    src/AudioFormatGuard.h
    src/LibraryManager.cpp
    src/LibraryManager.h
    src/LibraryMetadata.cpp
    src/LibraryTableModel.cpp
    src/LibraryTableView.cpp
    src/LibraryDatabase.cpp
    src/LibraryDatabase.h
    src/MasterLevelMonitor.cpp
    src/MasterLevelMonitor.h
    src/MenuBar.cpp
    src/MenuBar.h
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
    src/MidiEngine.cpp
    src/MidiEngine.h
    src/CoverArtExtractor.cpp
    src/CoverArtExtractor.h
    src/AppConfig.h
    src/DeckSettings.h)

target_sources(David PRIVATE ${DAVID_SOURCES})

target_link_libraries(David
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_graphics
        juce::juce_dsp
        CURL::libcurl
        Qt6::Widgets
        Qt6::OpenGLWidgets
        Qt6::Sql)

if(AUBIO_FOUND)
    message(STATUS "Aubio found: including headers and linking libs")
    target_include_directories(David PRIVATE ${AUBIO_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${AUBIO_LIBRARIES})
    target_compile_options(David PRIVATE ${AUBIO_CFLAGS_OTHER})
    target_compile_definitions(David PRIVATE AUBIO_FOUND=1)
else()
    message(STATUS "Aubio not found - building with fallback BPM analyzer")
endif()

if(TAGLIB_FOUND)
    message(STATUS "TagLib found: enabling cover art extraction")
    target_include_directories(David PRIVATE ${TAGLIB_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${TAGLIB_LIBRARIES})
    target_compile_options(David PRIVATE ${TAGLIB_CFLAGS_OTHER})
    target_compile_definitions(David PRIVATE TAGLIB_FOUND=1)
else()
    message(STATUS "TagLib not found - cover art extraction disabled")
endif()

if(RUBBERBAND_FOUND)
    message(STATUS "Rubber Band found: enabling high-quality keylock")
    target_include_directories(David PRIVATE ${RUBBERBAND_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${RUBBERBAND_LIBRARIES})
    target_compile_options(David PRIVATE ${RUBBERBAND_CFLAGS_OTHER})
    target_compile_definitions(David PRIVATE RUBBERBAND_FOUND=1)
else()
    message(WARNING "Rubber Band not found - keylock quality will be reduced")
endif()

# Linux-specific: JUCE's juce_gui_extra expects GTK (not needed for this project)
if(GTK3_FOUND)
    target_include_directories(David PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${GTK3_LIBRARIES})
    target_compile_options(David PRIVATE ${GTK3_CFLAGS_OTHER})
endif()

# Removed unused MPG123 and WebKit2 blocks (no references in source)

set_target_properties(David PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

target_include_directories(David PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
