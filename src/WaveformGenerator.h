#pragma once

#include <vector>
#include <JuceHeader.h>

class WaveformGenerator {
public:
    struct Result {
        std::vector<float> minBins;   // signed min per bin [-1..0]
        std::vector<float> maxBins;   // signed max per bin [0..1]
        double audioStartOffsetSec{0.0};
        double lengthSeconds{0.0};
        int sampleRate{0};
        int64 totalSamples{0};
    };

    WaveformGenerator();
    // binCount: number of horizontal bins desired
    // silenceThreshold: RMS threshold to detect start of audible content (0..1)
    // consecutiveChunksNeeded: number of consecutive chunks above threshold
    bool generate(const juce::File& file,
                  int binCount,
                  Result& out,
                  float silenceThreshold = 0.02f,
                  int consecutiveChunksNeeded = 3);

private:
    juce::AudioFormatManager formatManager;
};
