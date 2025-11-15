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
    const float trimADb = trimA.load();
    const float trimBDb = trimB.load();
    
    // Convert trim from dB to linear gain (dB = 20 * log10(gain))
    const float trimGainA = std::pow(10.0f, trimADb / 20.0f);
    const float trimGainB = std::pow(10.0f, trimBDb / 20.0f);

    // Crossfader calculation with proper center behavior
    // crossfaderValue: -1.0 (full A) to 0.0 (center/both) to +1.0 (full B)
    float gainA = 1.0f;
    float gainB = 1.0f;

    if (crossfaderValue < 0.0f) {
        // Fading towards A (left side): reduce B
        const float fadePos = std::abs(crossfaderValue);  // 0.0 to 1.0
        gainB = std::cos(fadePos * juce::MathConstants<float>::halfPi);
        // gainA stays at 1.0
    } else if (crossfaderValue > 0.0f) {
        // Fading towards B (right side): reduce A
        const float fadePos = crossfaderValue;  // 0.0 to 1.0
        gainA = std::cos(fadePos * juce::MathConstants<float>::halfPi);
        // gainB stays at 1.0
    }
    // else: crossfaderValue == 0.0 (center): both gainA and gainB stay at 1.0

    gainA *= volA;
    gainB *= volB;

    // Calculate deck levels AFTER trim but BEFORE volume fader
    // This shows the actual signal strength going into the channel
    float rmsA = 0.0f;
    float rmsB = 0.0f;
    
    if (tempBufferA.getNumChannels() >= 2) {
        float sumA = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            // Apply ONLY trim for VU meter calculation (not volume or crossfader)
            float l = tempBufferA.getSample(0, i) * trimGainA;
            float r = tempBufferA.getSample(1, i) * trimGainA;
            sumA += l * l + r * r;
        }
        rmsA = std::sqrt(sumA / (numSamples * 2));
    }
    
    if (tempBufferB.getNumChannels() >= 2) {
        float sumB = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            // Apply ONLY trim for VU meter calculation (not volume or crossfader)
            float l = tempBufferB.getSample(0, i) * trimGainB;
            float r = tempBufferB.getSample(1, i) * trimGainB;
            sumB += l * l + r * r;
        }
        rmsB = std::sqrt(sumB / (numSamples * 2));
    }
    
    deckALevel.store(rmsA);
    deckBLevel.store(rmsB);

    // Apply trim to the actual output mix
    const float finalGainA = gainA * trimGainA;
    const float finalGainB = gainB * trimGainB;

    const int mixChannels = std::min(numOutputChannels, kMinStereoChannels);
    for (int ch = 0; ch < mixChannels; ++ch) {
        if (auto* out = outputChannelData[ch]) {
            if (ch < tempBufferA.getNumChannels()) {
                juce::FloatVectorOperations::addWithMultiply(out,
                                                             tempBufferA.getReadPointer(ch),
                                                             finalGainA,
                                                             numSamples);
            }

            if (ch < tempBufferB.getNumChannels()) {
                juce::FloatVectorOperations::addWithMultiply(out,
                                                             tempBufferB.getReadPointer(ch),
                                                             finalGainB,
                                                             numSamples);
            }

            if (master != 1.0f) {
                juce::FloatVectorOperations::multiply(out, master, numSamples);
            }
        }
    }
    
    // Calculate master output levels (L and R separately)
    if (numOutputChannels >= 2 && outputChannelData[0] && outputChannelData[1]) {
        float sumL = 0.0f;
        float sumR = 0.0f;
        
        for (int i = 0; i < numSamples; ++i) {
            float l = outputChannelData[0][i];
            float r = outputChannelData[1][i];
            sumL += l * l;
            sumR += r * r;
        }
        
        masterLevelL.store(std::sqrt(sumL / numSamples));
        masterLevelR.store(std::sqrt(sumR / numSamples));
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

    // Reset shutdown flag so audio resumes after device restarts
    isShuttingDown.store(false);

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

void StereoAudioCallback::setTrimA(float trimDb) {
    trimA.store(juce::jlimit(-24.0f, 24.0f, trimDb));
}

void StereoAudioCallback::setTrimB(float trimDb) {
    trimB.store(juce::jlimit(-24.0f, 24.0f, trimDb));
}
