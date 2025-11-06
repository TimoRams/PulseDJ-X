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
#include <QRect>
#include <QCursor>
#include <QAbstractButton>
#include <ctime>
#include <chrono>
#include <memory>
#include <QSet>
#include "WaveformGenerator.h"
#include "DeckWidget.h"
#include "BeatIndicator.h"
#include "MenuBar.h"
#include <QListWidget>
#include "LibraryManager.h"
#include "MasterLevelMonitor.h"
#include "StereoAudioCallback.h"
// #include "AudioMixer.h" // Removed - using simplified AudioSourcePlayer approach
class DJAudioPlayer;
class BpmAnalyzer;
class PreferencesDialog;
class ScratchEngine;

class QtMainWindow : public QWidget {
    Q_OBJECT
    
    // Forward declaration and friend class for threaded BPM analysis
    friend class BpmAnalysisTask;
    friend class TopWaveformDisplayTask;
    friend class WaveformStreamChunkTask;
    
public:
    explicit QtMainWindow(QWidget* parent = nullptr);
    ~QtMainWindow();

    // Expose window drag helpers for child widgets (e.g., custom menu bar)
    void beginExternalWindowDrag(const QPoint& globalPos);
    void updateExternalWindowDrag(const QPoint& globalPos);
    void endExternalWindowDrag();

protected:
    // Event filter for double-click reset functionality
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onCrossfader(int v);
    void initializeAudio();
    void onLibraryLoadToDeck(int deckIndex, const QString& filePath); // new slot
    void onAnalyzeTracksRequested(const QStringList& filePaths);
    void onAnalyzeTracksAdvancedRequested(const QStringList& filePaths, double minBpm, double maxBpm);
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
    void handleBpmAnalysisResult(double bpm, const std::vector<double>& beatsSec, double totalSec, 
                                 const std::string& algorithm, double firstBeatOffset, bool isDeckA);
    std::unique_ptr<BpmAnalyzer> bpmAnalyzer;
    
    // MIDI control access methods
    void setCrossfaderPosition(float normalizedValue); // 0.0 = full A, 1.0 = full B
    void setDeckAPlayPause(bool shouldPlay); // MIDI control for Deck A play/pause
    void setDeckBPlayPause(bool shouldPlay); // MIDI control for Deck B play/pause
    void setDeckATempo(float normalizedValue); // MIDI control for Deck A tempo/pitch (0.0 = -100%, 0.5 = normal, 1.0 = +100%)
    void setDeckBTempo(float normalizedValue); // MIDI control for Deck B tempo/pitch (0.0 = -100%, 0.5 = normal, 1.0 = +100%)
    void setDeckAVolume(float normalizedValue); // MIDI control for Deck A channel volume (0.0 = mute, 1.0 = full)
    void setDeckBVolume(float normalizedValue); // MIDI control for Deck B channel volume (0.0 = mute, 1.0 = full)
    DJAudioPlayer* getPlayerA() const { return playerA.get(); }
    DJAudioPlayer* getPlayerB() const { return playerB.get(); }
    DJAudioPlayer* getPlayer(bool isDeckA) const { return isDeckA ? playerA.get() : playerB.get(); }
    class WaveformDisplay* overviewTopA;
    class WaveformDisplay* overviewTopB;
    static std::shared_ptr<juce::AudioFormatManager> sharedFormatManager;

    QtDeckWidget* deckA;
    QtDeckWidget* deckB;

    std::unique_ptr<DJAudioPlayer> playerA;
    std::unique_ptr<DJAudioPlayer> playerB;
    std::unique_ptr<ScratchEngine> scratchEngineA;
    std::unique_ptr<ScratchEngine> scratchEngineB;

private:
    struct WaveformStreamSession {
        QString filePath;
        WaveformGenerator::AnalysisMetadata metadata;
        int totalBins{0};
        double lengthSeconds{0.0};
        double binsPerSecond{0.0};
        int chunkBinSize{4096};
        int cacheCapacityBins{0};
        bool valid{false};
        bool hasCache{false};
        int cachedStartBin{0};
        int cachedEndBin{0};
        QSet<int> pendingChunks;
    };

    // Analysis status for overview labels
    bool analysisActiveA{false};
    bool analysisActiveB{false};
    double analysisProgressA{0.0};
    double analysisProgressB{0.0};
    bool analysisFailedA{false};
    bool analysisFailedB{false};

    BeatIndicator* beatIndicator;
    QSlider* crossfader;
    
    // BetaPulseX Menu System
    MenuBar* menuBar{nullptr};
    
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

    // PREROLL SUPPORT: Timer for automatic position updates
    QTimer* positionUpdateTimer;
    
    // Scratching state management to prevent timer conflicts
    qint64 lastScratchEndA{0};
    qint64 lastScratchEndB{0};
    
    // Thread pool optimization
    std::unique_ptr<QThreadPool> bpmThreadPool;
    // Dedicated low-priority pool for waveform streaming/generation (isolated from BPM/audio tasks)
    std::unique_ptr<QThreadPool> waveformThreadPool;

    // Connections we want to explicitly disconnect during shutdown to avoid
    // queued region requests racing with widget teardown
    QMetaObject::Connection connWaveformRegionA;
    QMetaObject::Connection connWaveformRegionB;
    // Scratch resume state per deck
    bool scratchWasPlayingA{false};
    bool scratchWasPlayingB{false};
    // Sync state (follower flags): if true, that deck follows the other deck's tempo

    // Scratch inertia handling per deck
    QTimer* scratchInertiaTimerA{nullptr};
    QTimer* scratchInertiaTimerB{nullptr};
    double scratchInertiaVelocityA{0.0};
    double scratchInertiaVelocityB{0.0};
    double scratchInertiaElapsedA{0.0};
    double scratchInertiaElapsedB{0.0};
    bool scratchInertiaActiveA{false};
    bool scratchInertiaActiveB{false};
    bool scratchInertiaResumeA{false};
    bool scratchInertiaResumeB{false};
    bool syncAEnabled{false};
    bool syncBEnabled{false};
    // Prevent sync feedback loops
    bool syncUpdateInProgress{false};

    WaveformStreamSession streamSessionA;
    WaveformStreamSession streamSessionB;
    
    // Timer for continuous waveform fill-in (loads chunks progressively)
    QTimer* waveformFillInTimer{nullptr};

    // Optional per-deck visual sync trim (seconds), positive adds extra visual delay.
    // Useful for tiny per-system calibration without changing core logic.
    double userVisualTrimA{0.0};
    double userVisualTrimB{0.0};
    // Global output latency compensation (seconds) applied to waveform displays
    double userRenderLatencySec{0.03};
    
    // Window drag functionality for frameless window
    bool isDragging{false};
    QPoint dragStartPosition;
    bool systemMoveActive{false};
    bool externalDragActive{false};
    enum class ResizeRegion {
        None,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };
    static constexpr int resizeMargin = 8;
    ResizeRegion currentResizeRegion{ResizeRegion::None};
    bool isResizing{false};
    QPoint resizeStartPosition;
    QRect resizeStartGeometry;
    bool cursorOverridden{false};
    Qt::CursorShape currentCursorShape{Qt::ArrowCursor};

    bool shouldAnalyzeTrackOnLoad(const QString& filePath) const;
    void startDeckAnalysisIfNeeded(const QString& filePath, bool isDeckA);
    void handleWaveformRegionRequestDeckA(double startSec, double endSec);
    void handleWaveformRegionRequestDeckB(double startSec, double endSec);
    void handleWaveformRegionRequest(bool deckIsA, double startSec, double endSec);
    void scheduleWaveformChunk(bool deckIsA, int startBin, int binCount);
    void continuousWaveformFillIn(bool deckIsA); // Continuously loads chunks until complete
    void handleWaveformChunkResult(bool deckIsA,
                                   const QString& filePath,
                                   int startBin,
                                   std::shared_ptr<std::vector<float>> maxBins,
                                   std::shared_ptr<std::vector<float>> minBins,
                                   bool success,
                                   std::shared_ptr<std::vector<float>> lowBins,
                                   std::shared_ptr<std::vector<float>> midBins,
                                   std::shared_ptr<std::vector<float>> highBins);
    
protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    // Window drag functionality
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void updateOverviewLabel(bool isDeckA);
    void performCleanup(); // Safe cleanup method
    bool cleanupCompleted{false}; // Prevent double cleanup
    bool shutdownInitiated{false};
    
    // PREROLL SUPPORT: Update playback positions automatically
    void updatePlaybackPositions();
    
    // BetaPulseX Menu Setup
    // BetaPulseX: Deck Settings Management
    void applyDeckSettings();       // Wendet geladene Settings auf die Decks an
    void connectDeckSettings();     // Verbindet Deck-Controls mit Settings-System
    void applyStoredCuePoints(QtDeckWidget* deck, bool isDeckA);
    void applyStoredBeatGrid(QtDeckWidget* deck, bool isDeckA);
    void loadAndApplyCoverArt(QtDeckWidget* deck, bool isDeckA);  // NEW: Load cover art from DB
    void reapplyStoredDeckMetadata(bool isDeckA);

    // Scratch helper routines
    void applyScratchPosition(bool isDeckA, double absRel);
    void startScratchInertia(bool isDeckA, double initialVelocity, bool resumePlayback);
    void stopScratchInertia(bool isDeckA, bool resumePlayback);
    void handleScratchInertiaTick(bool isDeckA);
    void handleScratchStart(bool isDeckA);
    void handleScratchVelocityChanged(bool isDeckA, double velocity);
    void handleScratchEnd(bool isDeckA, double releaseVelocity);
    ResizeRegion detectResizeRegion(const QPoint& pos) const;
    void updateCursorForRegion(ResizeRegion region);
    void performResize(const QPoint& globalPos);
    void beginWindowDragInternal(const QPoint& globalPos, bool fromExternalSource);
    void updateWindowDragInternal(const QPoint& globalPos);
    void endWindowDragInternal();
    DJAudioPlayer* playerForDeck(bool isDeckA) const { return isDeckA ? playerA.get() : playerB.get(); }
};
