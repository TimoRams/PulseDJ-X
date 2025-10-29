#pragma once

#include <vector>
#include <functional>
#include <JuceHeader.h>

class WaveformGenerator {
public:
    struct AnalysisMetadata {
        double audioStartOffsetSec{0.0};
        double lengthSeconds{0.0};
        int sampleRate{0};
        int64 totalSamples{0};
        int64 audioStartSample{0};
    };

    struct Result {
        std::vector<float> minBins;   // signed min per bin [-1..0]
        std::vector<float> maxBins;   // signed max per bin [0..1]
        double audioStartOffsetSec{0.0};
        double lengthSeconds{0.0};
        int sampleRate{0};
        int64 totalSamples{0};
    };

    struct StreamingCallbacks {
        std::function<void(int totalBins,
                           double audioStartOffsetSec,
                           double lengthSeconds,
                           int sampleRate,
                           int64 totalSamples)> onBegin;
        std::function<void(int startBin,
                           const std::vector<float>& maxBins,
                           const std::vector<float>& minBins,
                           bool isFinalChunk)> onChunk;
        std::function<void(double progress)> onProgress;
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

    bool generateStreaming(const juce::File& file,
                           int binCount,
                           const StreamingCallbacks& callbacks,
                           int chunkBinSize = 512,
                           float silenceThreshold = 0.02f,
                           int consecutiveChunksNeeded = 3);

    bool analyzeFile(const juce::File& file,
                     AnalysisMetadata& metadata,
                     float silenceThreshold = 0.02f,
                     int consecutiveChunksNeeded = 3);

    bool renderBinWindow(const juce::File& file,
                         const AnalysisMetadata& metadata,
                         int totalBins,
                         int startBin,
                         int binCount,
                         std::vector<float>& outMax,
                         std::vector<float>& outMin);

    // Extended: also compute rough 3-band RMS energies (bass/mid/treble) per bin
    bool renderBinWindow(const juce::File& file,
                         const AnalysisMetadata& metadata,
                         int totalBins,
                         int startBin,
                         int binCount,
                         std::vector<float>& outMax,
                         std::vector<float>& outMin,
                         std::vector<float>& outLow,
                         std::vector<float>& outMid,
                         std::vector<float>& outHigh);

private:
    juce::AudioFormatManager formatManager;
};
