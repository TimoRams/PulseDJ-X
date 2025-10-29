#include "WaveformGenerator.h"
#include "AudioFormatGuard.h"
#include <algorithm>
#include <cmath>
#include <vector>

WaveformGenerator::WaveformGenerator()
{
    formatManager.registerBasicFormats(); // JUCE's basic formats include MP3 with JUCE_USE_MP3AUDIOFORMAT=1
}

bool WaveformGenerator::generate(const juce::File& file,
                                 int binCount,
                                 Result& out,
                                 float silenceThreshold,
                                 int consecutiveChunksNeeded)
{
    if (binCount <= 0) [[unlikely]] return false;

    std::vector<float> collectedMax, collectedMin;
    const int reserveSize = std::max(binCount, 1);
    collectedMax.reserve(reserveSize);
    collectedMin.reserve(reserveSize);

    StreamingCallbacks callbacks;
    callbacks.onBegin = [&](int totalBins, double audioStartOffsetSec, double lengthSeconds, int sampleRate, int64 totalSamples) {
        collectedMax.assign(totalBins, 0.0f);
        collectedMin.assign(totalBins, 0.0f);
        out.audioStartOffsetSec = audioStartOffsetSec;
        out.lengthSeconds = lengthSeconds;
        out.sampleRate = sampleRate;
        out.totalSamples = totalSamples;
    };

    callbacks.onChunk = [&](int startBin, const std::vector<float>& maxBins, const std::vector<float>& minBins, bool) {
        if (startBin < 0 || startBin + static_cast<int>(maxBins.size()) > static_cast<int>(collectedMax.size())) [[unlikely]] return;
        std::copy(maxBins.begin(), maxBins.end(), collectedMax.begin() + startBin);
        std::copy(minBins.begin(), minBins.end(), collectedMin.begin() + startBin);
    };

    callbacks.onProgress = nullptr;

    if (!generateStreaming(file, binCount, callbacks, std::max(binCount, 1), silenceThreshold, consecutiveChunksNeeded)) [[unlikely]] {
        return false;
    }

    out.maxBins = std::move(collectedMax);
    out.minBins = std::move(collectedMin);
    return true;
}

bool WaveformGenerator::generateStreaming(const juce::File& file,
                                          int binCount,
                                          const StreamingCallbacks& callbacks,
                                          int chunkBinSize,
                                          float silenceThreshold,
                                          int consecutiveChunksNeeded)
{
    if (binCount <= 0 || !callbacks.onBegin || !callbacks.onChunk) [[unlikely]] return false;

    AnalysisMetadata metadata;
    if (!analyzeFile(file, metadata, silenceThreshold, consecutiveChunksNeeded)) [[unlikely]] return false;

    callbacks.onBegin(binCount, metadata.audioStartOffsetSec, metadata.lengthSeconds, metadata.sampleRate, metadata.totalSamples);

    if (callbacks.onProgress) [[unlikely]] callbacks.onProgress(0.0);

    std::unique_ptr<juce::AudioFormatReader> reader;
    {
        AudioFormatManagerGuard formatGuard;
        reader.reset(formatManager.createReaderFor(file));
    }

    if (!reader) [[unlikely]] return false;

    const int64 samplesFromStart = std::max<int64>(0, metadata.totalSamples - metadata.audioStartSample);
    if (samplesFromStart <= 0) [[unlikely]] return true;

    const double samplesPerBin = static_cast<double>(samplesFromStart) / binCount;
    if (samplesPerBin <= 0.0) [[unlikely]] return false;

    chunkBinSize = std::max(1, chunkBinSize);

    std::vector<float> minBins(binCount, 0.0f), maxBins(binCount, 0.0f);
    std::vector<bool> touched(binCount, false);

    constexpr int streamChunk = 4096;
    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), streamChunk);
    int64 processedSamples = 0;
    int flushCursor = 0;

    auto flushBins = [&](int upToBin, bool finalChunk) {
        upToBin = std::clamp(upToBin, 0, binCount);
        if (upToBin <= flushCursor) [[unlikely]] return;

        int remaining = upToBin - flushCursor;
        int emitStart = flushCursor;

        while (remaining > 0) [[likely]] {
            const int emitCount = std::min(chunkBinSize, remaining);
            const bool isFinal = finalChunk && (emitStart + emitCount >= binCount);
            callbacks.onChunk(emitStart, 
                            std::vector<float>(maxBins.begin() + emitStart, maxBins.begin() + emitStart + emitCount),
                            std::vector<float>(minBins.begin() + emitStart, minBins.begin() + emitStart + emitCount),
                            isFinal);

            emitStart += emitCount;
            remaining -= emitCount;

            if (callbacks.onProgress) [[unlikely]] {
                callbacks.onProgress(std::clamp(static_cast<double>(emitStart) / std::max(1, binCount), 0.0, 1.0));
            }
        }

        flushCursor = upToBin;
    };

    while (processedSamples < samplesFromStart) [[likely]] {
        const int toRead = static_cast<int>(std::min<int64>(streamChunk, samplesFromStart - processedSamples));
        reader->read(&buffer, 0, toRead, metadata.audioStartSample + processedSamples, true, true);

        const int numChannels = buffer.getNumChannels();
        const float invChannels = 1.0f / std::max(1, numChannels);

        for (int i = 0; i < toRead; ++i) [[likely]] {
            float sample = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch) {
                sample += buffer.getReadPointer(ch)[i];
            }
            sample *= invChannels;

            const int bin = std::clamp(static_cast<int>(std::floor((processedSamples + i) / samplesPerBin)), 0, binCount - 1);

            if (!touched[bin]) [[unlikely]] {
                minBins[bin] = maxBins[bin] = sample;
                touched[bin] = true;
            } else {
                minBins[bin] = std::min(minBins[bin], sample);
                maxBins[bin] = std::max(maxBins[bin], sample);
            }

            if (bin >= flushCursor + chunkBinSize) [[unlikely]] {
                flushBins(bin, false);
            }
        }

        processedSamples += toRead;
    }

    flushBins(binCount, true);

    if (callbacks.onProgress) [[unlikely]] callbacks.onProgress(1.0);

    return true;
}

bool WaveformGenerator::analyzeFile(const juce::File& file,
                                    AnalysisMetadata& metadata,
                                    float silenceThreshold,
                                    int consecutiveChunksNeeded)
{
    std::unique_ptr<juce::AudioFormatReader> reader;
    {
        AudioFormatManagerGuard formatGuard;
        reader.reset(formatManager.createReaderFor(file));
    }

    if (!reader || reader->lengthInSamples <= 0) [[unlikely]] return false;

    metadata.totalSamples = reader->lengthInSamples;
    metadata.sampleRate = static_cast<int>(reader->sampleRate);
    metadata.lengthSeconds = static_cast<double>(metadata.totalSamples) / reader->sampleRate;
    metadata.audioStartSample = 0;

    constexpr int rmsChunk = 1024;
    juce::AudioBuffer<float> rmsBuffer(static_cast<int>(reader->numChannels), rmsChunk);
    int consecutive = 0;

    for (int64 pos = 0; pos < metadata.totalSamples; pos += rmsChunk) [[likely]] {
        const int toRead = static_cast<int>(std::min<int64>(rmsChunk, metadata.totalSamples - pos));
        reader->read(&rmsBuffer, 0, toRead, pos, true, true);

        double sum = 0.0;
        int n = 0;
        for (int ch = 0; ch < rmsBuffer.getNumChannels(); ++ch) {
            const float* data = rmsBuffer.getReadPointer(ch);
            for (int i = 0; i < toRead; ++i) {
                const double val = static_cast<double>(data[i]);
                sum += val * val;
                ++n;
            }
        }

        const float rms = (n > 0) ? std::sqrt(sum / n) : 0.0f;
        if (rms > silenceThreshold) [[likely]] {
            if (++consecutive >= consecutiveChunksNeeded) [[unlikely]] {
                const int64 candidate = std::max<int64>(0, pos - (consecutiveChunksNeeded - 1) * rmsChunk);
                const int preRoll = static_cast<int>(std::round(0.02 * reader->sampleRate));
                metadata.audioStartSample = std::max<int64>(0, candidate - preRoll);
                break;
            }
        } else {
            consecutive = 0;
        }
    }

    metadata.audioStartOffsetSec = static_cast<double>(metadata.audioStartSample) / reader->sampleRate;
    return true;
}

bool WaveformGenerator::renderBinWindow(const juce::File& file,
                                        const AnalysisMetadata& metadata,
                                        int totalBins,
                                        int startBin,
                                        int binCount,
                                        std::vector<float>& outMax,
                                        std::vector<float>& outMin)
{
    if (totalBins <= 0 || binCount <= 0 || startBin < 0 || startBin >= totalBins) [[unlikely]] return false;

    const int safeCount = std::min(binCount, totalBins - startBin);
    if (safeCount <= 0) [[unlikely]] return false;

    std::unique_ptr<juce::AudioFormatReader> reader;
    {
        AudioFormatManagerGuard formatGuard;
        reader.reset(formatManager.createReaderFor(file));
    }

    if (!reader || reader->lengthInSamples <= metadata.audioStartSample) [[unlikely]] return false;

    const int64 samplesFromStart = std::max<int64>(0, metadata.totalSamples - metadata.audioStartSample);
    if (samplesFromStart <= 0) [[unlikely]] return false;

    const double samplesPerBin = static_cast<double>(samplesFromStart) / totalBins;
    if (samplesPerBin <= 0.0) [[unlikely]] return false;

    const double startOffsetSamples = static_cast<double>(startBin) * samplesPerBin;
    const double endOffsetSamples = static_cast<double>(startBin + safeCount) * samplesPerBin;

    const int64 sampleStart = std::clamp<int64>(metadata.audioStartSample + static_cast<int64>(std::floor(startOffsetSamples)), metadata.audioStartSample, metadata.totalSamples);
    const int64 sampleEnd = std::clamp<int64>(metadata.audioStartSample + static_cast<int64>(std::ceil(endOffsetSamples)), sampleStart, metadata.totalSamples);

    const int64 samplesToRead = sampleEnd - sampleStart;
    if (samplesToRead <= 0) [[unlikely]] {
        outMax.assign(safeCount, 0.0f);
        outMin.assign(safeCount, 0.0f);
        return true;
    }

    outMax.assign(safeCount, 0.0f);
    outMin.assign(safeCount, 0.0f);
    std::vector<bool> touched(safeCount, false);

    constexpr int streamChunk = 4096;
    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), streamChunk);

    int64 processed = 0;
    while (processed < samplesToRead) [[likely]] {
        const int toRead = static_cast<int>(std::min<int64>(streamChunk, samplesToRead - processed));
        reader->read(&buffer, 0, toRead, sampleStart + processed, true, true);

        const int numChannels = buffer.getNumChannels();
        const float invChannels = 1.0f / std::max(1, numChannels);

        for (int i = 0; i < toRead; ++i) [[likely]] {
            float sample = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch) {
                sample += buffer.getReadPointer(ch)[i];
            }
            sample *= invChannels;

            const double sampleIndexFromStart = static_cast<double>(sampleStart - metadata.audioStartSample) + processed + i;
            const int bin = static_cast<int>(std::floor(sampleIndexFromStart / samplesPerBin));
            if (bin < startBin) [[unlikely]] continue;
            const int localIndex = bin - startBin;
            if (localIndex < 0 || localIndex >= safeCount) [[unlikely]] continue;

            if (!touched[localIndex]) [[unlikely]] {
                outMin[localIndex] = outMax[localIndex] = sample;
                touched[localIndex] = true;
            } else {
                outMin[localIndex] = std::min(outMin[localIndex], sample);
                outMax[localIndex] = std::max(outMax[localIndex], sample);
            }
        }

        processed += toRead;
    }

    for (int i = 0; i < safeCount; ++i) {
        if (!touched[i]) [[unlikely]] {
            outMin[i] = outMax[i] = 0.0f;
        }
    }

    return true;
}

bool WaveformGenerator::renderBinWindow(const juce::File& file,
                                        const AnalysisMetadata& metadata,
                                        int totalBins,
                                        int startBin,
                                        int binCount,
                                        std::vector<float>& outMax,
                                        std::vector<float>& outMin,
                                        std::vector<float>& outLow,
                                        std::vector<float>& outMid,
                                        std::vector<float>& outHigh)
{
    // Compute min/max as usual and also rough 3-band RMS energies using dual lowpass filters
    if (totalBins <= 0 || binCount <= 0 || startBin < 0 || startBin >= totalBins) [[unlikely]] return false;

    const int safeCount = std::min(binCount, totalBins - startBin);
    if (safeCount <= 0) [[unlikely]] return false;

    std::unique_ptr<juce::AudioFormatReader> reader;
    {
        AudioFormatManagerGuard formatGuard;
        reader.reset(formatManager.createReaderFor(file));
    }

    if (!reader || reader->lengthInSamples <= metadata.audioStartSample) [[unlikely]] return false;

    const int64 samplesFromStart = std::max<int64>(0, metadata.totalSamples - metadata.audioStartSample);
    if (samplesFromStart <= 0) [[unlikely]] return false;

    const double samplesPerBin = static_cast<double>(samplesFromStart) / totalBins;
    if (samplesPerBin <= 0.0) [[unlikely]] return false;

    const double startOffsetSamples = static_cast<double>(startBin) * samplesPerBin;
    const double endOffsetSamples = static_cast<double>(startBin + safeCount) * samplesPerBin;

    const int64 sampleStart = std::clamp<int64>(metadata.audioStartSample + static_cast<int64>(std::floor(startOffsetSamples)), metadata.audioStartSample, metadata.totalSamples);
    const int64 sampleEnd = std::clamp<int64>(metadata.audioStartSample + static_cast<int64>(std::ceil(endOffsetSamples)), sampleStart, metadata.totalSamples);

    const int64 samplesToRead = sampleEnd - sampleStart;
    if (samplesToRead <= 0) [[unlikely]] {
        outMax.assign(safeCount, 0.0f);
        outMin.assign(safeCount, 0.0f);
        outLow.assign(safeCount, 0.0f);
        outMid.assign(safeCount, 0.0f);
        outHigh.assign(safeCount, 0.0f);
        return true;
    }

    outMax.assign(safeCount, 0.0f);
    outMin.assign(safeCount, 0.0f);
    outLow.assign(safeCount, 0.0f);
    outMid.assign(safeCount, 0.0f);
    outHigh.assign(safeCount, 0.0f);
    std::vector<bool> touched(safeCount, false);
    std::vector<int> counts(safeCount, 0);

    constexpr int streamChunk = 4096;
    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), streamChunk);

    // Simple dual lowpass approach to split bands:
    // low = LP(sample, 200 Hz)
    // mid = LP(sample, 2000 Hz) - LP(sample, 200 Hz)
    // high = sample - LP(sample, 2000 Hz)
    const double fs = std::max(1000.0, static_cast<double>(reader->sampleRate));
    auto lpAlpha = [&](double fc) {
        const double omega = 2.0 * M_PI * std::max(1.0, fc);
        return static_cast<float>(omega / (fs + omega));
    };
    const float a200 = lpAlpha(200.0);
    const float a2000 = lpAlpha(2000.0);
    float lp200_state = 0.0f;
    float lp2000_state = 0.0f;

    int64 processed = 0;
    while (processed < samplesToRead) [[likely]] {
        const int toRead = static_cast<int>(std::min<int64>(streamChunk, samplesToRead - processed));
        reader->read(&buffer, 0, toRead, sampleStart + processed, true, true);

        const int numChannels = buffer.getNumChannels();
        const float invChannels = 1.0f / std::max(1, numChannels);

        for (int i = 0; i < toRead; ++i) [[likely]] {
            float sample = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch) {
                sample += buffer.getReadPointer(ch)[i];
            }
            sample *= invChannels;

            // Bin index mapping
            const double sampleIndexFromStart = static_cast<double>(sampleStart - metadata.audioStartSample) + processed + i;
            const int bin = static_cast<int>(std::floor(sampleIndexFromStart / samplesPerBin));
            if (bin < startBin) [[unlikely]] continue;
            const int localIndex = bin - startBin;
            if (localIndex < 0 || localIndex >= safeCount) [[unlikely]] continue;

            // Min/Max
            if (!touched[localIndex]) [[unlikely]] {
                outMin[localIndex] = outMax[localIndex] = sample;
                touched[localIndex] = true;
            } else {
                outMin[localIndex] = std::min(outMin[localIndex], sample);
                outMax[localIndex] = std::max(outMax[localIndex], sample);
            }

            // Lowpass filters (one-pole)
            lp200_state  += a200  * (sample - lp200_state);
            lp2000_state += a2000 * (sample - lp2000_state);

            const float low  = lp200_state;
            const float mid  = lp2000_state - lp200_state;
            const float high = sample - lp2000_state;

            outLow[localIndex]  += low  * low;
            outMid[localIndex]  += mid  * mid;
            outHigh[localIndex] += high * high;
            counts[localIndex]++;
        }

        processed += toRead;
    }

    for (int i = 0; i < safeCount; ++i) {
        if (!touched[i]) [[unlikely]] {
            outMin[i] = outMax[i] = 0.0f;
        }
        const int n = std::max(1, counts[i]);
        outLow[i]  = std::sqrt(outLow[i]  / n);
        outMid[i]  = std::sqrt(outMid[i]  / n);
        outHigh[i] = std::sqrt(outHigh[i] / n);
    }

    return true;
}
