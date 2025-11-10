#include "StereoAudioCallback.h"

#include "DJAudioPlayer.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
constexpr int kMinStereoChannels = 2;
}

StereoAudioCallback::StereoAudioCallback(DJAudioPlayer* playerA, DJAudioPlayer* playerB)
    : audioPlayerA(playerA), audioPlayerB(playerB) {}

void StereoAudioCallback::audioDeviceIOCallback(const float* const* inputChannelData,
                                                int numInputChannels,
                                                float* const* outputChannelData,
                                                int numOutputChannels,
                                                int numSamples) {
    juce::AudioIODeviceCallbackContext context{};
    audioDeviceIOCallbackWithContext(inputChannelData,
                                     numInputChannels,
                                     outputChannelData,
                                     numOutputChannels,
                                     numSamples,
                                     context);
}

void StereoAudioCallback::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                           int numInputChannels,
                                                           float* const* outputChannelData,
                                                           int numOutputChannels,
                                                           int numSamples,
                                                           const juce::AudioIODeviceCallbackContext& context) {
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

    // CRITICAL: Check shutdown flag FIRST before accessing ANYTHING
    if (isShuttingDown.load()) {
        // Immediately silence all outputs and return
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            if (auto* out = outputChannelData[ch]) {
                juce::FloatVectorOperations::clear(out, numSamples);
            }
        }
        return;
    }

    static int callCount = 0;
    static bool infoLogged = false;

    if (!infoLogged) {
        std::cout << "Stereo Mixer callback: outputChannels=" << numOutputChannels
                  << ", samples=" << numSamples << std::endl;
        infoLogged = true;
    }

    if (++callCount % 5000 == 0) {
        std::cout << "Mixer running (" << callCount << " callbacks)" << std::endl;
    }

    if (numSamples <= 0) {
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            if (auto* out = outputChannelData[ch]) {
                juce::FloatVectorOperations::clear(out, numSamples);
            }
        }
        return;
    }

    const int bufferChannels = std::max(kMinStereoChannels, numOutputChannels);
    if (tempBufferA.getNumChannels() != bufferChannels || tempBufferA.getNumSamples() < numSamples) {
        tempBufferA.setSize(bufferChannels, numSamples, false, false, true);
    }
    if (tempBufferB.getNumChannels() != bufferChannels || tempBufferB.getNumSamples() < numSamples) {
        tempBufferB.setSize(bufferChannels, numSamples, false, false, true);
    }

    tempBufferA.clear();
    tempBufferB.clear();

    juce::AudioSourceChannelInfo bufferInfoA{&tempBufferA, 0, numSamples};
    juce::AudioSourceChannelInfo bufferInfoB{&tempBufferB, 0, numSamples};

    // Check if players are still valid and safe to access
    if (audioPlayerA) {
        try {
            audioPlayerA->getNextAudioBlock(bufferInfoA);
        } catch (...) {
            // Player might be in shutdown, silence this channel
            tempBufferA.clear();
        }
    }

    if (audioPlayerB) {
        try {
            audioPlayerB->getNextAudioBlock(bufferInfoB);
        } catch (...) {
            // Player might be in shutdown, silence this channel
            tempBufferB.clear();
        }
    }

    for (int ch = 0; ch < numOutputChannels; ++ch) {
        if (auto* out = outputChannelData[ch]) {
            juce::FloatVectorOperations::clear(out, numSamples);
        }
    }

    const float volA = volumeA.load();
    const float volB = volumeB.load();
    const float crossfaderValue = crossfaderPos.load();
    const float master = masterVolume.load();

    float gainA = 1.0f;
    float gainB = 1.0f;

    if (crossfaderValue <= 0.0f) {
        const float fadePos = std::abs(crossfaderValue);
        gainB = std::cos(fadePos * juce::MathConstants<float>::halfPi);
    } else {
        const float fadePos = crossfaderValue;
        gainA = std::cos(fadePos * juce::MathConstants<float>::halfPi);
    }

    gainA *= volA;
    gainB *= volB;

    const int mixChannels = std::min(numOutputChannels, kMinStereoChannels);
    for (int ch = 0; ch < mixChannels; ++ch) {
        if (auto* out = outputChannelData[ch]) {
            if (ch < tempBufferA.getNumChannels()) {
                juce::FloatVectorOperations::addWithMultiply(out,
                                                             tempBufferA.getReadPointer(ch),
                                                             gainA,
                                                             numSamples);
            }

            if (ch < tempBufferB.getNumChannels()) {
                juce::FloatVectorOperations::addWithMultiply(out,
                                                             tempBufferB.getReadPointer(ch),
                                                             gainB,
                                                             numSamples);
            }

            if (master != 1.0f) {
                juce::FloatVectorOperations::multiply(out, master, numSamples);
            }
        }
    }

    for (int ch = mixChannels; ch < numOutputChannels; ++ch) {
        if (auto* out = outputChannelData[ch]) {
            const int sourceCh = ch % mixChannels;
            if (sourceCh < numOutputChannels && outputChannelData[sourceCh]) {
                juce::FloatVectorOperations::copy(out,
                                                  outputChannelData[sourceCh],
                                                  numSamples);
            }
        }
    }
}

void StereoAudioCallback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    if (!device) {
        return;
    }

    std::cout << "StereoAudioCallback: Device starting - " << device->getName().toStdString()
              << ", Channels: " << device->getActiveOutputChannels().toInteger()
              << ", Sample Rate: " << device->getCurrentSampleRate() << std::endl;
}

void StereoAudioCallback::audioDeviceStopped() {
    std::cout << "StereoAudioCallback: Device stopped" << std::endl;
    isShuttingDown.store(true);
}

void StereoAudioCallback::setVolumeA(float vol) {
    volumeA.store(juce::jlimit(0.0f, 1.0f, vol));
}

void StereoAudioCallback::setVolumeB(float vol) {
    volumeB.store(juce::jlimit(0.0f, 1.0f, vol));
}

void StereoAudioCallback::setCrossfader(float pos) {
    crossfaderPos.store(juce::jlimit(-1.0f, 1.0f, pos));
}

void StereoAudioCallback::setMasterVolume(float vol) {
    masterVolume.store(juce::jlimit(0.0f, 1.0f, vol));
}
