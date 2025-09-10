#ifndef MASTERLEVELMONITOR_H
#define MASTERLEVELMONITOR_H

#include <JuceHeader.h>
#include <atomic>

/**
 * Audio callback that monitors the final master output levels
 * for display in the Master Out level bars
 */
class MasterLevelMonitor : public juce::AudioIODeviceCallback
{
public:
    MasterLevelMonitor() = default;
    ~MasterLevelMonitor() override = default;

    // AudioIODeviceCallback implementation
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numberOfSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // Get current levels (thread-safe)
    float getLeftChannelLevel() const { return leftChannelLevel.load(); }
    float getRightChannelLevel() const { return rightChannelLevel.load(); }
    std::pair<float, float> getLevels() const;

private:
    // Thread-safe level storage
    std::atomic<float> leftChannelLevel{0.0f};
    std::atomic<float> rightChannelLevel{0.0f};
    
    // For level smoothing
    float smoothingFactor = 0.3f;
};

#endif // MASTERLEVELMONITOR_H
