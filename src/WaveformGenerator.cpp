#include "WaveformGenerator.h"

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
    if (binCount <= 0) return false;
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (!reader) return false;

    const int64 totalSamples = reader->lengthInSamples;
    if (totalSamples <= 0) return false;
    out.totalSamples = totalSamples;
    out.sampleRate = (int)reader->sampleRate;
    out.lengthSeconds = (double)totalSamples / reader->sampleRate;

    // Find audible start by scanning RMS with finer granularity and a small pre-roll
    int64 audioStartSample = 0;
    const int chunkSize = 1024; // finer resolution (~23ms @44.1k)
    int consecutive = 0;
    juce::AudioBuffer<float> searchBuf((int)reader->numChannels, chunkSize);
    for (int64 pos = 0; pos < totalSamples; pos += chunkSize) {
        const int toRead = (int)std::min<int64>(chunkSize, totalSamples - pos);
        reader->read(&searchBuf, 0, toRead, pos, true, true);
        double sum = 0.0;
        int n = 0;
        for (int ch = 0; ch < searchBuf.getNumChannels(); ++ch) {
            const float* d = searchBuf.getReadPointer(ch);
            for (int i = 0; i < toRead; ++i) { sum += (double)d[i] * d[i]; ++n; }
        }
        const float rms = n > 0 ? std::sqrt(sum / (double)n) : 0.0f;
    if (rms > silenceThreshold) {
            if (++consecutive >= consecutiveChunksNeeded) {
        // Back up to the first above-threshold chunk start
        int64 candidate = std::max<int64>(0, pos - (consecutiveChunksNeeded - 1) * chunkSize);
        // Apply a small pre-roll so we never cut off a transient
        const int preRoll = (int)std::round(0.02 * reader->sampleRate); // 20 ms
        audioStartSample = std::max<int64>(0, candidate - preRoll);
                break;
            }
        } else {
            consecutive = 0;
        }
    }
    out.audioStartOffsetSec = (double)audioStartSample / reader->sampleRate;

    // Stream into bins - REAL WAVEFORM like Serato/Rekordbox (not RMS/Peak!)
    out.minBins.assign(binCount, 0.0f);
    out.maxBins.assign(binCount, 0.0f);
    
    // Use smaller chunks for better waveform precision
    const int streamChunk = 4096;
    juce::AudioBuffer<float> buf((int)reader->numChannels, streamChunk);
    const int64 samplesFromStart = totalSamples - audioStartSample;
    int64 processed = 0;
    
    // Pre-calculate for performance
    const double binToSampleRatio = (double)samplesFromStart / (double)binCount;
    
    while (processed < samplesFromStart) {
        const int toRead = (int)std::min<int64>(streamChunk, samplesFromStart - processed);
        reader->read(&buf, 0, toRead, audioStartSample + processed, true, true);
        
        for (int i = 0; i < toRead; ++i) {
            // Mix channels for mono signal
            float sample = 0.0f;
            const int numCh = buf.getNumChannels();
            for (int ch = 0; ch < numCh; ++ch) {
                sample += buf.getReadPointer(ch)[i];
            }
            sample /= (float)numCh; // Average channels
            
            const int64 globalIdx = processed + i;
            const int bin = (int)(globalIdx / binToSampleRatio);
            
            if ((unsigned)bin < (unsigned)binCount) {
                // REAL WAVEFORM: Store actual min/max values (not envelope!)
                if (sample < out.minBins[bin]) out.minBins[bin] = sample;
                if (sample > out.maxBins[bin]) out.maxBins[bin] = sample;
            }
        }
        processed += toRead;
    }
    
    // NO post-processing! Keep the real waveform min/max values
    // This is how Serato/Rekordbox actually work - they show the real audio waveform
    return true;
}
