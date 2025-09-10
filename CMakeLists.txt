# Example GUI App CMakeLists.txt

# To get started on a new GUI app, copy this entire folder (containing this file and C++ sources) to
# a convenient location, and then start making modifications. For other examples of CMakeLists for
# GUI apps, check `extras/Projucer` and `examples/DemoRunner` in the JUCE repo.

# The first line of any CMake project should be a call to `cmake_minimum_required`, which checks
# that the installed CMake will be able to understand the following CMakeLists, and ensures that
# CMake's behaviour is compatible with the named version. This is a standard CMake command, so more
# information can be found in the CMake docs.

cmake_minimum_required(VERSION 3.15)

# Performance optimization: Enable ccache if available
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache for faster compilation")
endif()

project(GUI_APP_EXAMPLE VERSION 0.0.1)

# Performance optimization: Enable parallel builds
include(ProcessorCount)
ProcessorCount(N)
if(NOT N EQUAL 0)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${N})
    message(STATUS "Using ${N} parallel build jobs")
endif()

# Performance optimization: Use pre-compiled headers and unity builds when possible
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags for release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -march=native")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG -march=native")
    message(STATUS "Optimized build flags enabled")
endif()

# This project now builds only the Qt frontend
add_subdirectory(JUCE)                    # keep JUCE as a library for backend functionality

# Since we only use JUCE as an audio backend for Qt, disable JUCE GUI modules to avoid
# pulling in GTK headers on Linux.
set(JUCE_RECOMMENDED_GLOBAL_OPTIONS "JUCE_WEB_BROWSER=0;JUCE_USE_CURL=1;JUCE_USE_XINERAMA=0;JUCE_USE_XRANDR=0;JUCE_USE_XRENDER=0;JUCE_USE_XCURSOR=0")

# Qt frontend target that uses JUCE as backend for audio/waveform rendering
# Prefer Qt6, but provide a Qt5 fallback if Qt6 OpenGLWidgets is unavailable
set(QT_PACKAGE "")
find_package(Qt6 QUIET COMPONENTS Widgets OpenGLWidgets)
if (Qt6_FOUND)
    set(QT_PACKAGE "Qt6")
    message(STATUS "Using Qt6 with OpenGLWidgets")
else()
    message(STATUS "Qt6 not found; trying Qt5 fallback")
    find_package(Qt5 REQUIRED COMPONENTS Widgets OpenGL)
    set(QT_PACKAGE "Qt5")
    message(STATUS "Using Qt5 with OpenGL module")
endif()
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(AUBIO QUIET aubio)
pkg_check_modules(RUBBERBAND QUIET rubberband)

# Try common webkit2 pkg-config names (4.1 then 4.0)
pkg_check_modules(WEBKIT2_41 QUIET webkit2gtk-4.1)
if (WEBKIT2_41_FOUND)
    set(WEBKIT2_FOUND TRUE)
    set(WEBKIT2_INCLUDE_DIRS ${WEBKIT2_41_INCLUDE_DIRS})
    set(WEBKIT2_LIBRARIES ${WEBKIT2_41_LIBRARIES})
    set(WEBKIT2_CFLAGS_OTHER ${WEBKIT2_41_CFLAGS_OTHER})
else()
    pkg_check_modules(WEBKIT2_40 QUIET webkit2gtk-4.0)
    if (WEBKIT2_40_FOUND)
        set(WEBKIT2_FOUND TRUE)
        set(WEBKIT2_INCLUDE_DIRS ${WEBKIT2_40_INCLUDE_DIRS})
        set(WEBKIT2_LIBRARIES ${WEBKIT2_40_LIBRARIES})
        set(WEBKIT2_CFLAGS_OTHER ${WEBKIT2_40_CFLAGS_OTHER})
    else()
        set(WEBKIT2_FOUND FALSE)
    endif()
endif()

juce_add_console_app(David
    PRODUCT_NAME "David")

juce_generate_juce_header(David)

# Enable MP3 support in JUCE
target_compile_definitions(David PRIVATE 
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_INCLUDE_ZLIB_CODE=1)

target_sources(David
    PRIVATE
    src/QtMain.cpp
    src/QtMainWindow.cpp
    src/QtMainWindow.h
    src/QtDeckWidget.cpp
    src/QtDeckWidget.h
    src/QtTurntableWidget.cpp
    src/QtTurntableWidget.h
    src/BeatIndicator.cpp
    src/BeatIndicator.h
    src/DraggableListWidget.h
    src/WaveformDisplay.cpp
    src/WaveformDisplay.h
    src/DeckWaveformOverview.cpp
    src/DeckWaveformOverview.h
    src/PerformancePads.cpp
    src/PerformancePads.h
    src/DJAudioPlayer.cpp
    src/WaveformGenerator.cpp
    src/WaveformGenerator.h
    src/BpmAnalyzer.cpp
    src/LibraryManager.cpp
    src/LibraryManager.h
    src/MasterLevelMonitor.cpp
    src/MasterLevelMonitor.h
    src/MenuBar.cpp
    src/MenuBar.h
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
    src/AppConfig.h
    src/DeckSettings.h)
    # AudioMixer removed - using simplified AudioSourcePlayer approach

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
    $<$<STREQUAL:${QT_PACKAGE},Qt6>:Qt6::Widgets>
    $<$<STREQUAL:${QT_PACKAGE},Qt6>:Qt6::OpenGLWidgets>
    $<$<STREQUAL:${QT_PACKAGE},Qt5>:Qt5::Widgets>
    $<$<STREQUAL:${QT_PACKAGE},Qt5>:Qt5::OpenGL>)

if (AUBIO_FOUND)
    message(STATUS "Aubio found: including headers and linking libs")
    target_include_directories(David PRIVATE ${AUBIO_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${AUBIO_LIBRARIES})
    target_compile_options(David PRIVATE ${AUBIO_CFLAGS_OTHER})
    target_compile_definitions(David PRIVATE AUBIO_FOUND=1)
else()
    message(STATUS "Aubio not found - building with fallback BPM analyzer")
endif()

# Require Rubber Band time-stretcher for high-quality keylock
find_package(PkgConfig REQUIRED)
pkg_check_modules(RUBBERBAND REQUIRED rubberband)
if (RUBBERBAND_FOUND)
    message(STATUS "Rubber Band found: enabling high-quality keylock")
    target_include_directories(David PRIVATE ${RUBBERBAND_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${RUBBERBAND_LIBRARIES})
    target_compile_options(David PRIVATE ${RUBBERBAND_CFLAGS_OTHER})
    target_compile_definitions(David PRIVATE RUBBERBAND_FOUND=1)
else()
    message(FATAL_ERROR "Rubber Band is required but not found. Please install rubberband-dev")
endif()

# Add GTK/WebKit include/link flags if found via pkg-config
if (GTK3_FOUND)
    target_include_directories(David PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${GTK3_LIBRARIES})
    target_compile_options(David PRIVATE ${GTK3_CFLAGS_OTHER})
endif()

if (MPG123_FOUND)
    target_include_directories(David PRIVATE ${MPG123_INCLUDE_DIRS})
    target_compile_options(David PRIVATE ${MPG123_CFLAGS_OTHER})
endif()

if (WEBKIT2_FOUND)
    target_include_directories(David PRIVATE ${WEBKIT2_INCLUDE_DIRS})
    target_link_libraries(David PRIVATE ${WEBKIT2_LIBRARIES})
    target_compile_options(David PRIVATE ${WEBKIT2_CFLAGS_OTHER})
endif()

set_target_properties(David PROPERTIES
    CXX_STANDARD 17
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Make project headers available to the Qt target
target_include_directories(David PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
