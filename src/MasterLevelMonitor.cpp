#include "MasterLevelMonitor.h"
#include <cmath>

void MasterLevelMonitor::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                        int numInputChannels,
                                                        float* const* outputChannelData,
                                                        int numOutputChannels,
                                                        int numberOfSamples,
                                                        const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(context, inputChannelData, numInputChannels);
    
    // Calculate RMS levels for left and right channels
    float leftSum = 0.0f;
    float rightSum = 0.0f;
    
    if (outputChannelData && numOutputChannels >= 2 && numberOfSamples > 0)
    {
        for (int i = 0; i < numberOfSamples; ++i)
        {
            if (outputChannelData[0])
                leftSum += outputChannelData[0][i] * outputChannelData[0][i];
            if (outputChannelData[1])
                rightSum += outputChannelData[1][i] * outputChannelData[1][i];
        }
        
        float leftRms = std::sqrt(leftSum / numberOfSamples);
        float rightRms = std::sqrt(rightSum / numberOfSamples);
        
        // Convert to dB (-60dB to 0dB range)
        float dbMin = -60.0f;
        float dbMax = 0.0f;
        
        float leftDb = leftRms > 0.000001f ? 20.0f * std::log10(leftRms) : dbMin;
        float rightDb = rightRms > 0.000001f ? 20.0f * std::log10(rightRms) : dbMin;
        
        leftDb = std::max(leftDb, dbMin);
        rightDb = std::max(rightDb, dbMin);
        
        // Convert to percentage (0-100%)
        float leftPercent = juce::jlimit(0.0f, 100.0f, ((leftDb - dbMin) / (dbMax - dbMin)) * 100.0f);
        float rightPercent = juce::jlimit(0.0f, 100.0f, ((rightDb - dbMin) / (dbMax - dbMin)) * 100.0f);
        
        // Apply exponential smoothing
        float currentLeft = leftChannelLevel.load();
        float currentRight = rightChannelLevel.load();
        
        float newLeft = currentLeft * (1.0f - smoothingFactor) + leftPercent * smoothingFactor;
        float newRight = currentRight * (1.0f - smoothingFactor) + rightPercent * smoothingFactor;
        
        leftChannelLevel.store(newLeft);
        rightChannelLevel.store(newRight);
    }
}

void MasterLevelMonitor::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // Initialize to zero levels
    leftChannelLevel.store(0.0f);
    rightChannelLevel.store(0.0f);
}

void MasterLevelMonitor::audioDeviceStopped()
{
    // Reset levels to zero when device stops
    leftChannelLevel.store(0.0f);
    rightChannelLevel.store(0.0f);
}

std::pair<float, float> MasterLevelMonitor::getLevels() const
{
    return std::make_pair(leftChannelLevel.load(), rightChannelLevel.load());
}
