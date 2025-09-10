#pragma once

#include <JuceHeader.h>
#include <functional>
#include "GlobalBeatGrid.h"

class BpmAnalyzer {
public:
    explicit BpmAnalyzer(juce::AudioFormatManager& fm) : formatManager(fm) {}
    /** Analyze file and return estimated BPM (0 if unknown).
        Optionally fills `outBeatsSeconds` with beat timestamps (seconds) and `outTotalLengthSeconds` with the file length.
        Also optionally returns the algorithm method name used for detection.
        `outFirstBeatOffset` receives the time offset (in seconds) of the first actual beat in the track.
        
        NEW: Automatically updates the global beat grid with analyzed results.
    */
    // Optional progress callback: invoked with values 0..1 from worker thread
    using ProgressFn = std::function<void(double)>;
    using StatusFn = std::function<void(const std::string&)>;
    double analyzeFile(const juce::File& file, double maxSecondsToAnalyze = 120.0,
                       std::vector<double>* outBeatsSeconds = nullptr,
                       double* outTotalLengthSeconds = nullptr,
                       std::string* outAlgorithmUsed = nullptr,
                       double* outFirstBeatOffset = nullptr,
                       ProgressFn progress = nullptr,
                       StatusFn errorOut = nullptr);

    // NEW: Set whether this analyzer should update the global beat grid
    void setUpdateGlobalBeatGrid(bool update) { updateGlobalGrid = update; }

private:
    juce::AudioFormatManager& formatManager;
    bool updateGlobalGrid{true}; // Default: update global grid on analysis
};
