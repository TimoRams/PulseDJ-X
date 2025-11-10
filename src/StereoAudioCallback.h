#pragma once

#include <JuceHeader.h>
#include <atomic>

class DJAudioPlayer;

/**
 * Custom audio callback that mixes Deck A and Deck B into a single stereo output
 * while applying volume, crossfader, and master level adjustments.
 */
class StereoAudioCallback : public juce::AudioIODeviceCallback {
public:
    StereoAudioCallback(DJAudioPlayer* playerA, DJAudioPlayer* playerB);
    ~StereoAudioCallback() override = default;

    void audioDeviceIOCallback(const float* const* inputChannelData,
                               int numInputChannels,
                               float* const* outputChannelData,
                               int numOutputChannels,
                               int numSamples);

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void setVolumeA(float vol);
    void setVolumeB(float vol);
    void setCrossfader(float pos);
    void setMasterVolume(float vol);
    
    // Safely detach players before destruction
    void detachPlayers() {
        isShuttingDown.store(true);
        audioPlayerA = nullptr;
        audioPlayerB = nullptr;
    }

private:
    DJAudioPlayer* audioPlayerA{nullptr};
    DJAudioPlayer* audioPlayerB{nullptr};
    juce::AudioBuffer<float> tempBufferA;
    juce::AudioBuffer<float> tempBufferB;

    std::atomic<float> volumeA{1.0f};
    std::atomic<float> volumeB{1.0f};
    std::atomic<float> crossfaderPos{0.0f};
    std::atomic<float> masterVolume{1.0f};
    std::atomic<bool> isShuttingDown{false};
};
