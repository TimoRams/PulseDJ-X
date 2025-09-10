// Copied from project root
#ifndef GUI_APP_EXAMPLE_DJAUDIOPLAYER_H
#define GUI_APP_EXAMPLE_DJAUDIOPLAYER_H

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <iostream>
#include <array>
#include <atomic>
#include <chrono>
using namespace juce;
#include <queue>
#if defined(RUBBERBAND_FOUND)
#include <rubberband/RubberBandStretcher.h>
#endif

/**
 * A class to handle the audio functionality of a DJ deck. Works in tandem with the DeckGUI to represent a DJ deck
 * in the application
 */
class DJAudioPlayer : public AudioSource {
public:
    explicit DJAudioPlayer(AudioFormatManager &_formatManager);
    ~DJAudioPlayer() override;

    void loadFile(const File &file);
    // NEW: Apply a pre-loaded audio source (for threaded loading)
    void applyLoadedSource(std::unique_ptr<AudioFormatReaderSource> source, double sampleRate);
    void setGain(double gain);
    void setSpeed(double ratio);
    void setPositionRelative(double pos);
    double getPositionRelative();
    void start();
    void stop();
    // Pause is an alias to stop playback without unloading the track
    void pause() { stop(); }
    bool isPlaying();
    // Transport helpers
    double getCurrentPositionSeconds() const { return transportSource.getCurrentPosition(); }
    double getLengthInSeconds() const { return transportSource.getLengthInSeconds(); }
    void setPositionSeconds(double secs) { setPosition(secs); }
    // Total processing latency added by the DSP pipeline (e.g., Rubber Band), in seconds
    double getPipelineLatencySeconds() const {
#if defined(RUBBERBAND_FOUND)
        if (keylockEnabled && rbReady) return rbLatencySeconds;
#endif
        return 0.0;
    }
    
    // Scratch control - sets playback speed based on scratch velocity
    void setScratchVelocity(double velocity);
    void enableScratch(bool enable);
    bool isScratchMode() const { return scratchMode; }

    // Simple loop control (seconds)
    void enableLoop(double startSec, double lengthSec);
    void disableLoop();
    bool isLoopEnabled() const { return loopEnabled; }
    double getLoopStart() const { return loopStartSec; }
    double getLoopEnd() const { return loopEndSec; }
    
    // Simple EQ/filter control stubs (values: -1.0 .. +1.0)
    void setHighGain(double v);
    void setMidGain(double v);
    void setLowGain(double v);
    void setFilterCutoff(double v);
    
    // Keylock (pitch lock) - maintains original pitch when speed changes
    void setKeylockEnabled(bool enabled);
    bool isKeylockEnabled() const { return keylockEnabled; }
    // Runtime keylock quality profile
    enum class KeylockQuality { Fast, Balanced, Quality };
    void setKeylockQuality(KeylockQuality q);
    KeylockQuality getKeylockQuality() const { return rbQuality; }
    
    // Quantize control - snaps cues and loops to nearest beat
    void setQuantizeEnabled(bool enabled);
    bool isQuantizeEnabled() const { return quantizeEnabled; }
    void setBeatInfo(double bpm, double firstBeatOffset, double trackLength);
    double quantizePosition(double positionSec) const;
    // Beat info getters
    double getTrackBpm() const { return trackBpm; }
    double getFirstBeatOffset() const { return trackFirstBeatOffset; }
    double getTrackLengthSeconds() const { return trackLengthSec; }
    
    // Audio level monitoring for Master Out display
    float getLeftChannelLevel() const { return leftChannelLevel.load(); }
    float getRightChannelLevel() const { return rightChannelLevel.load(); }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const AudioSourceChannelInfo &bufferToFill) override;
    void releaseResources() override;

private:
    void setPosition(double posInSecs);
    
#if defined(RUBBERBAND_FOUND)
    // Recreate/configure Rubber Band according to the selected quality profile
    void reinitRubberBand();
#endif

    AudioFormatManager &formatManager;
    AudioTransportSource transportSource;
    std::unique_ptr<AudioFormatReaderSource> readerSource;
    ResamplingAudioSource resampleSource{&transportSource, false, 2};
    // Store EQ/filter values
    double highGain{0.0};
    double midGain{0.0};
    double lowGain{0.0};
    // filter knob: -1..0..+1; negative -> lowpass, positive -> highpass
    double filterKnob{0.0};

    // DSP objects for a simple 3-band EQ + filter (optimized for real-time performance)
    juce::dsp::IIR::Filter<float> lowShelf;
    juce::dsp::IIR::Filter<float> midPeak;
    juce::dsp::IIR::Filter<float> highShelf;
    juce::dsp::StateVariableTPTFilter<float> svf; // used for filter knob (LP/HP)
    
    // Performance optimization: Cache coefficient objects to avoid recreation
    juce::dsp::IIR::Coefficients<float>::Ptr cachedLowCoeffs;
    juce::dsp::IIR::Coefficients<float>::Ptr cachedMidCoeffs;
    juce::dsp::IIR::Coefficients<float>::Ptr cachedHighCoeffs;
    double lastLowGain{0.0}, lastMidGain{0.0}, lastHighGain{0.0}, lastFilterKnob{0.0};
    
    // PROFESSIONAL KEYLOCK: High-quality pitch shifting using JUCE DSP
    std::unique_ptr<juce::dsp::ProcessorChain<
        juce::dsp::Gain<float>,
        juce::dsp::Reverb
    >> pitchShiftChain;
    
    // Time-domain pitch shifter with better quality
    std::unique_ptr<juce::dsp::ProcessorChain<juce::dsp::Gain<float>>> timeStretchProcessor;
    
    double currentSpeed{1.0};
    double pitchShiftRatio{1.0};

#if defined(RUBBERBAND_FOUND)
    // High-quality Rubber Band time-stretcher (required for keylock functionality)
    std::unique_ptr<RubberBand::RubberBandStretcher> rb;
    juce::AudioBuffer<float> rbInputBuffer;
    juce::AudioBuffer<float> rbOutScratch;
    double rbLastTimeRatio{1.0};
    int rbNumChannels{2};
    bool rbReady{false};
    int rbLatencySamples{0};
    double rbLatencySeconds{0.0};
    bool rbPaddedStartDone{false};
    int rbDiscardOutRemaining{0};
    KeylockQuality rbQuality{KeylockQuality::Quality};
#endif

    // Hard mute flag to kill output immediately on stop
    std::atomic<bool> forceSilent{false};
    // Soft pause flag: mute output without stopping transport to avoid glitches
    std::atomic<bool> softPaused{false};
    // Request to capture exact position on the audio thread
    std::atomic<bool> savePosRequested{false};
    // Request transportSource.stop() to be executed on the audio thread
    std::atomic<bool> stopRequested{false};
    // Perform one-time heavy resets after pause/stop inside audio thread without repeating every callback
    std::atomic<bool> pausedResetPending{false};

    // DSP prepare state
    double currentSampleRate{44100.0};
    bool dspPrepared{false};
    // Precise pause/resume handling
    double pausedPosSec{0.0};
    bool resumeCompensatePending{false};
    int resumeWarmupSamplesRemaining{0};
    int lastBlockSizeHint{512};
    
    // Performance optimizations for multiple loaded songs
    static constexpr int AUDIO_POOL_SIZE = 4;
    std::array<std::unique_ptr<AudioBuffer<float>>, AUDIO_POOL_SIZE> audioBufferPool;
    std::atomic<int> poolIndex{0};
    
    // Loop crossfade buffers for click-free loop transitions
    juce::AudioBuffer<float> loopCrossfadeBuffer;
    bool loopCrossfadeActive{false};
    int loopCrossfadeSamples{0};
    int loopCrossfadePosition{0};
    
    // Smart caching for waveform data to reduce memory allocations
    struct WaveformCache {
        std::vector<float> peaks;
        double lastDuration = 0.0;
        bool valid = false;
        std::chrono::steady_clock::time_point lastUpdate;
    } waveformCache;

    // Loop state
    bool loopEnabled{false};
    double loopStartSec{0.0};
    double loopEndSec{0.0};
    
    // Scratch state
    bool scratchMode{false};
    double scratchVelocity{0.0};
    
    // Keylock state
    bool keylockEnabled{false};
    // Defer keylock toggles to audio thread: -1 none, 0 disable, 1 enable
    std::atomic<int> keylockChangePending{-1};
    // Debug logging for keylock paths
    bool debugKeylock{true};
    // Short warm-up delay for keylock to ensure internal buffers are primed (~5ms)
    int keylockPrimeSamplesRemaining{0};
    double keylockPrimeMs{5.0};
    
    // Quantize state
    bool quantizeEnabled{false};
    double trackBpm{120.0};
    double trackFirstBeatOffset{0.0};
    double trackLengthSec{0.0};
    
    // Audio level monitoring (thread-safe for real-time display)
    std::atomic<float> leftChannelLevel{0.0f};
    std::atomic<float> rightChannelLevel{0.0f};
};

#endif //GUI_APP_EXAMPLE_DJAUDIOPLAYER_H
