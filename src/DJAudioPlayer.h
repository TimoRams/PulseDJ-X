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
    void setPitchBendRatio(double ratio) noexcept;
    double getPitchBendRatio() const { return pitchBendRatio; }
    void setPositionRelative(double pos);
    double getPositionRelative();
    void start();
    void stop();
    // Fully unload the current track and detach sources
    void unload();
    // Pause is an alias to stop playback without unloading the track
    void pause() { stop(); }
    bool isPlaying();
    // Transport helpers
    double getCurrentPositionSeconds() const;
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
    void setScratchPlaybackContext(bool wasPlaying) noexcept;
    bool isScratchMode() const { return scratchMode.load(); }
    [[nodiscard]] bool isSoftPaused() const noexcept;
    [[nodiscard]] bool isAudible() const noexcept;
    void ensureScratchAudible();

    // Simple loop control (seconds)
    void enableLoop(double startSec, double lengthSec);
    void disableLoop() noexcept;
    bool isLoopEnabled() const { return loopEnabled; }
    double getLoopStart() const { return loopStartSec; }
    double getLoopEnd() const { return loopEndSec; }
    
    // Simple EQ/filter control stubs (values: -1.0 .. +1.0)
    void setHighGain(double v) noexcept;
    void setMidGain(double v) noexcept;
    void setLowGain(double v) noexcept;
    void setFilterCutoff(double v) noexcept;
    
    // Keylock (pitch lock) - maintains original pitch when speed changes
    void setKeylockEnabled(bool enabled) noexcept;
    bool isKeylockEnabled() const { return keylockEnabled; }
    // Runtime keylock quality profile
    enum class KeylockQuality { Fast, Balanced, Quality };
    void setKeylockQuality(KeylockQuality q);
    KeylockQuality getKeylockQuality() const { return rbQuality; }
    
    // Quantize control - snaps cues and loops to nearest beat
    void setQuantizeEnabled(bool enabled) noexcept;
    bool isQuantizeEnabled() const { return quantizeEnabled; }
    void setBeatInfo(double bpm, double firstBeatOffset, double trackLength) noexcept;
    double quantizePosition(double positionSec) const noexcept;
    // Beat info getters
    double getTrackBpm() const { return trackBpm; }
    double getFirstBeatOffset() const { return trackFirstBeatOffset; }
    double getTrackLengthSeconds() const { return trackLengthSec; }
    
    // Audio level monitoring for Master Out display
    float getLeftChannelLevel() const { return leftChannelLevel.load(); }
    float getRightChannelLevel() const { return rightChannelLevel.load(); }
    
    // Latency measurement (in milliseconds)
    double getMeasuredLatencyMs() const { return measuredLatencyMs.load(); }
    int getLatencyCompensationSamples() const { return latencyCompensationSamples; }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const AudioSourceChannelInfo &bufferToFill) override;
    void releaseResources() override;

private:
    void setPosition(double posInSecs);
    void renderScratchAudio(const AudioSourceChannelInfo &bufferToFill);
    float fetchScratchSample(AudioFormatReader* reader, double samplePos, int channel);
    void ensureScratchCache(AudioFormatReader* reader, int64 sampleIndex);
    
    void applyDSPEffects(const AudioSourceChannelInfo &bufferToFill);
    void applyCrossfadeTransition(const AudioSourceChannelInfo &bufferToFill);
    
#if defined(RUBBERBAND_FOUND)
    void reinitRubberBand();
#endif

    AudioFormatManager &formatManager;
    AudioTransportSource transportSource;
    std::unique_ptr<AudioFormatReaderSource> readerSource;
    ResamplingAudioSource resampleSource{&transportSource, false, 2};
    double highGain{0.0};
    double midGain{0.0};
    double lowGain{0.0};
    double filterKnob{0.0};

    juce::dsp::IIR::Filter<float> lowShelf;
    juce::dsp::IIR::Filter<float> midPeak;
    juce::dsp::IIR::Filter<float> highShelf;
    juce::dsp::StateVariableTPTFilter<float> svf;
    
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
    double pitchBendRatio{1.0};
    double pitchShiftRatio{1.0};
    double effectiveSpeed() const noexcept;
    void updateResampleRatio() noexcept;

#if defined(RUBBERBAND_FOUND)
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

    std::atomic<bool> forceSilent{false};
    std::atomic<bool> softPaused{false};
    std::atomic<bool> savePosRequested{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> pausedResetPending{false};
    
    // Latency compensation and crossfade buffering
    static constexpr int kKeylockTransitionMs = 5;
    AudioBuffer<float> transitionBuffer;
    bool transitionBufferValid = false;
    int transitionSamplesRemaining = 0;
    int transitionSamplesTotal = 0;
    bool transitionToKeylock = false;
    
    std::atomic<double> measuredLatencyMs{0.0};
    int latencyCompensationSamples = 0;
    
    std::atomic<int> keylockChangePending{-1};

    double currentSampleRate{44100.0};
    bool dspPrepared{false};
    double pausedPosSec{0.0};
    
    bool startFadeActive{false};
    bool stopFadeActive{false};
    int fadeSamplesRemaining{0};
    int fadeSamplesTotal{0};
    float fadeStartGain{1.0f};
    float fadeTargetGain{1.0f};
    float lastOutputSampleL{0.0f};
    float lastOutputSampleR{0.0f};
    bool resumeCompensatePending{false};
    int resumeWarmupSamplesRemaining{0};
    int lastBlockSizeHint{512};
    
    static constexpr int AUDIO_POOL_SIZE = 4;
    std::array<std::unique_ptr<AudioBuffer<float>>, AUDIO_POOL_SIZE> audioBufferPool;
    std::atomic<int> poolIndex{0};
    
    juce::AudioBuffer<float> loopCrossfadeBuffer;
    bool loopCrossfadeActive{false};
    int loopCrossfadeSamples{0};
    int loopCrossfadePosition{0};
    
    struct WaveformCache {
        std::vector<float> peaks;
        double lastDuration = 0.0;
        bool valid = false;
        std::chrono::steady_clock::time_point lastUpdate;
    } waveformCache;

    bool loopEnabled = false;
    double loopStartSec{0.0};
    double loopEndSec{0.0};
    
    // Scratch state
    std::atomic<bool> scratchMode{false};
    std::atomic<double> scratchVelocity{0.0};
    std::atomic<double> scratchTargetSeconds{0.0};
    std::atomic<bool> scratchJumpPending{false};
    std::atomic<double> scratchAudioSeconds{0.0};
    std::atomic<bool> scratchContextWasPlaying{false};
    double scratchCurrentSeconds{0.0};
    double scratchSmoothedVelocity{0.0};
    int scratchFadeSamplesRemaining{0};
    int scratchFadeSamplesTotal{0};
    float scratchFadeStartL{0.0f};
    float scratchFadeStartR{0.0f};
    float scratchPrevSampleL{0.0f};
    float scratchPrevSampleR{0.0f};
    juce::AudioBuffer<float> scratchCacheBuffer;
    int64 scratchCacheStartSample{0};
    int scratchCacheValidSamples{0};
    bool scratchCacheValid{false};
    static constexpr int SCRATCH_CACHE_SAMPLES = 8192;
    
    // Keylock state
    bool keylockEnabled{false};
    // Debug logging for keylock paths
    bool debugKeylock{false};
    // Short warm-up delay for keylock to ensure internal buffers are primed (~5ms)
    int keylockPrimeSamplesRemaining{0};
    double keylockPrimeMs{5.0};
    
    // Quantize state
    bool quantizeEnabled{false};
    double trackBpm{120.0};
    double trackFirstBeatOffset{0.0};
    double trackLengthSec{0.0};
    
    // Preroll state for DJ-style cueing
    double prerollPosition{0.0};        // Current preroll position (negative when in preroll)
    bool inPrerollMode{false};          // Whether we're currently in preroll area
    double prerollTimeSec{8.0};         // Preroll time in seconds (matches WaveformDisplay)
    
    // Audio level monitoring (thread-safe for real-time display)
    std::atomic<float> leftChannelLevel{0.0f};
    std::atomic<float> rightChannelLevel{0.0f};
};

#endif //GUI_APP_EXAMPLE_DJAUDIOPLAYER_H
