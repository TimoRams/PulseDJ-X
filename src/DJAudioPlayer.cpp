// Copied from project root
#include "DJAudioPlayer.h"
#include <QDebug>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

DJAudioPlayer::DJAudioPlayer(AudioFormatManager &_formatManager) : formatManager(_formatManager) {
    // Initialize with safe defaults
    transportSource.setGain(1.0f);
    resampleSource.setResamplingRatio(1.0);
    currentSpeed = 1.0;
    pitchShiftRatio = 1.0;
}

DJAudioPlayer::~DJAudioPlayer() {
    // Safe destruction sequence to avoid segfaults
    try {
        // 1. Stop transport first
        transportSource.stop();
        
        // 2. Disconnect transport source to prevent callbacks during destruction
        transportSource.setSource(nullptr);
        
        // 3. Release resources in correct order
        resampleSource.releaseResources();
        transportSource.releaseResources();
        
        // 4. Clear reader source
        readerSource.reset();
        
#if defined(RUBBERBAND_FOUND)
    rb.reset();
#endif
        
    } catch (const std::exception& e) {
        std::cout << "Exception in DJAudioPlayer destructor: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "Unknown exception in DJAudioPlayer destructor" << std::endl;
    }
}

void DJAudioPlayer::prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
    std::cout << "DJAudioPlayer::prepareToPlay called with " << samplesPerBlockExpected << " samples, " << sampleRate << "Hz" << std::endl;
    transportSource.prepareToPlay(samplesPerBlockExpected, sampleRate);
    resampleSource.prepareToPlay(samplesPerBlockExpected, sampleRate);
    currentSampleRate = sampleRate;
    
    // PERFORMANCE: Initialize audio buffer pool for better memory management with multiple songs
    for (auto& buffer : audioBufferPool) {
        buffer = std::make_unique<AudioBuffer<float>>(2, samplesPerBlockExpected * 2); // Double buffer for safety
        buffer->clear(); // Initialize to silence
    }
    
    lastBlockSizeHint = samplesPerBlockExpected;
    
    // Prepare DSP filters for maximum possible channels (up to stereo)
    juce::dsp::ProcessSpec spec{ sampleRate, (uint32) samplesPerBlockExpected, 2 };
    lowShelf.reset(); lowShelf.prepare(spec);
    midPeak.reset(); midPeak.prepare(spec);
    highShelf.reset(); highShelf.prepare(spec);
    svf.reset(); svf.prepare(spec);

    std::cout << "DSP filters prepared for max " << spec.numChannels << " channels, audio pool initialized" << std::endl;

    // Pre-calculate and cache neutral coefficients for performance
    cachedLowCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentSampleRate, 250.0f, 0.707f, 1.0f);
    cachedMidCoeffs = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 2500.0f, 1.0f, 1.0f);
    cachedHighCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentSampleRate, 10000.0f, 0.707f, 1.0f);
    
    lowShelf.coefficients = cachedLowCoeffs;
    midPeak.coefficients = cachedMidCoeffs;
    highShelf.coefficients = cachedHighCoeffs;

    // Initialize SVF for optimal performance
    svf.setCutoffFrequency(1000.0f);
    svf.setResonance(0.7f);

    dspPrepared = true;
    std::cout << "Enhanced DSP initialization complete with memory optimizations" << std::endl;

#if defined(RUBBERBAND_FOUND)
    // Initialize RubberBand for keylock functionality
    reinitRubberBand();
    std::cout << "RubberBand keylock initialized successfully" << std::endl;
#else
    #error "RubberBand is required for keylock functionality"
#endif

    // Compute prime samples for keylock based on current block size and SR
    keylockPrimeSamplesRemaining = (int) std::ceil((keylockPrimeMs / 1000.0) * currentSampleRate);
}

void DJAudioPlayer::getNextAudioBlock(const AudioSourceChannelInfo &bufferToFill) {
    if (readerSource.get() == nullptr) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }
    lastBlockSizeHint = bufferToFill.numSamples;
    
    // DEBUG: Check if we're being called and if transport is playing
    static int debugCallCount = 0;
    if (debugCallCount++ % 1000 == 0) {
        std::cout << "[DJAP] getNextAudioBlock called #" << debugCallCount 
                  << ", transport playing: " << transportSource.isPlaying() 
                  << ", soft paused: " << softPaused.load() << std::endl;
    }

    // Apply deferred keylock toggle on the audio thread for thread safety
    int pendingKL = keylockChangePending.exchange(-1);
    if (pendingKL != -1) {
        const bool enable = (pendingKL == 1);
        keylockEnabled = enable;
        if (debugKeylock) {
            std::cout << "[KL] Toggle: " << (enable ? "ON" : "OFF") << ", SR=" << currentSampleRate
                      << ", lastBlockSizeHint=" << lastBlockSizeHint << std::endl;
        }
        if (enable) {
            resampleSource.setResamplingRatio(1.0);
#if defined(RUBBERBAND_FOUND)
            // Start RubberBand when keylock is enabled for 24/7 operation (no lag)
            if (!rbReady) {
                rbReady = true;
                rbPaddedStartDone = false;
                rbDiscardOutRemaining = 0;
                if (debugKeylock) std::cout << "[KL] RB started for 24/7 mode" << std::endl;
            }
#endif
            // Start a brief prime period to accumulate input only if RB wasn't already running
            if (!rbReady) {
                keylockPrimeSamplesRemaining = (int) std::ceil((keylockPrimeMs / 1000.0) * currentSampleRate);
            }
        } else {
            resampleSource.setResamplingRatio(currentSpeed);
#if defined(RUBBERBAND_FOUND)
            // Keep RB running when keylock is disabled for instant re-activation
            // RB will run in pass-through mode at unity speed
            if (rbReady && debugKeylock) {
                std::cout << "[KL] RB staying active for instant re-enable" << std::endl;
            }
#endif
        }
    }

    // Immediate silence requested (e.g., right after stop) or soft-paused (keep transport running)
    if (forceSilent.load() || softPaused.load()) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    // If paused/stopped, clear once and avoid repeated heavy resets every callback
    if (!transportSource.isPlaying()) {
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
    
    // Loop checking: Must be done every buffer for precise timing with click-free crossfade
    if (loopEnabled) {
        double pos = transportSource.getCurrentPosition();
        double nextPos = pos + (double(bufferToFill.numSamples) / currentSampleRate);
        
        // Handle active crossfade first
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
        
        // Check if we will cross the loop end point in this buffer
        if (pos < loopEndSec && nextPos >= loopEndSec && loopEndSec > loopStartSec) {
            // Calculate how many samples until loop end
            double timeToLoopEnd = loopEndSec - pos;
            int samplesToLoopEnd = (int)(timeToLoopEnd * currentSampleRate);
            
            // Clamp to buffer boundaries
            samplesToLoopEnd = std::max(0, std::min(samplesToLoopEnd, bufferToFill.numSamples));
            
            // IMPROVED: Longer crossfade with better pre-buffering for ultra-smooth loops
            const int crossfadeLength = std::min(1024, std::min(samplesToLoopEnd, bufferToFill.numSamples / 2));
            
            if (crossfadeLength >= 16 && samplesToLoopEnd >= crossfadeLength) {
                // ADVANCED STRATEGY: Pre-buffer both end and start with overlap compensation
                
                // Step 1: Get the complete current buffer (end portion)
                AudioBuffer<float> endBuffer(bufferToFill.buffer->getNumChannels(), bufferToFill.numSamples);
                AudioSourceChannelInfo endInfo;
                endInfo.buffer = &endBuffer;
                endInfo.startSample = 0;
                endInfo.numSamples = bufferToFill.numSamples;
                
                if (keylockEnabled) {
                    resampleSource.setResamplingRatio(1.0);
                } else {
                    resampleSource.setResamplingRatio(currentSpeed);
                }
                resampleSource.getNextAudioBlock(endInfo);
                
                // Step 2: Store current position and jump to loop start
                double currentPos = transportSource.getCurrentPosition();
                transportSource.setPosition(loopStartSec);
                
                // Step 3: Get extended start portion for seamless crossfade
                const int startBufferSize = std::max(crossfadeLength * 2, bufferToFill.numSamples);
                AudioBuffer<float> startBuffer(bufferToFill.buffer->getNumChannels(), startBufferSize);
                AudioSourceChannelInfo startInfo;
                startInfo.buffer = &startBuffer;
                startInfo.startSample = 0;
                startInfo.numSamples = startBufferSize;
                resampleSource.getNextAudioBlock(startInfo);
                
                // Step 4: Create ultra-smooth crossfade with equal-power panning
                const int fadeStartIndex = samplesToLoopEnd - crossfadeLength;
                
                for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                    // First, copy the end buffer completely
                    bufferToFill.buffer->copyFrom(ch, bufferToFill.startSample, endBuffer, ch, 0, bufferToFill.numSamples);
                    
                    // Apply extended crossfade with equal-power curves
                    for (int i = 0; i < crossfadeLength; ++i) {
                        int outputIndex = fadeStartIndex + i;
                        if (outputIndex >= 0 && outputIndex < bufferToFill.numSamples) {
                            // Equal-power crossfade for perfect energy conservation
                            float fadeProgress = (float)i / (float)(crossfadeLength - 1);
                            
                            // Raised cosine (Hann window) for ultra-smooth transition
                            float hannProgress = 0.5f * (1.0f - std::cos(fadeProgress * M_PI));
                            
                            // Equal-power gain curves
                            float endGain = std::cos(hannProgress * M_PI * 0.5f);
                            float startGain = std::sin(hannProgress * M_PI * 0.5f);
                            
                            // Get samples with bounds checking
                            float endSample = endBuffer.getSample(ch, outputIndex);
                            float startSample = (i < startBuffer.getNumSamples()) ? startBuffer.getSample(ch, i) : 0.0f;
                            
                            // Apply equal-power crossfade
                            float crossfadedSample = endSample * endGain + startSample * startGain;
                            
                            bufferToFill.buffer->setSample(ch, bufferToFill.startSample + outputIndex, crossfadedSample);
                        }
                    }
                    
                    // Fill remainder with pure start audio for seamless continuation
                    int remainderStart = fadeStartIndex + crossfadeLength;
                    int remainderLength = bufferToFill.numSamples - remainderStart;
                    if (remainderLength > 0 && remainderStart >= 0) {
                        for (int i = 0; i < remainderLength; ++i) {
                            int outputIndex = remainderStart + i;
                            int startIndex = crossfadeLength + i;
                            if (outputIndex < bufferToFill.numSamples && startIndex < startBuffer.getNumSamples()) {
                                float startSample = startBuffer.getSample(ch, startIndex);
                                bufferToFill.buffer->setSample(ch, bufferToFill.startSample + outputIndex, startSample);
                            }
                        }
                    }
                }
                
                qDebug() << "EQUAL-POWER crossfade applied: pos" << currentPos << "-> start" << loopStartSec
                         << "crossfade:" << crossfadeLength << "samples, fadeStart:" << fadeStartIndex
                         << "remainder:" << (bufferToFill.numSamples - (fadeStartIndex + crossfadeLength));
                
                return; // Skip normal processing
            } else {
                // For very short crossfades, use extended linear fade with pre-buffering
                
                // Get a small buffer before jumping to analyze waveform continuity
                AudioBuffer<float> preJumpBuffer(bufferToFill.buffer->getNumChannels(), 32);
                AudioSourceChannelInfo preInfo;
                preInfo.buffer = &preJumpBuffer;
                preInfo.startSample = 0;
                preInfo.numSamples = 32;
                
                if (keylockEnabled) {
                    resampleSource.setResamplingRatio(1.0);
                } else {
                    resampleSource.setResamplingRatio(currentSpeed);
                }
                resampleSource.getNextAudioBlock(preInfo);
                
                // Jump to loop start
                transportSource.setPosition(loopStartSec);
                
                // Get audio from new position with extended buffer
                if (keylockEnabled) {
                    resampleSource.setResamplingRatio(1.0);
                } else {
                    resampleSource.setResamplingRatio(currentSpeed);
                }
                resampleSource.getNextAudioBlock(bufferToFill);
                
                // Apply intelligent fade-in based on waveform matching
                const int extendedFade = std::min(64, bufferToFill.numSamples / 2);
                for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                    // Get last sample from pre-jump for continuity reference
                    float lastSample = (preJumpBuffer.getNumSamples() > 0) ? 
                                      preJumpBuffer.getSample(ch, preJumpBuffer.getNumSamples() - 1) : 0.0f;
                    
                    for (int i = 0; i < extendedFade; ++i) {
                        // Smooth Hann window fade-in with DC offset compensation
                        float fadeProgress = (float)i / (float)extendedFade;
                        float hannFade = 0.5f * (1.0f - std::cos(fadeProgress * M_PI));
                        
                        float currentSample = bufferToFill.buffer->getSample(ch, bufferToFill.startSample + i);
                        
                        // Compensate for potential DC offset at loop boundary
                        if (i == 0 && std::abs(lastSample) > 0.001f) {
                            float dcOffset = lastSample * 0.1f; // Small compensation
                            currentSample += dcOffset * (1.0f - hannFade);
                        }
                        
                        float fadedSample = currentSample * hannFade;
                        bufferToFill.buffer->setSample(ch, bufferToFill.startSample + i, fadedSample);
                    }
                }
                
                qDebug() << "EXTENDED Hann fade-in applied: pos" << pos << "-> start" << loopStartSec 
                         << "fadeLength:" << extendedFade;
                return;
            }
        }
        // Fallback: if position is already past loop end, jump back with intelligent fade-in
        else if (pos >= loopEndSec && loopEndSec > loopStartSec) {
            transportSource.setPosition(loopStartSec);
            qDebug() << "Late loop jump with intelligent fade-in: pos" << pos << "-> start" << loopStartSec;
            
            // Get audio and apply sophisticated fade-in to prevent any artifacts
            if (keylockEnabled) {
                resampleSource.setResamplingRatio(1.0);
            } else {
                resampleSource.setResamplingRatio(currentSpeed);
            }
            resampleSource.getNextAudioBlock(bufferToFill);
            
            // Apply double-stage fade-in: fast initial suppression + smooth ramp
            const int totalFadeLength = std::min(128, bufferToFill.numSamples / 2);
            const int quickSuppressLength = totalFadeLength / 4; // First 25% for click suppression
            
            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                for (int i = 0; i < totalFadeLength; ++i) {
                    float sample = bufferToFill.buffer->getSample(ch, bufferToFill.startSample + i);
                    float fadedSample;
                    
                    if (i < quickSuppressLength) {
                        // Stage 1: Quick exponential suppression for click elimination
                        float quickFade = (float)i / (float)quickSuppressLength;
                        quickFade = quickFade * quickFade; // Quadratic for faster initial suppression
                        fadedSample = sample * quickFade;
                    } else {
                        // Stage 2: Smooth cosine ramp for natural sound
                        float remainingProgress = (float)(i - quickSuppressLength) / (float)(totalFadeLength - quickSuppressLength);
                        float cosineFade = 0.5f * (1.0f - std::cos(remainingProgress * M_PI));
                        fadedSample = sample * cosineFade;
                    }
                    
                    bufferToFill.buffer->setSample(ch, bufferToFill.startSample + i, fadedSample);
                }
            }
            return;
        }
    }
    
#if defined(RUBBERBAND_FOUND)
    // Use Rubber Band when available and ready (runs 24/7 once keylock was enabled once)
    if (rbReady && rb) {
        const bool isKeylockActive = keylockEnabled;
        if (debugKeylock) std::cout << "[RB] Enter path: keylock=" << isKeylockActive 
                                     << ", desiredOut=" << bufferToFill.numSamples
                                     << ", chsOut=" << bufferToFill.buffer->getNumChannels() << std::endl;
        if (lastBlockSizeHint <= 0 || currentSampleRate <= 0.0) {
            // Not ready; fallback this block
            if (debugKeylock) std::cout << "[KL][RB] Not ready: lastBlockSizeHint=" << lastBlockSizeHint
                                        << ", SR=" << currentSampleRate << ". Fallback." << std::endl;
            resampleSource.getNextAudioBlock(bufferToFill);
            return;
        }
        // Defensive: if no channels, just clear
        if (bufferToFill.buffer->getNumChannels() <= 0) {
            if (debugKeylock) std::cout << "[KL][RB] No output channels, clearing" << std::endl;
            bufferToFill.clearActiveBufferRegion();
            return;
        }
        // Priming stage: feed input and output silence until primed (only if keylock is active)
        if (keylockPrimeSamplesRemaining > 0 && isKeylockActive) {
            const int chsRB = rbNumChannels;
            const int chunk = lastBlockSizeHint > 0 ? lastBlockSizeHint : bufferToFill.numSamples;
            if (rbInputBuffer.getNumChannels() < chsRB || rbInputBuffer.getNumSamples() < chunk)
                rbInputBuffer.setSize(chsRB, chunk, false, true, true);
            AudioSourceChannelInfo tempInfo;
            tempInfo.buffer = &rbInputBuffer;
            tempInfo.startSample = 0;
            tempInfo.numSamples = chunk;
            for (int c = 0; c < chsRB; ++c) rbInputBuffer.clear(c, 0, chunk);
            resampleSource.setResamplingRatio(1.0);
            resampleSource.getNextAudioBlock(tempInfo);
            std::vector<const float*> inPtrs(chsRB);
            for (int c = 0; c < chsRB; ++c) inPtrs[c] = rbInputBuffer.getReadPointer(c);
            rb->process(inPtrs.data(), chunk, false);
            keylockPrimeSamplesRemaining -= chunk;
            if (debugKeylock) std::cout << "[RB] Priming... remaining=" << keylockPrimeSamplesRemaining << std::endl;
            bufferToFill.clearActiveBufferRegion();
            return;
        }
        
        // If keylock is off, run RB in pass-through mode (ready for instant keylock activation)
        if (!isKeylockActive) {
            if (debugKeylock) std::cout << "[RB] Pass-through mode (keylock off, staying ready)" << std::endl;
            // Set to unity ratio for pass-through
            if (std::abs(rbLastTimeRatio - 1.0) > 1e-4) {
                rb->setTimeRatio(1.0);
                rbLastTimeRatio = 1.0;
            }
            rb->setPitchScale(1.0);
            
            // Process audio through RB but at unity speed (ready for instant keylock)
            resampleSource.setResamplingRatio(currentSpeed); // Normal pitch+tempo changes
            
            // Simple pass-through with minimal processing
            const int desiredOut = bufferToFill.numSamples;
            const int chsOut = bufferToFill.buffer->getNumChannels();
            const int chsRB = rbNumChannels;
            
            // Get input directly into output buffer
            resampleSource.getNextAudioBlock(bufferToFill);
            
            // Feed the same audio through RB to keep it primed (discard RB output)
            if (rbInputBuffer.getNumChannels() < chsRB || rbInputBuffer.getNumSamples() < desiredOut)
                rbInputBuffer.setSize(chsRB, desiredOut, false, true, true);
            
            // Copy from output buffer to RB input buffer
            const int copyChs = std::min(chsOut, chsRB);
            for (int c = 0; c < copyChs; ++c) {
                rbInputBuffer.copyFrom(c, 0, *bufferToFill.buffer, c, bufferToFill.startSample, desiredOut);
            }
            
            std::vector<const float*> inPtrs(chsRB);
            for (int c = 0; c < chsRB; ++c) inPtrs[c] = rbInputBuffer.getReadPointer(c);
            rb->process(inPtrs.data(), desiredOut, false);
            
            // Discard RB output to keep it fresh
            while (rb->available() > 0) {
                int avail = std::min(rb->available(), desiredOut);
                if (rbOutScratch.getNumChannels() < chsRB || rbOutScratch.getNumSamples() < avail)
                    rbOutScratch.setSize(chsRB, avail, false, true, true);
                std::vector<float*> outPtrsRB(chsRB);
                for (int c = 0; c < chsRB; ++c) outPtrsRB[c] = rbOutScratch.getWritePointer(c);
                rb->retrieve(outPtrsRB.data(), avail);
            }
            return;
        }
        
        try {
        // Near-unity speeds: bypass stretching for transparency (keylock active mode)
        if (std::abs(currentSpeed - 1.0) <= 0.01) {
            if (debugKeylock) std::cout << "[RB] Near unity speed=" << currentSpeed << ", bypass" << std::endl;
            resampleSource.setResamplingRatio(currentSpeed); // very small change, accept slight pitch shift for quality
            resampleSource.getNextAudioBlock(bufferToFill);
            return;
        }
        // Set desired time ratio (tempo change) and keep pitch 1.0
        const double speed = std::clamp(currentSpeed, 0.05, 8.0);
        double timeRatio = 1.0 / speed; // speed up -> smaller ratio
        if (std::abs(timeRatio - rbLastTimeRatio) > 1e-4) {
            rb->setTimeRatio(timeRatio);
            rbLastTimeRatio = timeRatio;
            if (debugKeylock) std::cout << "[RB] setTimeRatio=" << timeRatio << std::endl;
        }
        rb->setPitchScale(1.0);

    // Number of output samples requested this callback
    const int desiredOut = bufferToFill.numSamples;
    const int chsOut = bufferToFill.buffer->getNumChannels();
    const int chsRB = rbNumChannels; // Always talk to RB with its configured channel count

        // Ensure we provide enough input using getSamplesRequired when possible
        resampleSource.setResamplingRatio(1.0);

    // Handle preferred start padding once after (re)initialisation
        if (!rbPaddedStartDone) {
            size_t pad = rb->getPreferredStartPad();
            if (debugKeylock) std::cout << "[KL][RB] preferredStartPad=" << pad << std::endl;
            if (pad > 0) {
                // Feed silence to prime the stretcher
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

        // Produce enough output to fully cover desiredOut, honouring initial start-delay discard
        int produced = 0;
        while (rbDiscardOutRemaining > 0 || rb->available() < (desiredOut - produced)) {
            int needIn = (int)rb->getSamplesRequired();
            if (needIn <= 0) {
                double tr = std::max(1e-6, rbLastTimeRatio);
                needIn = (int)std::ceil((desiredOut - produced) / tr);
            }
            if (needIn <= 0) break;
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

            // Drain and discard initial latency into scratch buffer
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

        // Now retrieve exactly what we need for this buffer
        const int toRetrieve = std::max(0, juce::jmin(rb->available(), desiredOut));
        if (rbOutScratch.getNumChannels() < chsRB || rbOutScratch.getNumSamples() < std::max(1, toRetrieve))
            rbOutScratch.setSize(chsRB, std::max(1, toRetrieve), false, true, true);
        std::vector<float*> outPtrsRB(chsRB);
        for (int c = 0; c < chsRB; ++c) outPtrsRB[c] = rbOutScratch.getWritePointer(c);
    const int got = (toRetrieve > 0) ? rb->retrieve(outPtrsRB.data(), toRetrieve) : 0;
    if (debugKeylock) std::cout << "[KL][RB] retrieved got=" << got << "/" << desiredOut
                     << ", availableAfter=" << rb->available() << std::endl;

        // Robustly map/mix RubberBand output into the device buffer
        if (got <= 0) {
            bufferToFill.clearActiveBufferRegion();
        } else {
            // Case 1: RB provides >= output channels -> copy matching channels
            if (chsRB >= chsOut) {
                for (int c = 0; c < chsOut; ++c) {
                    bufferToFill.buffer->copyFrom(c, bufferToFill.startSample, rbOutScratch, c, 0, got);
                }
            }
            // Case 2: RB mono -> duplicate to all output channels
            else if (chsRB == 1 && chsOut >= 1) {
                for (int c = 0; c < chsOut; ++c) {
                    bufferToFill.buffer->copyFrom(c, bufferToFill.startSample, rbOutScratch, 0, 0, got);
                }
            }
            // Case 3: RB stereo but output mono -> mix to mono and copy
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

            // If RB had fewer channels than device, clear remaining channels to silence
            for (int c = chsRB; c < chsOut; ++c) {
                bufferToFill.buffer->clear(c, bufferToFill.startSample, got);
            }
        }

        // Fill any remainder with silence for available channels
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
        } catch (const std::exception& e) {
            // If RB throws or anything goes wrong, fail safe to silence + fallback next block
            std::cout << "RubberBand processing error: " << e.what() << std::endl;
            bufferToFill.clearActiveBufferRegion();
            rbReady = false;
            return;
        } catch (...) {
            std::cout << "RubberBand processing unknown error" << std::endl;
            bufferToFill.clearActiveBufferRegion();
            rbReady = false;
            return;
        }
    } else {
        // When keylock is disabled, use normal resampling (affects pitch+tempo together)
        if (debugKeylock && keylockEnabled && std::abs(currentSpeed - 1.0) > 0.01) {
            std::cout << "[KL] RubberBand not available - keylock disabled" << std::endl;
        }
        
        // Set speed normally (pitch and tempo change together)
        resampleSource.setResamplingRatio(currentSpeed);
        resampleSource.getNextAudioBlock(bufferToFill);
        
        // Debug: Log channel info occasionally for normal playback 
        static int normalPlaybackCounter = 0;
        if (++normalPlaybackCounter % 2000 == 0) {
            std::cout << "[Normal] Playing: channels=" << bufferToFill.buffer->getNumChannels() 
                      << ", samples=" << bufferToFill.numSamples << std::endl;
        }
    }
#else
    // RubberBand is required - this should not happen with the new CMake setup
    #error "RubberBand is required for keylock functionality"
#endif

    // OPTIMIZED DSP processing with early exit for better performance
    if (!dspPrepared || bufferToFill.buffer->getNumChannels() == 0) {
        return; // Early exit if no DSP needed
    }
    
    // Check if any processing is actually needed
    const bool needsEQ = (std::abs(highGain) > 0.01f) || (std::abs(midGain) > 0.01f) || (std::abs(lowGain) > 0.01f);
    const bool needsFilter = std::abs(filterKnob) > 0.15;
    
    if (!needsEQ && !needsFilter) {
        return; // Skip all DSP if no effects are active
    }

    AudioBuffer<float>& buffer = *bufferToFill.buffer;
    const int numSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;
    
    // Use JUCE's optimized audio block system
    juce::dsp::AudioBlock<float> block(buffer);
    auto subBlock = block.getSubBlock(startSample, numSamples);
    
    // Limit to stereo for performance
    auto limitedBlock = subBlock.getSubsetChannelBlock(0, std::min(buffer.getNumChannels(), 2));
    juce::dsp::ProcessContextReplacing<float> ctx(limitedBlock);

    // PERFORMANCE: Reduced tolerance and update frequency  
    const float tolerance = 0.05f; // Larger tolerance = fewer updates
    static int updateCounter = 0;
    const bool shouldUpdate = (++updateCounter & 7) == 0; // Update every 8th buffer
    
    if (needsEQ && shouldUpdate) {
        // Update EQ coefficients only when significantly changed AND on update cycle
        if (std::abs(highGain - lastHighGain) > tolerance) {
            float gainDb = juce::jlimit(-12.0f, 12.0f, (float)(highGain * 12.0)); 
            float gainLinear = juce::Decibels::decibelsToGain(gainDb);
            highShelf.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
                currentSampleRate, 8000.0f, 0.707f, gainLinear);
            lastHighGain = highGain;
        }
        
        if (std::abs(midGain - lastMidGain) > tolerance) {
            float gainDb = juce::jlimit(-12.0f, 12.0f, (float)(midGain * 12.0));
            float gainLinear = juce::Decibels::decibelsToGain(gainDb);
            midPeak.coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
                currentSampleRate, 2500.0f, 1.0f, gainLinear);
            lastMidGain = midGain;
        }
        
        if (std::abs(lowGain - lastLowGain) > tolerance) {
            float gainDb = juce::jlimit(-12.0f, 12.0f, (float)(lowGain * 12.0));
            float gainLinear = juce::Decibels::decibelsToGain(gainDb);
            lowShelf.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowShelf(
                currentSampleRate, 300.0f, 0.707f, gainLinear);
            lastLowGain = lowGain;
        }
    }

    // Apply EQ only if needed
    if (needsEQ) {
        if (std::abs(lowGain) > 0.01f) lowShelf.process(ctx);
        if (std::abs(midGain) > 0.01f) midPeak.process(ctx);  
        if (std::abs(highGain) > 0.01f) highShelf.process(ctx);
    }

    // Optimized filter processing with reduced updates
    if (needsFilter && shouldUpdate && std::abs(filterKnob - lastFilterKnob) > tolerance) {
        const double bypassZone = 0.15; 
        double absNorm = (std::abs(filterKnob) - bypassZone) / (1.0 - bypassZone);
        
        if (filterKnob < 0.0) {
            // Smooth lowpass curve: 20kHz down to 200Hz
            double cutoffHz = 20000.0 * std::pow(0.01, absNorm);
            svf.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
            svf.setCutoffFrequency(juce::jlimit(200.0f, 20000.0f, (float)cutoffHz));
        } else {
            // Smooth highpass curve: 20Hz up to 5kHz  
            double cutoffHz = 20.0 * std::pow(250.0, absNorm);
            svf.setType(juce::dsp::StateVariableTPTFilterType::highpass);
            svf.setCutoffFrequency(juce::jlimit(20.0f, 5000.0f, (float)cutoffHz));
        }
        lastFilterKnob = filterKnob;
    }
    
    // Apply filter only when needed
    if (needsFilter) {
        svf.process(ctx);
    }
    
    // Audio level monitoring for Master Out display
    // Calculate RMS levels for both channels (thread-safe updates)
    if (buffer.getNumChannels() > 0 && numSamples > 0) {
        // Left channel (always present)
        float leftRMS = 0.0f;
        if (buffer.getNumChannels() >= 1) {
            const float* leftData = buffer.getReadPointer(0, startSample);
            float sum = 0.0f;
            for (int i = 0; i < numSamples; ++i) {
                float sample = leftData[i];
                sum += sample * sample;
            }
            leftRMS = std::sqrt(sum / numSamples);
        }
        
        // Right channel (if available, otherwise copy left)
        float rightRMS = leftRMS; // Default to left if mono
        if (buffer.getNumChannels() >= 2) {
            const float* rightData = buffer.getReadPointer(1, startSample);
            float sum = 0.0f;
            for (int i = 0; i < numSamples; ++i) {
                float sample = rightData[i];
                sum += sample * sample;
            }
            rightRMS = std::sqrt(sum / numSamples);
        }
        
        // Convert to dB scale (0-100%) with smoothing for stable display
        const float dbMin = -60.0f; // -60dB as 0%
        const float dbMax = 0.0f;   // 0dB as 100%
        
        // Convert RMS to dB
        float leftDb = leftRMS > 0.0f ? 20.0f * std::log10(leftRMS) : dbMin;
        float rightDb = rightRMS > 0.0f ? 20.0f * std::log10(rightRMS) : dbMin;
        
        // Map to 0-100% range
        float leftPercent = juce::jlimit(0.0f, 100.0f, ((leftDb - dbMin) / (dbMax - dbMin)) * 100.0f);
        float rightPercent = juce::jlimit(0.0f, 100.0f, ((rightDb - dbMin) / (dbMax - dbMin)) * 100.0f);
        
        // Smooth the levels with simple exponential moving average for stable display
        const float smoothing = 0.3f; // Adjust for responsiveness vs stability
        float currentLeft = leftChannelLevel.load();
        float currentRight = rightChannelLevel.load();
        
        float newLeft = currentLeft * (1.0f - smoothing) + leftPercent * smoothing;
        float newRight = currentRight * (1.0f - smoothing) + rightPercent * smoothing;
        
        leftChannelLevel.store(newLeft);
        rightChannelLevel.store(newRight);
    }
}

void DJAudioPlayer::releaseResources() {
    // Safe resource release with proper error handling
    try {
        // Ensure playback is stopped before releasing processing resources
        transportSource.stop();
        
        // Release processing resources safely
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
    std::cout << "DJAudioPlayer::loadFile called with: " << file.getFullPathName().toStdString() << std::endl;
    auto *reader = formatManager.createReaderFor(file);

    if (reader != nullptr) {
        std::cout << "Reader created successfully, sample rate: " << reader->sampleRate << ", length: " << reader->lengthInSamples << std::endl;
        std::unique_ptr<AudioFormatReaderSource> newSource
                (new AudioFormatReaderSource(reader, true));
        transportSource.setSource(newSource.get(),
                                  0,
                                  nullptr,
                                  reader->sampleRate);
        readerSource.reset(newSource.release());
        
        // Ensure the newly set source is prepared if our DSP was already prepared by the device
        if (dspPrepared && currentSampleRate > 0.0 && lastBlockSizeHint > 0) {
            try {
                transportSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
                resampleSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
            } catch (...) {
                // Fail-safe: ignore; device callback will prepare again on next start
            }
        }
        
#if defined(RUBBERBAND_FOUND)
        // Re-initialize RubberBand with correct channel count for the new audio file
        reinitRubberBand();
        std::cout << "RubberBand re-initialized for new audio file" << std::endl;
#endif
        
        std::cout << "Audio file loaded successfully" << std::endl;
    } else {
        std::cout << "Failed to create reader for file: " << file.getFullPathName().toStdString() << std::endl;
    }
}

// NEW: Apply a pre-loaded audio source for threaded loading
void DJAudioPlayer::applyLoadedSource(std::unique_ptr<AudioFormatReaderSource> source, double sampleRate) {
    std::cout << "DJAudioPlayer::applyLoadedSource called with sample rate: " << sampleRate << std::endl;
    
    if (source && source->getAudioFormatReader()) {
        // Stop any current playback before switching sources
        bool wasPlaying = transportSource.isPlaying();
        if (wasPlaying) {
            transportSource.stop();
        }
        
        transportSource.setSource(source.get(), 0, nullptr, sampleRate);
        readerSource = std::move(source);
        
        // If the audio device has already called prepareToPlay on us, prepare the new source now
        if (dspPrepared && currentSampleRate > 0.0 && lastBlockSizeHint > 0) {
            try {
                transportSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
                resampleSource.prepareToPlay(lastBlockSizeHint, currentSampleRate);
                std::cout << "Prepared newly loaded source for immediate playback" << std::endl;
            } catch (...) {
                // Ignore and let the audio graph recover next cycle
                std::cout << "Warning: Failed to prepare newly loaded source" << std::endl;
            }
        }
        
#if defined(RUBBERBAND_FOUND)
        // Re-initialize RubberBand with correct channel count for the new audio file
        reinitRubberBand();
        std::cout << "RubberBand re-initialized for new audio file" << std::endl;
#endif
        
        // If the player was playing before, restart it with the new source
        if (wasPlaying) {
            transportSource.start();
            std::cout << "Restarted playback with new source" << std::endl;
        }
        
        std::cout << "Pre-loaded audio source applied successfully" << std::endl;
    } else {
        std::cout << "Failed to apply pre-loaded audio source" << std::endl;
    }
}

void DJAudioPlayer::setGain(double gain) {
    if (gain < 0.0 || gain > 1.0) {
        std::cout << "DJAudioPlayer::setGain should be between 0.0 and 1.0\n";
    } else {
        transportSource.setGain(gain);
    }
}

void DJAudioPlayer::setSpeed(double ratio) {
    if (ratio < 0.0 || ratio > 100.0) {
        std::cout << "DJAudioPlayer::setSpeed should be between 0.0 and 100.0\n";
        return;
    }
    
    // Store the requested speed always
    currentSpeed = ratio;
    
    if (keylockEnabled) {
        // KEYLOCK: Keep resampler at unity to preserve pitch; RubberBand will change tempo
        resampleSource.setResamplingRatio(1.0);
        std::cout << "Keylock enabled - Tempo via RubberBand: " << ratio << "x (pitch locked)" << std::endl;
    } else {
        // Normal speed change (affects both tempo and pitch together)
        resampleSource.setResamplingRatio(ratio);
        std::cout << "Normal speed change: " << ratio << "x (tempo and pitch)" << std::endl;
    }
}

void DJAudioPlayer::setPosition(double posInSecs) {
    if (posInSecs < 0 || posInSecs > transportSource.getLengthInSeconds()) {
        std::cout << "DJAudioPlayer::setPosition should be between 0.0 and the length of the track in seconds\n";
    } else {
        // Apply quantization if enabled
        double finalPos = quantizePosition(posInSecs);
        transportSource.setPosition(finalPos);
        // If paused/softPaused, make this the new resume position so Play continues from here
        if (!transportSource.isPlaying() || softPaused.load()) {
            pausedPosSec = finalPos;
        }
    }
}

void DJAudioPlayer::setPositionRelative(double pos) {
    if (pos < 0.0 || pos > 1.0) {
        std::cout << "DJAudioPlayer::setPositionRelative should be between 0.0 and 1.0\n";
    } else {
        const double relativePos = transportSource.getLengthInSeconds() * pos;
        // Apply quantization if enabled
        double finalPos = quantizePosition(relativePos);
        setPosition(finalPos);
        // Ensure paused resume picks up here
        if (!transportSource.isPlaying() || softPaused.load()) {
            pausedPosSec = finalPos;
        }
    }
}

double DJAudioPlayer::getPositionRelative() {
    double currentPosInSecs = transportSource.getCurrentPosition();
    double lengthInSecs = transportSource.getLengthInSeconds();

    if (lengthInSecs == 0.0) {
        return 0.0;
    }

    return currentPosInSecs / lengthInSecs;
}

void DJAudioPlayer::start() {
    // lightweight start without heavy logging to avoid UI hitches
    try {
        std::cout << "=== DJAudioPlayer::start() BEGIN ===" << std::endl;
        std::cout << "  readerSource: " << (readerSource.get() ? "valid" : "null") << std::endl;
        std::cout << "  transportSource.isPlaying() BEFORE: " << transportSource.isPlaying() << std::endl;
        std::cout << "  softPaused BEFORE: " << softPaused.load() << std::endl;
        std::cout << "  dspPrepared: " << dspPrepared << std::endl;
        
        if (readerSource.get() != nullptr) {
            // Check current position and length to debug auto-stop
            double currentPos = transportSource.getCurrentPosition();
            double totalLength = transportSource.getLengthInSeconds();
            std::cout << "  Current position: " << currentPos << " / " << totalLength << " seconds" << std::endl;
            
            // If we're at the end, reset to beginning
            if (currentPos >= totalLength - 0.1) {
                std::cout << "  At end of file, resetting to start" << std::endl;
                transportSource.setPosition(0.0);
                pausedPosSec = 0.0;
            }
            
            // TEMPORARY FIX: Enable looping to prevent auto-stop at end of file
            transportSource.setLooping(true);
            std::cout << "  Enabled looping to prevent auto-stop" << std::endl;
            
            // Seek to last exact pause position if valid
            if (pausedPosSec > 0.0 && pausedPosSec <= totalLength) {
                std::cout << "  Seeking to pause position: " << pausedPosSec << std::endl;
                transportSource.setPosition(pausedPosSec);
            }
            // Clear soft pause so audio resumes immediately
            softPaused.store(false);
            forceSilent.store(false);
            pausedResetPending.store(false); // cancel pending pause resets for instant resume
            resumeCompensatePending = keylockEnabled;
            std::cout << "  Cleared pause flags" << std::endl;
            
            std::cout << "  About to call transportSource.start()..." << std::endl;
            transportSource.start();
            std::cout << "  transportSource.start() called successfully" << std::endl;
            std::cout << "  transportSource.isPlaying() AFTER: " << transportSource.isPlaying() << std::endl;
            
            // Check position again after start
            double newPos = transportSource.getCurrentPosition();
            std::cout << "  Position after start: " << newPos << " seconds" << std::endl;
        } else {
            std::cout << "  No file loaded - cannot start playback" << std::endl;
        }
        std::cout << "=== DJAudioPlayer::start() END ===" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Exception in DJAudioPlayer::start(): " << e.what() << std::endl;
    } catch (...) {
        std::cout << "Unknown exception in DJAudioPlayer::start()" << std::endl;
    }
}

void DJAudioPlayer::stop() {
    // Ultra-lightweight stop to avoid blocking the UI thread
    try {
        std::cout << "=== DJAudioPlayer::stop() BEGIN ===" << std::endl;
        std::cout << "  transportSource.isPlaying() BEFORE: " << transportSource.isPlaying() << std::endl;
        std::cout << "  softPaused BEFORE: " << softPaused.load() << std::endl;
        
        // Soft pause: keep transport running but output silence to avoid cross-deck glitch
        softPaused.store(true);
        // Save position for precise resume
        pausedPosSec = transportSource.getCurrentPosition();
        std::cout << "  Saved pause position: " << pausedPosSec << std::endl;
        // Prepare one-time resets to avoid artifacts on resume
        pausedResetPending.store(true);
        
        std::cout << "  softPaused AFTER: " << softPaused.load() << std::endl;
        std::cout << "=== DJAudioPlayer::stop() END ===" << std::endl;
    } catch (...) {
        std::cout << "Exception in DJAudioPlayer::stop()" << std::endl;
        // Swallow exceptions to avoid UI hitching
    }
}

#if defined(RUBBERBAND_FOUND)
void DJAudioPlayer::reinitRubberBand() {
    // Validate environment before creating RB stretcher
    if (currentSampleRate <= 0.0) {
        std::cout << "RubberBand skipped: invalid sample rate" << std::endl;
        rb.reset();
        rbReady = false;
        return;
    }
    
    // Determine channel count from the loaded audio source
    int sourceChannels = 1; // Default to mono
    if (readerSource && readerSource->getAudioFormatReader()) {
        sourceChannels = readerSource->getAudioFormatReader()->numChannels;
    }
    rbNumChannels = std::min(sourceChannels, 2); // Max 2 channels for performance
    
    std::cout << "RubberBand init: sourceChannels=" << sourceChannels << ", rbChannels=" << rbNumChannels << std::endl;
    
    RubberBand::RubberBandStretcher::Options opts =
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionThreadingAuto;

    // Base config by profile
    switch (rbQuality) {
        case KeylockQuality::Fast:
            opts |= RubberBand::RubberBandStretcher::OptionEngineFaster |
                    RubberBand::RubberBandStretcher::OptionTransientsCrisp |
                    RubberBand::RubberBandStretcher::OptionWindowShort |
                    RubberBand::RubberBandStretcher::OptionPitchHighSpeed |
                    RubberBand::RubberBandStretcher::OptionChannelsTogether;
            break;
        case KeylockQuality::Balanced:
            opts |= RubberBand::RubberBandStretcher::OptionEngineFiner |
                    RubberBand::RubberBandStretcher::OptionTransientsMixed |
                    RubberBand::RubberBandStretcher::OptionWindowStandard |
                    RubberBand::RubberBandStretcher::OptionPitchHighSpeed |
                    RubberBand::RubberBandStretcher::OptionChannelsTogether;
            break;
        case KeylockQuality::Quality:
            opts |= RubberBand::RubberBandStretcher::OptionEngineFiner |
                    RubberBand::RubberBandStretcher::OptionTransientsSmooth |
                    RubberBand::RubberBandStretcher::OptionWindowStandard |
                    RubberBand::RubberBandStretcher::OptionPitchHighQuality |
                    RubberBand::RubberBandStretcher::OptionChannelsTogether;
            break;
    }
    try {
        rb = std::make_unique<RubberBand::RubberBandStretcher>(currentSampleRate, rbNumChannels, opts);
        rb->setTimeRatio(1.0);
        rb->setPitchScale(1.0);
        rb->setMaxProcessSize((size_t) std::max(128, lastBlockSizeHint));
        rbLastTimeRatio = 1.0;
        rbInputBuffer.setSize(rbNumChannels, std::max(256, lastBlockSizeHint));
        rbInputBuffer.clear();
        rbReady = true;
        rbPaddedStartDone = false;
        rbLatencySamples = (int)rb->getStartDelay();
        rbLatencySeconds = rbLatencySamples / currentSampleRate;
        rbDiscardOutRemaining = 0;
        rbOutScratch.setSize(rbNumChannels, std::max(256, lastBlockSizeHint));
        rbOutScratch.clear();
        std::cout << "Rubber Band init: quality=" << (int)rbQuality << ", engine=" << rb->getEngineVersion() << ", SR=" << currentSampleRate << std::endl;
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
    // SIMPLIFIED: Direct transport check without soft pause complications
    bool transport_playing = transportSource.isPlaying();
    
    // Log only when state changes to avoid spam
    static bool lastResult = false;
    if (transport_playing != lastResult) {
        std::cout << "*** isPlaying() state change: transport=" << transport_playing << std::endl;
        lastResult = transport_playing;
    }
    
    return transport_playing; // Ignore softPaused for now to isolate the issue
}

// Simple EQ/filter stubs (store values, no DSP applied yet) - optimized for real-time performance
void DJAudioPlayer::setHighGain(double v) {
    highGain = std::clamp(v, -1.0, 1.0);
    // Coefficient update will happen in getNextAudioBlock for thread safety
}

void DJAudioPlayer::setMidGain(double v) {
    midGain = std::clamp(v, -1.0, 1.0);
    // Coefficient update will happen in getNextAudioBlock for thread safety
}

void DJAudioPlayer::setLowGain(double v) {
    lowGain = std::clamp(v, -1.0, 1.0);
    // Coefficient update will happen in getNextAudioBlock for thread safety
}

void DJAudioPlayer::setFilterCutoff(double v) {
    filterKnob = std::clamp(v, -1.0, 1.0);
    // Filter update will happen in getNextAudioBlock for thread safety
}

void DJAudioPlayer::enableLoop(double startSec, double lengthSec) {
    if (lengthSec <= 0.0) { disableLoop(); return; }
    double len = transportSource.getLengthInSeconds();
    loopStartSec = std::max(0.0, std::min(startSec, len));
    loopEndSec = std::max(loopStartSec, std::min(loopStartSec + lengthSec, len));
    loopEnabled = (loopEndSec > loopStartSec);
    
    // DEBUG: Log actual loop parameters
    qDebug() << "DJAudioPlayer::enableLoop - StartSec:" << startSec 
             << "LengthSec:" << lengthSec
             << "ActualStart:" << loopStartSec
             << "ActualEnd:" << loopEndSec
             << "ActualLength:" << (loopEndSec - loopStartSec)
             << "Enabled:" << loopEnabled;
}

void DJAudioPlayer::disableLoop() {
    loopEnabled = false;
    loopStartSec = 0.0;
    loopEndSec = 0.0;
}

void DJAudioPlayer::setScratchVelocity(double velocity) {
    // Store velocity for potential future use (e.g., inertia, vinyl emu)
    // Actual scratch audio is driven by setPositionRelative() from the UI.
    scratchVelocity = velocity;
}

void DJAudioPlayer::enableScratch(bool enable) {
    // Toggle scratch mode. During scratching, UI drives position updates and we keep audio flowing.
    scratchMode = enable;
    // Ensure we don't emit stale buffered audio right after toggling
    pausedResetPending.store(true);
    // Never hard-mute here; scratching should remain audible if transport is running
}

void DJAudioPlayer::setKeylockEnabled(bool enabled) {
    // Defer to audio thread to avoid races with getNextAudioBlock
    keylockChangePending.store(enabled ? 1 : 0);
}

void DJAudioPlayer::setQuantizeEnabled(bool enabled) {
    quantizeEnabled = enabled;
    std::cout << "Quantize " << (enabled ? "enabled" : "disabled") << std::endl;
}

void DJAudioPlayer::setBeatInfo(double bpm, double firstBeatOffset, double trackLength) {
    trackBpm = bpm;
    trackFirstBeatOffset = firstBeatOffset;
    trackLengthSec = trackLength;
}

double DJAudioPlayer::quantizePosition(double positionSec) const {
    if (!quantizeEnabled || trackBpm <= 0.0) {
        return positionSec; // No quantization if disabled or no BPM info
    }
    
    // Calculate beat length in seconds
    double beatLengthSec = 60.0 / trackBpm;
    
    // Calculate position relative to first beat
    double relativePos = positionSec - trackFirstBeatOffset;
    
    // Find nearest beat position
    double beatNumber = std::round(relativePos / beatLengthSec);
    
    // Calculate quantized position
    double quantizedPos = trackFirstBeatOffset + (beatNumber * beatLengthSec);
    
    // Ensure position is within track bounds
    return std::clamp(quantizedPos, 0.0, trackLengthSec);
}