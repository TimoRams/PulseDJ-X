#pragma once

#include <QWidget>
#include <QHBoxLayout>
#include <QSlider>
#include <QDial>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QThreadPool>
#include <QProgressBar>
#include <ctime>
#include <chrono>
#include "QtDeckWidget.h"
#include "BeatIndicator.h"
#include "MenuBar.h"
#include <QListWidget>
#include "LibraryManager.h"
#include "MasterLevelMonitor.h"
// #include "AudioMixer.h" // Removed - using simplified AudioSourcePlayer approach
class DJAudioPlayer;
class BpmAnalyzer;
class PreferencesDialog;

// Custom audio callback for proper stereo output
class StereoAudioCallback : public juce::AudioIODeviceCallback {
public:
    explicit StereoAudioCallback(DJAudioPlayer* player) : audioPlayer(player) {}
    
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                         float* const* outputChannelData, int numOutputChannels, 
                                         int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    
private:
    DJAudioPlayer* audioPlayer{nullptr};
    juce::AudioBuffer<float> tempBuffer;
};

class QtMainWindow : public QWidget {
    Q_OBJECT
    
    // Forward declaration and friend class for threaded BPM analysis
    friend class BpmAnalysisTask;
    
public:
    explicit QtMainWindow(QWidget* parent = nullptr);
    ~QtMainWindow();

private slots:
    void onCrossfader(int v);
    void initializeAudio();
    // EQ/filter slots
    void onLeftHighChanged(int v);
    void onLeftMidChanged(int v);
    void onLeftLowChanged(int v);
    void onLeftFilterChanged(int v);
    void onRightHighChanged(int v);
    void onRightMidChanged(int v);
    void onRightLowChanged(int v);
    void onRightFilterChanged(int v);
    // Volume slider slots
    void onLeftVolumeChanged(int v);
    void onRightVolumeChanged(int v);

public:
    // Performance optimization: Handle BPM analysis results (public for thread access)
    void handleBpmAnalysisResult(double bpm, const std::vector<double>& beatsSec, double totalSec, 
                                const std::string& algorithm, double firstBeatOffset, bool isDeckA);
    // Performance optimization: Make BPM analyzer accessible to threaded tasks
    BpmAnalyzer* bpmAnalyzer{nullptr};
    // THREADING FIX: Make waveform displays accessible to threads
    class WaveformDisplay* overviewTopA;
    class WaveformDisplay* overviewTopB;
    // THREADING FIX: Make format manager accessible to threads
    static juce::AudioFormatManager* sharedFormatManager;
    
    // THREADING FIX: Make deck widgets accessible to threads
    QtDeckWidget* deckA;
    QtDeckWidget* deckB;
    
    // THREADING FIX: Make players accessible to threads
    DJAudioPlayer* playerA;
    DJAudioPlayer* playerB;

private:
    // Analysis status for overview labels
    bool analysisActiveA{false};
    bool analysisActiveB{false};
    double analysisProgressA{0.0};
    double analysisProgressB{0.0};
    bool analysisFailedA{false};
    bool analysisFailedB{false};

    BeatIndicator* beatIndicator;
    QLabel* deckALabel;
    QLabel* deckBLabel;
    QSlider* crossfader;
    
    // BetaPulseX Menu System
    MenuBar* menuBar;
    
    // Store algorithm names for BPM display
    QString algorithmA;
    QString algorithmB;
    // EQ knobs
    QDial* leftHigh;
    QDial* leftMid;
    QDial* leftLow;
    QDial* leftFilter;
    QDial* rightHigh;
    QDial* rightMid;
    QDial* rightLow;
    QDial* rightFilter;
    // Volume sliders (moved from deck widgets)
    QSlider* leftVolumeSlider;
    QSlider* rightVolumeSlider;
    LibraryManager* libraryManager;
    juce::AudioDeviceManager deviceManager;
    
    // Custom stereo audio callback instead of AudioSourcePlayer
    std::unique_ptr<StereoAudioCallback> stereoCallback;
    
    // Master output level monitoring for the menubar display
    MasterLevelMonitor masterLevelMonitor;
    
    // Performance optimization: Cached format manager to avoid repeated initialization
    static int formatManagerRefCount;
    
    // Thread pool optimization
    std::unique_ptr<QThreadPool> bpmThreadPool;
    // Scratch resume state per deck
    bool scratchWasPlayingA{false};
    bool scratchWasPlayingB{false};
    // Sync state (follower flags): if true, that deck follows the other deck's tempo
    bool syncAEnabled{false};
    bool syncBEnabled{false};
    // Prevent sync feedback loops
    bool syncUpdateInProgress{false};

    // Optional per-deck visual sync trim (seconds), positive adds extra visual delay.
    // Useful for tiny per-system calibration without changing core logic.
    double userVisualTrimA{0.0};
    double userVisualTrimB{0.0};
    
    // Window drag functionality for frameless window
    bool isDragging{false};
    QPoint dragStartPosition;
    
protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    // Window drag functionality
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void updateOverviewLabel(bool isDeckA);
    void performCleanup(); // Safe cleanup method
    bool cleanupCompleted{false}; // Prevent double cleanup
    
    // BetaPulseX Menu Setup
    // BetaPulseX: Deck Settings Management
    void applyDeckSettings();       // Wendet geladene Settings auf die Decks an
    void connectDeckSettings();     // Verbindet Deck-Controls mit Settings-System
};
