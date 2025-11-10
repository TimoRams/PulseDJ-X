#include "DJAudioPlayer.h"
#include "AudioFormatGuard.h"
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <span>
#include <expected>

namespace {
using std::numbers::pi;

constexpr double kMinSpeedRatio = 0.05;
constexpr double kMaxSpeedRatio = 8.0;
constexpr double kPitchBendMinRatio = 0.4;
constexpr double kPitchBendMaxRatio = 2.5;
constexpr double kDbMin = -60.0;
constexpr double kDbMax = 0.0;
constexpr float kSmoothingFactor = 0.3f;
constexpr double kDbRange = kDbMax - kDbMin;
constexpr int kFadeInMs = 5;
constexpr int kFadeOutMs = 5;
constexpr float kClickSuppressionThreshold = 0.01f;

constexpr auto dbToLinear = [](float db) constexpr noexcept -> float {
    return std::pow(10.0f, db / 20.0f);
};

constexpr auto linearToDb = [](float linear) constexpr noexcept -> float {
    return linear > 0.0f ? 20.0f * std::log10(linear) : static_cast<float>(kDbMin);
};

template<typename T>
[[nodiscard]] constexpr auto fastClamp(T value, T min, T max) noexcept -> T {
    return std::clamp(value, min, max);
}

constexpr auto applyEqualPowerFade = [](float* buffer, int numSamples, float startGain, float endGain) noexcept {
    const float invSamples = 1.0f / static_cast<float>(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        const float progress = static_cast<float>(i) * invSamples;
        const float angle = progress * static_cast<float>(pi) * 0.5f;
        const float gain = startGain * std::cos(angle) + endGain * std::sin(angle);
        buffer[i] *= gain;
    }
};

constexpr auto detectAndSuppressClick = [](float* buffer, int numSamples, float prevSample) noexcept {
    if (numSamples > 0 && std::abs(buffer[0] - prevSample) > kClickSuppressionThreshold) {
        const int fadeLength = std::min(16, numSamples);
        for (int i = 0; i < fadeLength; ++i) {
            const float fade = static_cast<float>(i) / fadeLength;
            buffer[i] = prevSample * (1.0f - fade) + buffer[i] * fade;
        }
    }
};
}

DJAudioPlayer::DJAudioPlayer(AudioFormatManager &_formatManager) 
    : formatManager(_formatManager)
{
    transportSource.setGain(1.0f);
    resampleSource.setResamplingRatio(1.0);
    currentSpeed = 1.0;
    pitchShiftRatio = 1.0;
}

DJAudioPlayer::~DJAudioPlayer() {
    try {
        // Ensure no audio processing is happening
        forceSilent.store(true);
        softPaused.store(true);
        scratchMode.store(false);
        
        // Stop transport first
        transportSource.stop();
        
        // Small delay to ensure any pending callbacks complete
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
#if defined(RUBBERBAND_FOUND)
        // Clean up RubberBand before disconnecting sources
        rb.reset();
        rbReady = false;
#endif
        
        // Now safe to disconnect and release
        transportSource.setSource(nullptr);
        resampleSource.releaseResources();
        transportSource.releaseResources();
        readerSource.reset();
        
    } catch (const std::exception& e) {
        std::cout << "Exception in DJAudioPlayer destructor: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "Unknown exception in DJAudioPlayer destructor" << std::endl;
    }
}

void DJAudioPlayer::prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
    transportSource.prepareToPlay(samplesPerBlockExpected, sampleRate);
    resampleSource.prepareToPlay(samplesPerBlockExpected, sampleRate);
    currentSampleRate = sampleRate;
    lastBlockSizeHint = samplesPerBlockExpected;
    
    std::ranges::for_each(audioBufferPool, [samplesPerBlockExpected](auto& buffer) {
        buffer = std::make_unique<AudioBuffer<float>>(2, samplesPerBlockExpected * 2);
        buffer->clear();
    });
    
    const juce::dsp::ProcessSpec spec{ sampleRate, static_cast<uint32>(samplesPerBlockExpected), 2 };
    
    auto prepareFilter = [&spec](auto& filter) { filter.reset(); filter.prepare(spec); };
    prepareFilter(lowShelf);
    prepareFilter(midPeak);
    prepareFilter(highShelf);
    prepareFilter(svf);

    cachedLowCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentSampleRate, 250.0f, 0.707f, 1.0f);
    cachedMidCoeffs = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 2500.0f, 1.0f, 1.0f);
    cachedHighCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentSampleRate, 10000.0f, 0.707f, 1.0f);
    
    lowShelf.coefficients = cachedLowCoeffs;
    midPeak.coefficients = cachedMidCoeffs;
    highShelf.coefficients = cachedHighCoeffs;
    svf.setCutoffFrequency(1000.0f);
    svf.setResonance(0.7f);

    dspPrepared = true;

#if defined(RUBBERBAND_FOUND)
    reinitRubberBand();
#else
    #error "RubberBand is required for keylock functionality"
#endif

    keylockPrimeSamplesRemaining = static_cast<int>(std::ceil((keylockPrimeMs / 1000.0) * currentSampleRate));
}

void DJAudioPlayer::getNextAudioBlock(const AudioSourceChannelInfo &bufferToFill) {
    if (readerSource.get() == nullptr) [[unlikely]] {
        bufferToFill.clearActiveBufferRegion();
        return;
    }
    lastBlockSizeHint = bufferToFill.numSamples;
    
    int pendingKL = keylockChangePending.exchange(-1);
    if (pendingKL != -1) [[unlikely]] {
        keylockEnabled = (pendingKL == 1);
        // No crossfade necessary: RubberBand stays in the chain and manages continuity.
        transitionBufferValid = false;
        transitionSamplesRemaining = 0;
        transitionSamplesTotal = 0;
#if defined(RUBBERBAND_FOUND)
        if (rbReady) {
            // Re-prime timing state so toggling keylock only affects RubberBand parameters.
            rbPaddedStartDone = false;
            rbDiscardOutRemaining = 0;
            if (keylockEnabled) {
                keylockPrimeSamplesRemaining = (int) std::ceil((keylockPrimeMs / 1000.0) * currentSampleRate);
            } else {
                keylockPrimeSamplesRemaining = 0;
            }
        }
#endif
        updateResampleRatio();
    }

    if (forceSilent.load() || softPaused.load()) [[unlikely]] {
        bufferToFill.clearActiveBufferRegion();
        transitionBufferValid = false;
        transitionSamplesRemaining = 0;
        return;
    }

    if (scratchMode.load()) [[unlikely]] {
        renderScratchAudio(bufferToFill);
        transitionBufferValid = false;
        transitionSamplesRemaining = 0;
        
        // Scratch mode latency measurement (minimal - direktes sample fetching)
        if (currentSampleRate > 0.0) {
            int totalLatencySamples = 0;
            
            // 1. Audio system buffer (immer)
            totalLatencySamples += lastBlockSizeHint;
            
            // 2. Scratch cache access latency (sehr klein)
            totalLatencySamples += 4; // ~0.1ms bei 48kHz
            
            // 3. DSP Effects (falls aktiv)
            totalLatencySamples += (dspPrepared ? 8 : 0);
            
            // 4. Hardware output
            totalLatencySamples += (int)(currentSampleRate * 0.001);
            
            measuredLatencyMs.store((totalLatencySamples / currentSampleRate) * 1000.0);
            latencyCompensationSamples = totalLatencySamples;
        }
        
        return;
    }

    if (inPrerollMode && transportSource.isPlaying()) [[unlikely]] {
        double timeAdvance = double(bufferToFill.numSamples) / currentSampleRate;
        double currentPrerollTime = prerollPosition * prerollTimeSec;
        currentPrerollTime += timeAdvance;
        
        if (currentPrerollTime >= -0.01) {
            inPrerollMode = false;
            prerollPosition = 0.0;
            transportSource.setPosition(0.0);
            
#if defined(RUBBERBAND_FOUND)
            if (keylockEnabled && rbReady) {
                rb->reset();
                rbPaddedStartDone = false;
                rbDiscardOutRemaining = 0;
                keylockPrimeSamplesRemaining = (int) std::ceil((keylockPrimeMs / 1000.0) * currentSampleRate);
            }
#endif
        } else {
            prerollPosition = currentPrerollTime / prerollTimeSec;
            bufferToFill.clearActiveBufferRegion();
            return;
        }
    } else if (inPrerollMode) {
        bufferToFill.clearActiveBufferRegion();
        if (transportSource.getCurrentPosition() > 0.1) {
            transportSource.setPosition(0.0);
        }
        return;
    }

    if (!transportSource.isPlaying()) [[unlikely]] {
        bufferToFill.clearActiveBufferRegion();
        if (pausedResetPending.exchange(false)) {
#if defined(RUBBERBAND_FOUND)
            // Keep RB instance; just mark for a fresh start next time without heavy reset
            rbReady = true;
            rbPaddedStartDone = false;
            rbDiscardOutRemaining = 0;
#endif
        }
        return;
    }
    
    if (loopEnabled) [[unlikely]] {
        double pos = transportSource.getCurrentPosition();
        double nextPos = pos + (double(bufferToFill.numSamples) / currentSampleRate);
        
        if (loopCrossfadeActive) {
            // Apply crossfade from pre-buffered data
            const int samplesToProcess = std::min(bufferToFill.numSamples, loopCrossfadeSamples - loopCrossfadePosition);
            const int numChannels = std::min(bufferToFill.buffer->getNumChannels(), loopCrossfadeBuffer.getNumChannels());
            
            for (int ch = 0; ch < numChannels; ++ch) {
                bufferToFill.buffer->copyFrom(ch, bufferToFill.startSample,
                                             loopCrossfadeBuffer, ch, loopCrossfadePosition, samplesToProcess);
            }
            
            loopCrossfadePosition += samplesToProcess;
            
            // Check if crossfade is complete
            if (loopCrossfadePosition >= loopCrossfadeSamples) {
                loopCrossfadeActive = false;
                loopCrossfadePosition = 0;
                qDebug() << "Loop crossfade completed";
            }
            
            // Return - crossfade handles the entire buffer
            return;
        }
        
        if (pos < loopEndSec && nextPos >= loopEndSec && loopEndSec > loopStartSec) {
            // Calculate how many samples until loop end
            double timeToLoopEnd = loopEndSec - pos;
            int samplesToLoopEnd = (int)(timeToLoopEnd * currentSampleRate);
            
            // Clamp to buffer boundaries
            samplesToLoopEnd = std::max(0, std::min(samplesToLoopEnd, bufferToFill.numSamples));
            
            const int crossfadeLength = std::min(1024, std::min(samplesToLoopEnd, bufferToFill.numSamples / 2));
            
            if (crossfadeLength >= 16 && samplesToLoopEnd >= crossfadeLength) {
                AudioBuffer<float> endBuffer(bufferToFill.buffer->getNumChannels(), bufferToFill.numSamples);
                AudioSourceChannelInfo endInfo;
                endInfo.buffer = &endBuffer;
                endInfo.startSample = 0;
                endInfo.numSamples = bufferToFill.numSamples;
                
                const double loopResampleRatio = (rbReady && rb && keylockEnabled) ? 1.0 : effectiveSpeed();
                resampleSource.setResamplingRatio(loopResampleRatio);
                resampleSource.getNextAudioBlock(endInfo);
                
                double currentPos = transportSource.getCurrentPosition();
                transportSource.setPosition(loopStartSec);
                
                const int startBufferSize = std::max(crossfadeLength * 2, bufferToFill.numSamples);
                AudioBuffer<float> startBuffer(bufferToFill.buffer->getNumChannels(), startBufferSize);
                AudioSourceChannelInfo startInfo;
                startInfo.buffer = &startBuffer;
                startInfo.startSample = 0;
                startInfo.numSamples = startBufferSize;
                resampleSource.getNextAudioBlock(startInfo);
                
                const int fadeStartIndex = samplesToLoopEnd - crossfadeLength;
                const float invCrossfade = 1.0f / (crossfadeLength - 1);
                
                for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                    bufferToFill.buffer->copyFrom(ch, bufferToFill.startSample, endBuffer, ch, 0, bufferToFill.numSamples);
                    
                    auto* channelData = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                    
                    for (int i = 0; i < crossfadeLength; ++i) {
                        const int outputIndex = fadeStartIndex + i;
                        if (outputIndex >= 0 && outputIndex < bufferToFill.numSamples) {
                            const float progress = i * invCrossfade;
                            const float hannProgress = 0.5f * (1.0f - std::cos(progress * static_cast<float>(pi)));
                            const float angle = hannProgress * static_cast<float>(pi) * 0.5f;
                            const float endGain = std::cos(angle);
                            const float startGain = std::sin(angle);
                            
                            const float endSample = endBuffer.getSample(ch, outputIndex);
                            const float startSample = (i < startBuffer.getNumSamples()) ? startBuffer.getSample(ch, i) : 0.0f;
                            
                            channelData[outputIndex] = std::fma(endSample, endGain, startSample * startGain);
                        }
                    }
                    
                    const int remainderStart = fadeStartIndex + crossfadeLength;
                    const int remainderLength = bufferToFill.numSamples - remainderStart;
                    if (remainderLength > 0 && remainderStart >= 0) {
                        for (int i = 0; i < remainderLength; ++i) {
                            const int outputIndex = remainderStart + i;
                            const int startIndex = crossfadeLength + i;
                            if (outputIndex < bufferToFill.numSamples && startIndex < startBuffer.getNumSamples()) {
                                channelData[outputIndex] = startBuffer.getSample(ch, startIndex);
                            }
                        }
                    }
                }
                
                qDebug() << "Loop crossfade:" << crossfadeLength << "samples @" << currentPos;
                return;
            } else {
                AudioBuffer<float> preJumpBuffer(bufferToFill.buffer->getNumChannels(), 32);
                AudioSourceChannelInfo preInfo;
                preInfo.buffer = &preJumpBuffer;
                preInfo.startSample = 0;
                preInfo.numSamples = 32;
                
                if (rbReady && rb) {
                    resampleSource.setResamplingRatio(1.0);
                } else {
                    resampleSource.setResamplingRatio(effectiveSpeed());
                }
                resampleSource.getNextAudioBlock(preInfo);
                
                transportSource.setPosition(loopStartSec);
                resampleSource.getNextAudioBlock(bufferToFill);
                
                const int extendedFade = std::min(64, bufferToFill.numSamples / 2);
                for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                    const float lastSample = (preJumpBuffer.getNumSamples() > 0) ? 
                                      preJumpBuffer.getSample(ch, preJumpBuffer.getNumSamples() - 1) : 0.0f;
                    
                    auto* channelData = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                    
                    for (int i = 0; i < extendedFade; ++i) {
                        const float fadeProgress = static_cast<float>(i) / extendedFade;
                        const float hannFade = 0.5f * (1.0f - std::cos(fadeProgress * static_cast<float>(pi)));
                        
                        if (i == 0 && std::abs(lastSample) > 0.001f) {
                            channelData[i] = std::fma(channelData[i] - lastSample, hannFade, lastSample);
                        } else {
                            channelData[i] *= hannFade;
                        }
                    }
                }
                
                return;
            }
        }
        else if (pos >= loopEndSec && loopEndSec > loopStartSec) {
            transportSource.setPosition(loopStartSec);
            const double loopResampleRatio = (rbReady && rb && keylockEnabled) ? 1.0 : effectiveSpeed();
            resampleSource.setResamplingRatio(loopResampleRatio);
            resampleSource.getNextAudioBlock(bufferToFill);
            
            const int totalFadeLength = std::min(128, bufferToFill.numSamples / 2);
            const int quickSuppressLength = totalFadeLength / 4;
            
            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                auto* channelData = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                
                for (int i = 0; i < totalFadeLength; ++i) {
                    const float progress = static_cast<float>(i) / totalFadeLength;
                    const float fade = (i < quickSuppressLength) ? 
                        (progress * 4.0f) * (progress * 4.0f) :
                        0.5f * (1.0f - std::cos((progress - 0.25f) / 0.75f * static_cast<float>(pi)));
                    channelData[i] *= fade;
                }
            }
            return;
        }
    }
    
#if defined(RUBBERBAND_FOUND)
    if (rbReady && rb) [[likely]] {
        const bool isKeylockActive = keylockEnabled;
        const int desiredOut = bufferToFill.numSamples;
        const int chsOut = bufferToFill.buffer->getNumChannels();
        const double playbackSpeed = effectiveSpeed();
        
        // COMPREHENSIVE LATENCY MEASUREMENT: ALL audio pipeline components
        if (currentSampleRate > 0.0) {
            int totalLatencySamples = 0;
            
            // 1. RubberBand algorithmic latency (present while the stretcher is active)
            totalLatencySamples += rbLatencySamples;
            
            // 2. Audio system buffer latency (immer vorhanden)
            totalLatencySamples += lastBlockSizeHint;
            
            // 3. RubberBand internal buffer occupancy (nur überschüssiges, aggressiv gerechnet)
            const int availableSamples = rb->available();
            const int minRequired = desiredOut;
            const int excessBuffer = std::max(0, availableSamples - minRequired);
            // Nur 25% des Excess als Latenz zählen (rest ist notwendiger Puffer)
            totalLatencySamples += (excessBuffer / 4); // Changed from /2
            
            // 4. Resampling latency (immer vorhanden, auch ohne keylock)
            const double resampleRatio = keylockEnabled ? 1.0 : playbackSpeed;
            const int resamplingLatency = std::max(0, (int)(lastBlockSizeHint * 0.2 / resampleRatio)); // Reduced from 0.3
            totalLatencySamples += resamplingLatency;
            
            // 5. DSP Effects latency (EQ filters, SVF)
            // IIR filters haben typisch 1-2 samples group delay
            const int dspFilterLatency = dspPrepared ? 6 : 0; // Reduced from 8
            totalLatencySamples += dspFilterLatency;
            
            // 6. Fade/Crossfade processing latency (wenn aktiv)
            if (transitionBufferValid || fadeSamplesRemaining > 0) {
                totalLatencySamples += std::min(16, lastBlockSizeHint / 8); // Reduced from 32 and /4
            }
            
            // 7. Hardware output latency estimation (typisch 0.5-2ms bei low-latency drivers)
            // Dies ist eine Schätzung - echte HW-Latency müsste vom System abgefragt werden
            const int hwLatencyEstimate = (int)(currentSampleRate * 0.0008); // Reduced from 0.001 (1ms -> 0.8ms)
            totalLatencySamples += hwLatencyEstimate;
            
            measuredLatencyMs.store((totalLatencySamples / currentSampleRate) * 1000.0);
            latencyCompensationSamples = totalLatencySamples;
        }
        
        if (debugKeylock) std::cout << "[RB] Enter path: keylock=" << isKeylockActive 
                                     << ", desiredOut=" << desiredOut
                                     << ", chsOut=" << chsOut << std::endl;
        if (lastBlockSizeHint <= 0 || currentSampleRate <= 0.0) [[unlikely]] {
            if (debugKeylock) std::cout << "[KL][RB] Not ready: lastBlockSizeHint=" << lastBlockSizeHint
                                        << ", SR=" << currentSampleRate << ". Fallback." << std::endl;
            resampleSource.getNextAudioBlock(bufferToFill);
            return;
        }
        
        if (bufferToFill.buffer->getNumChannels() <= 0) [[unlikely]] {
            if (debugKeylock) std::cout << "[KL][RB] No output channels, clearing" << std::endl;
            bufferToFill.clearActiveBufferRegion();
            return;
        }
        
        if (keylockPrimeSamplesRemaining > 0) [[unlikely]] {
            const int chsRB = rbNumChannels;
            const int chunk = lastBlockSizeHint > 0 ? lastBlockSizeHint : bufferToFill.numSamples;
            if (rbInputBuffer.getNumChannels() < chsRB || rbInputBuffer.getNumSamples() < chunk)
                rbInputBuffer.setSize(chsRB, chunk, false, true, true);
            AudioSourceChannelInfo tempInfo;
            tempInfo.buffer = &rbInputBuffer;
            tempInfo.startSample = 0;
            tempInfo.numSamples = chunk;
            for (int c = 0; c < chsRB; ++c) rbInputBuffer.clear(c, 0, chunk);
            const double feedRatio = keylockEnabled ? 1.0 : effectiveSpeed();
            resampleSource.setResamplingRatio(feedRatio);
            resampleSource.getNextAudioBlock(tempInfo);
            std::vector<const float*> inPtrs(chsRB);
            for (int c = 0; c < chsRB; ++c) inPtrs[c] = rbInputBuffer.getReadPointer(c);
            rb->process(inPtrs.data(), chunk, false);
            keylockPrimeSamplesRemaining -= chunk;
            if (debugKeylock) std::cout << "[RB] Priming... remaining=" << keylockPrimeSamplesRemaining << std::endl;
            bufferToFill.clearActiveBufferRegion();
            return;
        }
        
    // NOTE: RubberBand remains active for both keylock modes so the processing
    // latency stays consistent. When keylock is OFF we leave RubberBand at unity
    // and let the resampler apply the natural speed/pitch change.
        
        try {
    const double speed = playbackSpeed;
    const int chsRB = rbNumChannels;

    const double safeSpeed = std::max(1e-6, speed);
    double desiredTimeRatio = isKeylockActive ? (1.0 / safeSpeed) : 1.0;
    double desiredPitchScale = 1.0;

        if (std::abs(desiredTimeRatio - rbLastTimeRatio) > 1e-4) {
            rb->setTimeRatio(desiredTimeRatio);
            rbLastTimeRatio = desiredTimeRatio;
            if (debugKeylock) std::cout << "[RB] setTimeRatio=" << desiredTimeRatio << std::endl;
        }

        if (std::abs(desiredPitchScale - rbLastPitchScale) > 1e-4) {
            rb->setPitchScale(desiredPitchScale);
            rbLastPitchScale = desiredPitchScale;
            if (debugKeylock) std::cout << "[RB] setPitchScale=" << desiredPitchScale << std::endl;
        }

    const double liveResampleRatio = keylockEnabled ? 1.0 : speed;
    resampleSource.setResamplingRatio(liveResampleRatio);

        if (!rbPaddedStartDone) {
            size_t pad = rb->getPreferredStartPad();
            // ULTRA-OPTIMIZED: Minimal padding for absolute lowest latency
            // DJ mixing prioritizes latency over perfect transient analysis
            const size_t maxPad = static_cast<size_t>(currentSampleRate * 0.002); // Max 2ms (reduced from 5ms)
            pad = std::min(pad, maxPad);
            
            if (debugKeylock) std::cout << "[KL][RB] preferredStartPad=" << pad << " (capped at " << maxPad << ")" << std::endl;
            if (pad > 0) {
                if (rbInputBuffer.getNumChannels() < chsRB || rbInputBuffer.getNumSamples() < (int)pad)
                    rbInputBuffer.setSize(chsRB, (int)pad, false, true, true);
                rbInputBuffer.clear();
                std::vector<const float*> z(chsRB);
                for (int c = 0; c < chsRB; ++c) z[c] = rbInputBuffer.getReadPointer(c);
                rb->process(z.data(), (int)pad, false);
            }
            rbLatencySamples = (int)rb->getStartDelay();
            rbLatencySeconds = rbLatencySamples / currentSampleRate;
            rbDiscardOutRemaining = rbLatencySamples;
            rbOutScratch.setSize(chsRB, std::max(desiredOut * 2, rbLatencySamples + desiredOut));
            rbOutScratch.clear();
            rbPaddedStartDone = true;
        }

        int produced = 0;
        while (rbDiscardOutRemaining > 0 || rb->available() < (desiredOut - produced)) {
            int needIn = (int)rb->getSamplesRequired();
            if (needIn <= 0) {
                double tr = std::max(1e-6, rbLastTimeRatio);
                needIn = (int)std::ceil((desiredOut - produced) / tr);
            }
            if (needIn <= 0) break;
            
            // ULTRA-OPTIMIZED: Sehr konservatives Feeding für minimale Latenz
            // Nur das absolut Notwendige füttern, nicht mehr
            const int maxFeedPerCycle = std::min(needIn, lastBlockSizeHint); // Limit to 1x block size (reduced from 2x)
            needIn = std::min(needIn, maxFeedPerCycle);
            
            if (rbInputBuffer.getNumChannels() < chsRB || rbInputBuffer.getNumSamples() < needIn)
                rbInputBuffer.setSize(chsRB, needIn, false, true, true);

            int fed = 0;
            while (fed < needIn) {
                const int chunk = juce::jmin(lastBlockSizeHint, needIn - fed);
                if (debugKeylock) std::cout << "[KL][RB] feeding chunk=" << chunk << "/" << needIn << std::endl;
                AudioSourceChannelInfo tempInfo;
                tempInfo.buffer = &rbInputBuffer;
                tempInfo.startSample = fed;
                tempInfo.numSamples = chunk;
                for (int c = 0; c < chsRB; ++c) rbInputBuffer.clear(c, fed, chunk);
                resampleSource.getNextAudioBlock(tempInfo);
                fed += chunk;
            }
            std::vector<const float*> inPtrs(chsRB);
            for (int c = 0; c < chsRB; ++c) inPtrs[c] = rbInputBuffer.getReadPointer(c);
            rb->process(inPtrs.data(), needIn, false);

            if (rbDiscardOutRemaining > 0 && rb->available() > 0) {
                int avail = rb->available();
                int toTake = juce::jmin(avail, rbDiscardOutRemaining);
                if (debugKeylock) std::cout << "[KL][RB] discard latency toTake=" << toTake << std::endl;
                if (rbOutScratch.getNumChannels() < chsRB || rbOutScratch.getNumSamples() < toTake)
                    rbOutScratch.setSize(chsRB, toTake, false, true, true);
                std::vector<float*> sPtrs(chsRB);
                for (int c = 0; c < chsRB; ++c) sPtrs[c] = rbOutScratch.getWritePointer(c);
                rb->retrieve(sPtrs.data(), toTake);
                rbDiscardOutRemaining -= toTake;
            }

            if (produced >= desiredOut) break;
        }

        const int toRetrieve = std::max(0, juce::jmin(rb->available(), desiredOut));
        if (rbOutScratch.getNumChannels() < chsRB || rbOutScratch.getNumSamples() < std::max(1, toRetrieve))
            rbOutScratch.setSize(chsRB, std::max(1, toRetrieve), false, true, true);
        std::vector<float*> outPtrsRB(chsRB);
        for (int c = 0; c < chsRB; ++c) outPtrsRB[c] = rbOutScratch.getWritePointer(c);
    const int got = (toRetrieve > 0) ? rb->retrieve(outPtrsRB.data(), toRetrieve) : 0;
    if (debugKeylock) std::cout << "[KL][RB] retrieved got=" << got << "/" << desiredOut
                     << ", availableAfter=" << rb->available() << std::endl;

        if (got <= 0) {
            bufferToFill.clearActiveBufferRegion();
        } else {
            if (chsRB >= chsOut) {
                for (int c = 0; c < chsOut; ++c) {
                    bufferToFill.buffer->copyFrom(c, bufferToFill.startSample, rbOutScratch, c, 0, got);
                }
            }
            else if (chsRB == 1 && chsOut >= 1) {
                for (int c = 0; c < chsOut; ++c) {
                    bufferToFill.buffer->copyFrom(c, bufferToFill.startSample, rbOutScratch, 0, 0, got);
                }
            }
            else if (chsRB >= 2 && chsOut == 1) {
                if (rbOutScratch.getNumChannels() >= 2) {
                    AudioBuffer<float> mix;
                    mix.setSize(1, got, false, true, true);
                    const float* lptr = rbOutScratch.getReadPointer(0);
                    const float* rptr = rbOutScratch.getReadPointer(1);
                    float* mptr = mix.getWritePointer(0);
                    for (int i = 0; i < got; ++i) mptr[i] = 0.5f * (lptr[i] + rptr[i]);
                    bufferToFill.buffer->copyFrom(0, bufferToFill.startSample, mix, 0, 0, got);
                } else {
                    // Fallback: copy first channel
                    bufferToFill.buffer->copyFrom(0, bufferToFill.startSample, rbOutScratch, 0, 0, got);
                }
            }

            for (int c = chsRB; c < chsOut; ++c) {
                bufferToFill.buffer->clear(c, bufferToFill.startSample, got);
            }
        }

        if (got < desiredOut) {
            const int remain = desiredOut - got;
            if (remain > 0) {
                for (int c = 0; c < chsOut; ++c) {  // Use chsOut instead of min(chsOut, chsRB)
                    auto* dst = bufferToFill.buffer->getWritePointer(c, bufferToFill.startSample + got);
                    juce::FloatVectorOperations::clear(dst, remain);
                }
            }
        }
        resumeCompensatePending = false;
        
        // Apply any pending crossfade transition (both enabling and disabling
        // keylock should use the prepared transition buffer). Keep this
        // unconditional so toggling direction still crossfades smoothly.
        if (transitionBufferValid && transitionSamplesRemaining > 0) {
            applyCrossfadeTransition(bufferToFill);
        }
        } catch (const std::exception& e) {
            std::cout << "RubberBand processing error: " << e.what() << std::endl;
            bufferToFill.clearActiveBufferRegion();
            rbReady = false;
            measuredLatencyMs.store(0.0);
            latencyCompensationSamples = 0;
            return;
        } catch (...) {
            std::cout << "RubberBand processing unknown error" << std::endl;
            bufferToFill.clearActiveBufferRegion();
            rbReady = false;
            measuredLatencyMs.store(0.0);
            latencyCompensationSamples = 0;
            return;
        }
    } else {
        if (debugKeylock && keylockEnabled && std::abs(effectiveSpeed() - 1.0) > 0.01) [[unlikely]] {
            std::cout << "[KL] RubberBand not available - keylock disabled" << std::endl;
        }
        
        resampleSource.setResamplingRatio(effectiveSpeed());
        resampleSource.getNextAudioBlock(bufferToFill);
        
        // Latency measurement WITHOUT RubberBand (normal playback mode)
        if (currentSampleRate > 0.0) {
            int totalLatencySamples = 0;
            
            // 1. Audio system buffer latency
            totalLatencySamples += lastBlockSizeHint;
            
            // 2. Resampling latency (speed-dependent)
            const double resampleRatio = effectiveSpeed();
            const int resamplingLatency = std::max(0, (int)(lastBlockSizeHint * 0.3 / resampleRatio));
            totalLatencySamples += resamplingLatency;
            
            // 3. DSP Effects latency
            const int dspFilterLatency = dspPrepared ? 8 : 0;
            totalLatencySamples += dspFilterLatency;
            
            // 4. Fade processing (wenn aktiv)
            if (fadeSamplesRemaining > 0) {
                totalLatencySamples += std::min(32, lastBlockSizeHint / 4);
            }
            
            // 5. Hardware output latency estimation
            const int hwLatencyEstimate = (int)(currentSampleRate * 0.001); // ~1ms
            totalLatencySamples += hwLatencyEstimate;
            
            measuredLatencyMs.store((totalLatencySamples / currentSampleRate) * 1000.0);
            latencyCompensationSamples = totalLatencySamples;
        } else {
            measuredLatencyMs.store(0.0);
            latencyCompensationSamples = 0;
        }
        
        static int normalPlaybackCounter = 0;
        if (++normalPlaybackCounter % 2000 == 0) [[unlikely]] {
            std::cout << "[Normal] Playing: channels=" << bufferToFill.buffer->getNumChannels() 
                      << ", samples=" << bufferToFill.numSamples << std::endl;
        }
    }
#else
    #error "RubberBand is required for keylock functionality"
#endif

    applyDSPEffects(bufferToFill);
    
    if (fadeSamplesRemaining > 0 && fadeSamplesTotal > 0) {
        const int samplesToProcess = std::min(bufferToFill.numSamples, fadeSamplesRemaining);
        const int numChannels = bufferToFill.buffer->getNumChannels();
        const float invTotal = 1.0f / static_cast<float>(fadeSamplesTotal);
        
        for (int ch = 0; ch < numChannels; ++ch) {
            auto* channelData = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
            
            for (int i = 0; i < samplesToProcess; ++i) {
                const float progress = static_cast<float>(fadeSamplesTotal - fadeSamplesRemaining + i) * invTotal;
                const float angle = progress * static_cast<float>(pi) * 0.5f;
                const float gain = fadeStartGain * std::cos(angle) + fadeTargetGain * std::sin(angle);
                channelData[i] *= gain;
            }
            
            if (ch == 0) lastOutputSampleL = channelData[samplesToProcess - 1];
            if (ch == 1) lastOutputSampleR = channelData[samplesToProcess - 1];
        }
        
        fadeSamplesRemaining -= samplesToProcess;
        
        if (fadeSamplesRemaining <= 0) {
            fadeSamplesRemaining = 0;
            startFadeActive = false;
            stopFadeActive = false;
        }
    } else if (bufferToFill.numSamples > 0) {
        const int numChannels = bufferToFill.buffer->getNumChannels();
        const int lastSample = bufferToFill.startSample + bufferToFill.numSamples - 1;
        
        for (int ch = 0; ch < numChannels; ++ch) {
            auto* channelData = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
            
            if (ch == 0) {
                detectAndSuppressClick(channelData, bufferToFill.numSamples, lastOutputSampleL);
                lastOutputSampleL = bufferToFill.buffer->getSample(ch, lastSample);
            } else if (ch == 1) {
                detectAndSuppressClick(channelData, bufferToFill.numSamples, lastOutputSampleR);
                lastOutputSampleR = bufferToFill.buffer->getSample(ch, lastSample);
            }
        }
    }
}

void DJAudioPlayer::applyDSPEffects(const AudioSourceChannelInfo &bufferToFill) {
    if (!dspPrepared || bufferToFill.buffer->getNumChannels() == 0) [[unlikely]] return;

    AudioBuffer<float>& buffer = *bufferToFill.buffer;
    const int numSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;

    juce::dsp::AudioBlock<float> block(buffer);
    auto subBlock = block.getSubBlock(startSample, numSamples);
    auto limitedBlock = subBlock.getSubsetChannelBlock(0, std::min(buffer.getNumChannels(), 2));
    juce::dsp::ProcessContextReplacing<float> ctx(limitedBlock);

    auto updateFilter = [this, &ctx](auto& filter, double gain, float freq, auto makeCoeff, bool shelf = true) {
        const float gainLinear = dbToLinear(fastClamp(static_cast<float>(gain * 12.0), -12.0f, 12.0f));
        filter.coefficients = shelf ? 
            makeCoeff(currentSampleRate, freq, 0.707f, gainLinear) :
            makeCoeff(currentSampleRate, freq, 1.0f, gainLinear);
        filter.process(ctx);
    };

    updateFilter(lowShelf, lowGain, 300.0f, juce::dsp::IIR::Coefficients<float>::makeLowShelf, true);
    updateFilter(midPeak, midGain, 2500.0f, juce::dsp::IIR::Coefficients<float>::makePeakFilter, false);
    updateFilter(highShelf, highGain, 8000.0f, juce::dsp::IIR::Coefficients<float>::makeHighShelf, true);

    const auto absNorm = std::abs(filterKnob);
    const auto [cutoffHz, filterType] = filterKnob < 0.0 ?
        std::pair{20000.0 * std::pow(0.01, absNorm), juce::dsp::StateVariableTPTFilterType::lowpass} :
        std::pair{20.0 * std::pow(250.0, absNorm), juce::dsp::StateVariableTPTFilterType::highpass};
    
    svf.setType(filterType);
    svf.setCutoffFrequency(fastClamp(static_cast<float>(cutoffHz), 20.0f, 20000.0f));
    svf.process(ctx);
    
    if (buffer.getNumChannels() > 0 && numSamples > 0) [[likely]] {
        auto calcRMS = [&](int ch) noexcept {
            const auto* data = buffer.getReadPointer(ch, startSample);
            float sum = 0.0f;
            for (int i = 0; i < numSamples; ++i) sum = std::fma(data[i], data[i], sum);
            return std::sqrt(sum / numSamples);
        };

        const float leftRMS = buffer.getNumChannels() >= 1 ? calcRMS(0) : 0.0f;
        const float rightRMS = buffer.getNumChannels() >= 2 ? calcRMS(1) : leftRMS;
        
        auto toPercent = [](float rms) constexpr noexcept {
            constexpr float kDbMinF = static_cast<float>(kDbMin);
            constexpr float kDbRangeF = static_cast<float>(kDbRange);
            const float db = linearToDb(rms);
            return fastClamp((db - kDbMinF) / kDbRangeF * 100.0f, 0.0f, 100.0f);
        };

        leftChannelLevel.store(std::fma(kSmoothingFactor, toPercent(leftRMS), 
            (1.0f - kSmoothingFactor) * leftChannelLevel.load()), std::memory_order_relaxed);
        rightChannelLevel.store(std::fma(kSmoothingFactor, toPercent(rightRMS),
            (1.0f - kSmoothingFactor) * rightChannelLevel.load()), std::memory_order_relaxed);
    }
}

void DJAudioPlayer::applyCrossfadeTransition(const AudioSourceChannelInfo &bufferToFill) {
    if (!transitionBufferValid || transitionSamplesRemaining <= 0 || transitionSamplesTotal <= 0) [[unlikely]] {
        return;
    }
    
    const int numChannels = std::min(bufferToFill.buffer->getNumChannels(), transitionBuffer.getNumChannels());
    const int samplesToProcess = std::min(bufferToFill.numSamples, transitionSamplesRemaining);
    const int transitionReadPos = transitionSamplesTotal - transitionSamplesRemaining;
    
    constexpr auto equalPowerGains = [](float progress) constexpr noexcept -> std::pair<float, float> {
        const float angle = progress * static_cast<float>(pi) * 0.5f;
        return {std::cos(angle), std::sin(angle)};
    };
    
    const float invTotal = 1.0f / static_cast<float>(transitionSamplesTotal);
    
    for (int ch = 0; ch < numChannels; ++ch) {
        auto* outPtr = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
        const auto* oldPtr = transitionBuffer.getReadPointer(ch, transitionReadPos);
        
        for (int i = 0; i < samplesToProcess; ++i) {
            const float fadeProgress = static_cast<float>(transitionReadPos + i) * invTotal;
            const auto [oldGain, newGain] = equalPowerGains(fadeProgress);
            
            outPtr[i] = std::fma(oldPtr[i], oldGain, outPtr[i] * newGain);
        }
    }
    
    transitionSamplesRemaining -= samplesToProcess;
    
    if (transitionSamplesRemaining <= 0) [[unlikely]] {
        transitionBufferValid = false;
        transitionSamplesRemaining = 0;
        if (debugKeylock) [[unlikely]] {
            std::cout << "[Transition] Crossfade completed to " 
                      << (transitionToKeylock ? "KEYLOCK" : "NORMAL") << " mode" << std::endl;
        }
    }
}

void DJAudioPlayer::renderScratchAudio(const AudioSourceChannelInfo &bufferToFill) {
    if (bufferToFill.buffer == nullptr || bufferToFill.numSamples <= 0) {
        return;
    }

    auto* outputBuffer = bufferToFill.buffer;
    const int destChannels = outputBuffer->getNumChannels();
    outputBuffer->clear(bufferToFill.startSample, bufferToFill.numSamples);

    AudioFormatReader* reader = readerSource ? readerSource->getAudioFormatReader() : nullptr;
    if (reader == nullptr || reader->sampleRate <= 0.0) {
        scratchAudioSeconds.store(0.0);
        scratchPrevSampleL = 0.0f;
        scratchPrevSampleR = 0.0f;
        scratchFadeSamplesRemaining = 0;
        scratchFadeSamplesTotal = 0;
        return;
    }

    const double readerSampleRate = reader->sampleRate;
    double outputRate = currentSampleRate > 0.0 ? currentSampleRate : readerSampleRate;
    if (outputRate <= 0.0) {
        outputRate = readerSampleRate;
    }
    const double invOutputRate = 1.0 / outputRate;

    const bool contextWasPlaying = scratchContextWasPlaying.load();
    const bool keylockActive = keylockEnabled;

    auto sanitizeVelocity = [&](double raw) {
        if (!std::isfinite(raw)) {
            return 0.0;
        }
        const double baseLimit = contextWasPlaying ? 16.0 : 12.0;
        const double limit = std::max(0.6, keylockActive ? baseLimit * 0.85 : baseLimit);
        if (limit <= 0.0) {
            return 0.0;
        }
        const double normalised = raw / limit;
        if (std::abs(normalised) < 1e-5) {
            return raw;
        }
        return limit * std::tanh(normalised);
    };

    double velocityTarget = sanitizeVelocity(scratchVelocity.load());

    if (scratchJumpPending.exchange(false)) {
        double jumpSeconds = scratchTargetSeconds.load();
        scratchCurrentSeconds = jumpSeconds;
        scratchAudioSeconds.store(jumpSeconds);
        scratchSmoothedVelocity = sanitizeVelocity(scratchVelocity.load());
        scratchFadeSamplesRemaining = std::min(bufferToFill.numSamples,
                                               (int) std::round(outputRate * 0.004)); // ~4ms fade
        scratchFadeSamplesTotal = scratchFadeSamplesRemaining;
        scratchFadeStartL = scratchPrevSampleL;
        scratchFadeStartR = scratchPrevSampleR;
    }

    const double blockSeconds = std::max(invOutputRate, bufferToFill.numSamples * invOutputRate);
    const double maxAccelPerSec = (contextWasPlaying ? 72.0 : 54.0) * (keylockActive ? 0.85 : 1.0);
    const double maxDelta = std::max(0.0, maxAccelPerSec) * blockSeconds;
    if (maxDelta > 0.0) {
        velocityTarget = juce::jlimit(scratchSmoothedVelocity - maxDelta,
                                      scratchSmoothedVelocity + maxDelta,
                                      velocityTarget);
    }

    const double smoothingTime = (contextWasPlaying ? 0.0075 : 0.012) * (keylockActive ? 1.05 : 1.0);
    double alpha = 0.0;
    if (smoothingTime > 0.0) {
        alpha = std::exp(-blockSeconds / smoothingTime);
    }
    alpha = juce::jlimit(0.0, 0.999, alpha);
    scratchSmoothedVelocity = scratchSmoothedVelocity * alpha + velocityTarget * (1.0 - alpha);
    if (std::abs(scratchSmoothedVelocity) < 1e-5) {
        scratchSmoothedVelocity = 0.0;
    }

    double minSeconds = -prerollTimeSec;
    double maxSeconds = transportSource.getLengthInSeconds();
    if (maxSeconds <= 0.0 && trackLengthSec > 0.0) {
        maxSeconds = trackLengthSec;
    }
    if (maxSeconds <= 0.0 && reader->lengthInSamples > 0) {
        maxSeconds = (reader->lengthInSamples - 1) / readerSampleRate;
    }
    if (maxSeconds < 0.0) {
        maxSeconds = 0.0;
    }

    double leftSumSq = 0.0;
    double rightSumSq = 0.0;

    for (int i = 0; i < bufferToFill.numSamples; ++i) {
        const double positionSeconds = scratchCurrentSeconds;
        float sampleL = 0.0f;
        float sampleR = 0.0f;

        if (positionSeconds >= 0.0 && positionSeconds <= maxSeconds + invOutputRate) {
            double exactSample = positionSeconds * readerSampleRate;
            sampleL = fetchScratchSample(reader, exactSample, 0);
            if (reader->numChannels > 1) {
                sampleR = fetchScratchSample(reader, exactSample, 1);
            } else {
                sampleR = sampleL;
            }
        }

        if (scratchFadeSamplesRemaining > 0 && scratchFadeSamplesTotal > 0) {
            double fadeProgress = 1.0 - (double) scratchFadeSamplesRemaining / (double) scratchFadeSamplesTotal;
            sampleL = scratchFadeStartL * (1.0 - fadeProgress) + sampleL * fadeProgress;
            sampleR = scratchFadeStartR * (1.0 - fadeProgress) + sampleR * fadeProgress;
            --scratchFadeSamplesRemaining;
        }

        const int destIndex = bufferToFill.startSample + i;
        if (destChannels >= 1) {
            outputBuffer->setSample(0, destIndex, sampleL);
        }
        if (destChannels >= 2) {
            outputBuffer->setSample(1, destIndex, sampleR);
        }
        for (int ch = 2; ch < destChannels; ++ch) {
            outputBuffer->setSample(ch, destIndex, sampleL);
        }

        leftSumSq += static_cast<double>(sampleL) * static_cast<double>(sampleL);
        rightSumSq += static_cast<double>(sampleR) * static_cast<double>(sampleR);

        scratchCurrentSeconds += scratchSmoothedVelocity * invOutputRate;

        if (scratchCurrentSeconds < minSeconds) {
            scratchCurrentSeconds = minSeconds;
        }
        if (scratchCurrentSeconds > maxSeconds) {
            scratchCurrentSeconds = maxSeconds;
        }
    }

    if (bufferToFill.numSamples > 0) {
        scratchPrevSampleL = outputBuffer->getSample(0, bufferToFill.startSample + bufferToFill.numSamples - 1);
        if (destChannels >= 2) {
            scratchPrevSampleR = outputBuffer->getSample(1, bufferToFill.startSample + bufferToFill.numSamples - 1);
        } else {
            scratchPrevSampleR = scratchPrevSampleL;
        }
    }

    scratchAudioSeconds.store(scratchCurrentSeconds);

    if (bufferToFill.numSamples > 0) {
        float leftRMS = std::sqrt((float) (leftSumSq / bufferToFill.numSamples));
        float rightRMS = std::sqrt((float) (rightSumSq / bufferToFill.numSamples));

        const float dbMin = -60.0f;
        const float dbMax = 0.0f;
        float leftDb = leftRMS > 0.0f ? 20.0f * std::log10(leftRMS) : dbMin;
        float rightDb = rightRMS > 0.0f ? 20.0f * std::log10(rightRMS) : dbMin;
        float leftPercent = juce::jlimit(0.0f, 100.0f, ((leftDb - dbMin) / (dbMax - dbMin)) * 100.0f);
        float rightPercent = juce::jlimit(0.0f, 100.0f, ((rightDb - dbMin) / (dbMax - dbMin)) * 100.0f);

        const float smoothing = 0.3f;
        float currentLeft = leftChannelLevel.load();
        float currentRight = rightChannelLevel.load();
        leftChannelLevel.store(currentLeft * (1.0f - smoothing) + leftPercent * smoothing);
        rightChannelLevel.store(currentRight * (1.0f - smoothing) + rightPercent * smoothing);
    }
}

void DJAudioPlayer::ensureScratchCache(AudioFormatReader* reader, int64 sampleIndex) {
    if (reader == nullptr) {
        scratchCacheValid = false;
        scratchCacheValidSamples = 0;
        scratchCacheStartSample = 0;
        return;
    }

    const int requiredChannels = std::max(1, (int) reader->numChannels);
    const int chunkSize = std::max(SCRATCH_CACHE_SAMPLES, lastBlockSizeHint * 4);

    if (!scratchCacheValid || sampleIndex < scratchCacheStartSample ||
        sampleIndex + 2 >= scratchCacheStartSample + scratchCacheValidSamples) {

        if (scratchCacheBuffer.getNumChannels() < requiredChannels ||
            scratchCacheBuffer.getNumSamples() < chunkSize) {
            scratchCacheBuffer.setSize(requiredChannels, chunkSize, false, false, true);
        }

        scratchCacheBuffer.clear();

        const int64 totalSamples = reader->lengthInSamples;
        if (totalSamples <= 0) {
            scratchCacheValid = false;
            scratchCacheValidSamples = 0;
            scratchCacheStartSample = 0;
            return;
        }

        int64 start = sampleIndex - chunkSize / 2;
        if (start < 0) {
            start = 0;
        }
        if (start > totalSamples - 1) {
            start = totalSamples - 1;
        }

        int samplesToRead = (int) std::min<int64>(chunkSize, totalSamples - start);
        if (samplesToRead <= 0) {
            scratchCacheValid = false;
            scratchCacheValidSamples = 0;
            scratchCacheStartSample = start;
            return;
        }

        reader->read(&scratchCacheBuffer, 0, samplesToRead, start, true, requiredChannels > 1);
        scratchCacheStartSample = start;
        scratchCacheValidSamples = samplesToRead;
        scratchCacheValid = true;
    }
}

float DJAudioPlayer::fetchScratchSample(AudioFormatReader* reader, double samplePos, int channel) {
    if (reader == nullptr || samplePos < 0.0) {
        return 0.0f;
    }

    int64 index = (int64) std::floor(samplePos);
    if (index < 0 || index >= reader->lengthInSamples) {
        return 0.0f;
    }

    ensureScratchCache(reader, index);

    if (!scratchCacheValid || scratchCacheValidSamples <= 0) {
        return 0.0f;
    }

    int localIndex = (int) (index - scratchCacheStartSample);
    if (localIndex < 0) {
        localIndex = 0;
    } else if (localIndex >= scratchCacheValidSamples) {
        localIndex = scratchCacheValidSamples - 1;
    }
    int nextIndex = std::min(localIndex + 1, scratchCacheValidSamples - 1);

    int bufferChannel = juce::jlimit(0, scratchCacheBuffer.getNumChannels() - 1, channel);
    float sample1 = scratchCacheBuffer.getSample(bufferChannel, localIndex);
    float sample2 = scratchCacheBuffer.getSample(bufferChannel, nextIndex);
    float frac = static_cast<float>(samplePos - (double) index);
    return sample1 + (sample2 - sample1) * frac;
}

void DJAudioPlayer::releaseResources() {
    try {
        transportSource.stop();
        resampleSource.releaseResources();
        transportSource.releaseResources();
        
#if defined(RUBBERBAND_FOUND)
        rb.reset();
        rbReady = false;
#endif
    } catch (const std::exception& e) {
        std::cout << "Exception in releaseResources: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "Unknown exception in releaseResources" << std::endl;
    }
}

void DJAudioPlayer::loadFile(const File &file) {
    AudioFormatReader* reader = nullptr;
    {
        AudioFormatManagerGuard formatGuard;
        reader = formatManager.createReaderFor(file);
    }

    if (reader != nullptr) [[likely]] {
        auto newSource = std::make_unique<AudioFormatReaderSource>(reader, true);
        transportSource.setSource(newSource.get(), 0, nullptr, reader->sampleRate);
        readerSource = std::move(newSource);
        
        if (dspPrepared && currentSampleRate > 0.0 && lastBlockSizeHint > 0) [[likely]] {
            try {
                transportSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
                resampleSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
            } catch (...) {}
        }
        
#if defined(RUBBERBAND_FOUND)
        reinitRubberBand();
#endif
        
        transportSource.setPosition(0.0);
        pausedPosSec = 0.0;
        prerollPosition = 0.0;
        inPrerollMode = false;
        softPaused.store(false);
    }
}

void DJAudioPlayer::applyLoadedSource(std::unique_ptr<AudioFormatReaderSource> source, double sampleRate) {
    if (source && source->getAudioFormatReader()) [[likely]] {
        bool wasPlaying = transportSource.isPlaying();
        if (wasPlaying) {
            transportSource.stop();
        }
        
        transportSource.setSource(source.get(), 0, nullptr, sampleRate);
        readerSource = std::move(source);
        
        if (dspPrepared && currentSampleRate > 0.0 && lastBlockSizeHint > 0) [[likely]] {
            try {
                transportSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
                resampleSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
            } catch (...) {}
        }
        
#if defined(RUBBERBAND_FOUND)
        reinitRubberBand();
#endif
        
        transportSource.setPosition(0.0);
        pausedPosSec = 0.0;
        prerollPosition = 0.0;
        inPrerollMode = false;
        softPaused.store(false);
        
        if (wasPlaying) {
            transportSource.start();
        }
    }
}

void DJAudioPlayer::setGain(double gain) {
    transportSource.setGain(std::clamp(gain, 0.0, 1.0));
}

[[nodiscard]] double DJAudioPlayer::effectiveSpeed() const noexcept {
    const double speed = currentSpeed * pitchBendRatio;
    if (std::isfinite(speed) && speed > 0.0) [[likely]] {
        return fastClamp(speed, kMinSpeedRatio, kMaxSpeedRatio);
    }
    return 1.0;
}

void DJAudioPlayer::updateResampleRatio() noexcept {
#if defined(RUBBERBAND_FOUND)
    if (rbReady && rb) {
        const double ratio = effectiveSpeed();
        resampleSource.setResamplingRatio(keylockEnabled ? 1.0 : ratio);
        return;
    }
#endif
    resampleSource.setResamplingRatio(effectiveSpeed());
}

void DJAudioPlayer::setSpeed(double ratio) {
    if (ratio < 0.0 || ratio > 100.0) [[unlikely]] return;
    
    currentSpeed = ratio;
    updateResampleRatio();
}

void DJAudioPlayer::setPitchBendRatio(double ratio) noexcept {
    constexpr double kEpsilon = 1e-4;
    const double clamped = fastClamp(std::isfinite(ratio) ? ratio : 1.0, 
                                     kPitchBendMinRatio, kPitchBendMaxRatio);
    
    if (std::abs(clamped - pitchBendRatio) < kEpsilon) [[unlikely]] return;
    
    pitchBendRatio = clamped;
    updateResampleRatio();
}

void DJAudioPlayer::setPosition(double posInSecs) {
    if (posInSecs < 0 || posInSecs > transportSource.getLengthInSeconds()) [[unlikely]] {
        return;
    }
    
    double finalPos = quantizePosition(posInSecs);
    transportSource.setPosition(finalPos);
    
    if (!transportSource.isPlaying() || softPaused.load()) {
        pausedPosSec = finalPos;
    }
}

void DJAudioPlayer::setPositionRelative(double pos) {
    constexpr double kMinRelativePos = -999.0;
    
    if (pos < kMinRelativePos || pos > 1.0) [[unlikely]] {
        return;
    }
    
    if (scratchMode.load()) {
        double targetSeconds = 0.0;
        if (pos < 0.0) {
            targetSeconds = pos * prerollTimeSec;
        } else {
            double trackLen = transportSource.getLengthInSeconds();
            if (trackLen <= 0.0 && trackLengthSec > 0.0) {
                    trackLen = trackLengthSec;
                }
                if (trackLen <= 0.0 && readerSource) {
                    if (auto* reader = readerSource->getAudioFormatReader()) {
                        if (reader->sampleRate > 0.0) {
                            trackLen = (double) reader->lengthInSamples / reader->sampleRate;
                        }
                    }
                }
                if (trackLen > 0.0) {
                    targetSeconds = pos * trackLen;
                } else {
                    targetSeconds = 0.0;
                }
            }

            scratchTargetSeconds.store(targetSeconds);
            scratchAudioSeconds.store(targetSeconds);
            scratchJumpPending.store(true);
            if (targetSeconds < 0.0) {
                inPrerollMode = true;
                double denom = std::max(0.001, prerollTimeSec);
                prerollPosition = juce::jlimit(-1.0, 0.0, targetSeconds / denom);
            } else {
                inPrerollMode = false;
                prerollPosition = 0.0;
            }
            return;
        }

        if (pos < 0.0) {
            transportSource.setPosition(0.0);
            prerollPosition = pos;
            inPrerollMode = true;
            pausedPosSec = 0.0;
        } else {
            inPrerollMode = false;
            prerollPosition = 0.0;
            const double relativePos = trackLengthSec * pos;
            double finalPos = quantizePosition(relativePos);
            setPosition(finalPos);
            
            if (!transportSource.isPlaying() || softPaused.load()) {
                pausedPosSec = finalPos;
            }
        }
}

[[nodiscard]] double DJAudioPlayer::getPositionRelative() {
    if (scratchMode.load()) {
        double seconds = scratchAudioSeconds.load();
        if (seconds < 0.0) {
            double denom = std::max(0.001, prerollTimeSec);
            return juce::jlimit(-1.0, 0.0, seconds / denom);
        }

        double lengthInSecs = transportSource.getLengthInSeconds();
        if (lengthInSecs <= 0.0 && trackLengthSec > 0.0) {
            lengthInSecs = trackLengthSec;
        }
        if (lengthInSecs <= 0.0 && readerSource) {
            if (auto* reader = readerSource->getAudioFormatReader()) {
                if (reader->sampleRate > 0.0) {
                    lengthInSecs = (double) reader->lengthInSamples / reader->sampleRate;
                }
            }
        }

        if (lengthInSecs > 0.0) {
            return juce::jlimit(0.0, 1.0, seconds / lengthInSecs);
        }
        return 0.0;
    }

    if (inPrerollMode) {
        return prerollPosition;  // Return the negative position
    }
    
    double currentPosInSecs = transportSource.getCurrentPosition();
    double lengthInSecs = transportSource.getLengthInSeconds();

    if (lengthInSecs == 0.0) {
        return 0.0;
    }

    return currentPosInSecs / lengthInSecs;
}

double DJAudioPlayer::getCurrentPositionSeconds() const {
    if (scratchMode.load()) {
        return scratchAudioSeconds.load();
    }
    // PREROLL SUPPORT: Report negative time while in preroll to keep UI stable
    if (inPrerollMode) {
        return prerollPosition * prerollTimeSec;
    }

    return transportSource.getCurrentPosition();
}

void DJAudioPlayer::start() {
    try {
        if (readerSource.get() != nullptr) {
            if (inPrerollMode) {
                transportSource.setPosition(0.0);
            }
            
            double currentPos = transportSource.getCurrentPosition();
            double totalLength = transportSource.getLengthInSeconds();
            
            if (currentPos >= totalLength - 0.1) {
                transportSource.setPosition(0.0);
                pausedPosSec = 0.0;
            }
            
            transportSource.setLooping(true);
            
            if (pausedPosSec > 0.0 && pausedPosSec <= totalLength) {
                transportSource.setPosition(pausedPosSec);
            }
            
            softPaused.store(false);
            forceSilent.store(false);
            pausedResetPending.store(false);
            resumeCompensatePending = keylockEnabled;
            
            fadeSamplesTotal = static_cast<int>(std::ceil((kFadeInMs / 1000.0) * currentSampleRate));
            fadeSamplesRemaining = fadeSamplesTotal;
            fadeStartGain = 0.0f;
            fadeTargetGain = 1.0f;
            startFadeActive = true;
            stopFadeActive = false;
            
            transportSource.start();
        }
    } catch (const std::exception& e) {
        std::cout << "Exception in DJAudioPlayer::start(): " << e.what() << std::endl;
    } catch (...) {
        std::cout << "Unknown exception in DJAudioPlayer::start()" << std::endl;
    }
}

void DJAudioPlayer::stop() {
    try {
        fadeSamplesTotal = static_cast<int>(std::ceil((kFadeOutMs / 1000.0) * currentSampleRate));
        fadeSamplesRemaining = fadeSamplesTotal;
        fadeStartGain = 1.0f;
        fadeTargetGain = 0.0f;
        stopFadeActive = true;
        startFadeActive = false;
        
        softPaused.store(true);
        pausedPosSec = transportSource.getCurrentPosition();
        pausedResetPending.store(true);
    } catch (...) {}
}

void DJAudioPlayer::unload() {
    try {
        // First: disable all processing modes
        forceSilent.store(true);
        softPaused.store(true);
        scratchMode.store(false);
        
        // Stop transport
        transportSource.stop();
        
        // Reset playback state
        inPrerollMode = false;
        prerollPosition = 0.0;
        pausedPosSec = 0.0;
        loopEnabled = false;
        loopStartSec = 0.0;
        loopEndSec = 0.0;

#if defined(RUBBERBAND_FOUND)
        // Clean up RubberBand BEFORE disconnecting sources
        // This prevents RB from trying to access deleted audio data
        if (rb) {
            rb.reset();
        }
        rbReady = false;
        rbPaddedStartDone = false;
        rbDiscardOutRemaining = 0;
        rbLatencySamples = 0;
        rbLatencySeconds = 0.0;
        measuredLatencyMs.store(0.0);
        latencyCompensationSamples = 0;
#endif

        // Now safe to disconnect and release resources
        transportSource.setSource(nullptr);
        readerSource.reset();

        try { resampleSource.releaseResources(); } catch (...) {}
        try { transportSource.releaseResources(); } catch (...) {}
        
    } catch (const std::exception& e) {
        std::cout << "Exception in DJAudioPlayer::unload(): " << e.what() << std::endl;
    } catch (...) {
        std::cout << "Unknown exception in DJAudioPlayer::unload()" << std::endl;
    }
}

#if defined(RUBBERBAND_FOUND)
void DJAudioPlayer::reinitRubberBand() {
    if (currentSampleRate <= 0.0) {
        rb.reset();
        rbReady = false;
        return;
    }
    
    int sourceChannels = 1;
    if (readerSource && readerSource->getAudioFormatReader()) {
        sourceChannels = readerSource->getAudioFormatReader()->numChannels;
    }
    rbNumChannels = std::min(sourceChannels, 2);
    
    RubberBand::RubberBandStretcher::Options opts =
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionThreadingAuto;

    switch (rbQuality) {
        case KeylockQuality::Fast:
            opts |= RubberBand::RubberBandStretcher::OptionEngineFaster |
                    RubberBand::RubberBandStretcher::OptionTransientsCrisp |
                    RubberBand::RubberBandStretcher::OptionWindowShort |
                    RubberBand::RubberBandStretcher::OptionPitchHighSpeed |
                    RubberBand::RubberBandStretcher::OptionChannelsTogether;
            break;
        case KeylockQuality::Balanced:
            // OPTIMIZED: Use faster settings for lower latency (DJ use case)
            opts |= RubberBand::RubberBandStretcher::OptionEngineFaster |  // Changed from Finer
                    RubberBand::RubberBandStretcher::OptionTransientsCrisp |  // Changed from Mixed
                    RubberBand::RubberBandStretcher::OptionWindowShort |      // Changed from Standard
                    RubberBand::RubberBandStretcher::OptionPitchHighSpeed |
                    RubberBand::RubberBandStretcher::OptionChannelsTogether;
            break;
        case KeylockQuality::Quality:
            // OPTIMIZED: Still use finer engine but with shorter window for lower latency
            opts |= RubberBand::RubberBandStretcher::OptionEngineFiner |
                    RubberBand::RubberBandStretcher::OptionTransientsCrisp |  // Changed from Smooth
                    RubberBand::RubberBandStretcher::OptionWindowShort |      // Changed from Standard
                    RubberBand::RubberBandStretcher::OptionPitchHighQuality |
                    RubberBand::RubberBandStretcher::OptionChannelsTogether;
            break;
    }
    try {
        rb = std::make_unique<RubberBand::RubberBandStretcher>(currentSampleRate, rbNumChannels, opts);
        rb->setTimeRatio(1.0);
        rb->setPitchScale(1.0);
        // ULTRA-OPTIMIZED: Sehr kleine Chunks für minimale Latenz
        // Reduziert interne RubberBand Buffering drastisch
        const size_t optimizedProcessSize = std::min(128, std::max(32, lastBlockSizeHint / 4)); // Reduced from 256 and /2
        rb->setMaxProcessSize(optimizedProcessSize);
        rbLastTimeRatio = 1.0;
    rbLastPitchScale = 1.0;
        rbInputBuffer.setSize(rbNumChannels, std::max(256, lastBlockSizeHint));
        rbInputBuffer.clear();
        rbReady = true;
        rbPaddedStartDone = false;
        rbLatencySamples = (int)rb->getStartDelay();
        rbLatencySeconds = rbLatencySamples / currentSampleRate;
        rbDiscardOutRemaining = 0;
        rbOutScratch.setSize(rbNumChannels, std::max(256, lastBlockSizeHint));
        rbOutScratch.clear();
    } catch (const std::exception& e) {
        std::cout << "RubberBand init failed: " << e.what() << std::endl;
        rb.reset();
        rbReady = false;
    } catch (...) {
        std::cout << "RubberBand init failed: unknown error" << std::endl;
        rb.reset();
        rbReady = false;
    }
}

void DJAudioPlayer::setKeylockQuality(KeylockQuality q) {
    if (q == rbQuality) return;
    rbQuality = q;
    if (keylockEnabled) {
        // Recreate stretcher with new profile
        reinitRubberBand();
    }
}
#endif

bool DJAudioPlayer::isPlaying() {
    return transportSource.isPlaying() && !softPaused.load() && !forceSilent.load();
}

void DJAudioPlayer::setHighGain(double v) noexcept { highGain = fastClamp(v, -1.0, 1.0); }
void DJAudioPlayer::setMidGain(double v) noexcept { midGain = fastClamp(v, -1.0, 1.0); }
void DJAudioPlayer::setLowGain(double v) noexcept { lowGain = fastClamp(v, -1.0, 1.0); }
void DJAudioPlayer::setFilterCutoff(double v) noexcept { filterKnob = fastClamp(v, -1.0, 1.0); }

void DJAudioPlayer::enableLoop(double startSec, double lengthSec) {
    if (lengthSec <= 0.0) [[unlikely]] { 
        disableLoop(); 
        return; 
    }
    
    double len = transportSource.getLengthInSeconds();
    loopStartSec = std::clamp(startSec, 0.0, len);
    loopEndSec = std::clamp(loopStartSec + lengthSec, loopStartSec, len);
    loopEnabled = (loopEndSec > loopStartSec);
}

void DJAudioPlayer::disableLoop() noexcept {
    loopEnabled = false;
    loopStartSec = 0.0;
    loopEndSec = 0.0;
}

void DJAudioPlayer::setScratchVelocity(double velocity) {
    if (!std::isfinite(velocity)) [[unlikely]] {
        velocity = 0.0;
    }
    const double maxSpeed = 8.0;
    velocity = juce::jlimit(-maxSpeed, maxSpeed, velocity);
    scratchVelocity.store(velocity);
}

void DJAudioPlayer::setScratchPlaybackContext(bool wasPlaying) noexcept {
    scratchContextWasPlaying.store(wasPlaying);
}

void DJAudioPlayer::enableScratch(bool enable) {
    bool current = scratchMode.load();
    if (enable == current) {
        if (!enable) {
            scratchVelocity.store(0.0);
        }
        return;
    }

    if (enable) {
        scratchMode.store(true);
        scratchVelocity.store(0.0);
        scratchSmoothedVelocity = 0.0;
        scratchCacheValid = false;
        scratchFadeSamplesRemaining = 0;
        scratchFadeSamplesTotal = 0;
        scratchPrevSampleL = 0.0f;
        scratchPrevSampleR = 0.0f;
        scratchFadeStartL = 0.0f;
        scratchFadeStartR = 0.0f;

        double currentSec = getCurrentPositionSeconds();
        scratchCurrentSeconds = currentSec;
        scratchTargetSeconds.store(currentSec);
        scratchAudioSeconds.store(currentSec);
        scratchJumpPending.store(true);
        if (!scratchContextWasPlaying.load()) {
            scratchContextWasPlaying.store(false);
        }

        // Make sure preroll state mirrors the scratch position for UI queries
        if (currentSec < 0.0) {
            inPrerollMode = true;
            prerollPosition = currentSec / std::max(0.001, prerollTimeSec);
        } else {
            inPrerollMode = false;
            prerollPosition = 0.0;
        }

        pausedResetPending.store(true);
    } else {
        scratchMode.store(false);
        scratchJumpPending.store(false);
        scratchCacheValid = false;
        scratchFadeSamplesRemaining = 0;
        scratchFadeSamplesTotal = 0;

        double finalSec = scratchAudioSeconds.load();
        scratchVelocity.store(0.0);
        scratchSmoothedVelocity = 0.0;

        if (finalSec < 0.0) {
            inPrerollMode = true;
            double denom = std::max(0.001, prerollTimeSec);
            prerollPosition = juce::jlimit(-1.0, 0.0, finalSec / denom);
            transportSource.setPosition(0.0);
            pausedPosSec = 0.0;
        } else {
            inPrerollMode = false;
            prerollPosition = 0.0;
            double trackLen = transportSource.getLengthInSeconds();
            if (trackLen <= 0.0 && trackLengthSec > 0.0) {
                trackLen = trackLengthSec;
            }
            if (trackLen > 0.0) {
                double clamped = juce::jlimit(0.0, trackLen, finalSec);
                transportSource.setPosition(clamped);
                pausedPosSec = clamped;
            } else {
                transportSource.setPosition(std::max(0.0, finalSec));
                pausedPosSec = std::max(0.0, finalSec);
            }
        }

        pausedResetPending.store(true);
        scratchContextWasPlaying.store(false);
        if (keylockEnabled) {
            resumeCompensatePending = true;
        }
    }
}

[[nodiscard]] bool DJAudioPlayer::isSoftPaused() const noexcept {
    return softPaused.load();
}

[[nodiscard]] bool DJAudioPlayer::isAudible() const noexcept {
    return transportSource.isPlaying() && !softPaused.load() && !forceSilent.load();
}

void DJAudioPlayer::ensureScratchAudible() {
    forceSilent.store(false);
    if (softPaused.exchange(false)) {
        pausedResetPending.store(false);
        resumeCompensatePending = keylockEnabled;
    }

    if (!transportSource.isPlaying() && readerSource) {
        transportSource.setLooping(true);
        double len = transportSource.getLengthInSeconds();
        if (pausedPosSec > 0.0 && pausedPosSec <= len) {
            transportSource.setPosition(pausedPosSec);
        }
        transportSource.start();
    }
}

void DJAudioPlayer::setKeylockEnabled(bool enabled) noexcept {
    keylockChangePending.store(enabled ? 1 : 0);
}

void DJAudioPlayer::setQuantizeEnabled(bool enabled) noexcept {
    quantizeEnabled = enabled;
}

void DJAudioPlayer::setBeatInfo(double bpm, double firstBeatOffset, double trackLength) noexcept {
    trackBpm = bpm;
    trackFirstBeatOffset = firstBeatOffset;
    trackLengthSec = trackLength;
}

[[nodiscard]] double DJAudioPlayer::quantizePosition(double positionSec) const noexcept {
    if (!quantizeEnabled || trackBpm <= 0.0) [[unlikely]] return positionSec;
    
    const double beatLengthSec = 60.0 / trackBpm;
    const double relativePos = positionSec - trackFirstBeatOffset;
    const double beatNumber = std::round(relativePos / beatLengthSec);
    
    return fastClamp(std::fma(beatNumber, beatLengthSec, trackFirstBeatOffset), 
                     0.0, trackLengthSec);
}