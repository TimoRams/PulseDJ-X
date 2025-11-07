#pragma once

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QString>
#include <memory>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <atomic>
#include <cstdint>
#include <vector>
#include <chrono>
#include "DeckWaveformOverview.h"
#include "TurntableWidget.h"
#include "PerformancePads.h"
class DJAudioPlayer;
class ScratchEngine;

class QtDeckWidget : public QWidget {
    Q_OBJECT
public:
    explicit QtDeckWidget(DJAudioPlayer* player, QWidget* parent = nullptr, const QString& deckTitle = "DECK", bool isLeftDeck = true);

signals:
    void playStateChanged(bool playing);
    void playheadUpdated(double relative);
    void fileLoaded();  // No parameter needed
    void fileLoadingStarted(const QString& filePath); // NEW: Signal for threaded loading
    void displayedBpmChanged(double displayedBpm);
    void tempoFactorChanged(double factor); // New signal for tempo changes
    void syncRequested(QtDeckWidget* requester); // One-shot sync
    void syncToggled(QtDeckWidget* requester, bool enabled); // Toggle follow-sync
    void loopChanged(bool enabled, double startSec, double endSec); // NEW: Loop status signal
    void fileUnloaded(); // NEW: Emitted when the deck unloads the track
    void turntableScratchStart();
    void turntableScratchMove(double relative);
    void turntableScratchVelocityChanged(double velocity);
    void turntableScratchEnd(double releaseVelocity);

public:
    void loadFile(const QString& path);
    void setDeckTitle(const QString& title);
    void setBeatIndicator(class BeatIndicator* indicator); // NEW: Set beat indicator
    QString getCurrentFilePath() const { return currentFilePath; }
    Q_SLOT void setDetectedBpm(double bpm);
    // Tempo helpers
    double getTempoFactor() const;                 // Current speed factor (1.0 = original)
    double getDetectedBpm() const { return detectedBpm; }
    void setTempoFactor(double factor);            // Programmatically set tempo
    double getMinTempoFactor() const { return minTempoFactor; } // Get current minimum tempo range
    double getMaxTempoFactor() const { return maxTempoFactor; } // Get current maximum tempo range
    // Turntable control
    void setPlatterSeconds(double seconds) { if (turntable) turntable->setPositionSeconds(seconds); }

    void setTrackNameDisplay(const QString& text, const QString& tooltip = QString());
    void setTrackInfoDisplay(const QString& text, const QString& style = QString(), const QString& tooltip = QString());
    void setCoverArt(const QByteArray& imageData, const QString& format); // NEW: Set cover art
    void setScratchEngine(ScratchEngine* engine);
    ScratchEngine* getScratchEngine() const { return scratchEngine; }
    
    // Getter for Rekordbox-style layout (waveform now integrated into controls)
    QWidget* getControlsWidget() const { return controlsWidget; }
    DeckWaveformOverview* getWaveform() const { return waveform; }
    QtTurntableWidget* getTurntable() const { return turntable; }
    PerformancePads* getPerformancePads() const { return pads; } // NEW: Access to performance pads
    
    // BetaPulseX: Getter für Settings-Integration
    QPushButton* getKeylockButton() const { return keylockBtn; }
    QPushButton* getQuantizeButton() const { return quantizeBtn; }
    QSlider* getSpeedSlider() const { return speedSlider; }
    
    // NEW: Handle threaded file loading completion
    void onFileLoadingComplete(const QString& filePath);

public slots:
    // BetaPulseX: Public slots für Settings-Integration
    void onKeylockToggle();
    void onQuantizeToggle();
    void onUnload();
    void onTempoRangeSelected();
    void onPlayPause();  // Made public for MIDI integration

private slots:
    void onLoad();
    void onCue();
    void onCuePressed();
    void onCueReleased();
    void onVolumeChanged(int v);
    void onSpeedChanged(int v);
    void syncPlayState(); // New slot for status synchronization
    void onSync();
    void onSyncToggled(bool enabled);
    void onTempoSpinChanged(double v);
    void applyTempo(double factor);
    void setTempoRangePm6();
    void setTempoRangePm8();
    void setTempoRangePm16();
    void setTempoRangeWide();
    void updateTempoControlsForRange();
    void handleOverviewWaveformResult(std::uint64_t generation,
                                      std::shared_ptr<std::vector<float>> amplitudes,
                                      std::shared_ptr<std::vector<float>> colours,
                                      double audioStart,
                                      double lengthSec);
    bool isWaveformGenerationCurrent(std::uint64_t generation) const noexcept;
    void resetDeckUiToEmptyState();

private:
    DJAudioPlayer* player;
    DeckWaveformOverview* waveform;
    QtTurntableWidget* turntable;
    QWidget* controlsWidget;  // Separate widget for controls
    QLabel* deckTitleLabel;
    QLabel* songNameLabel;
    QLabel* trackInfoLabel;
    QLabel* coverArtLabel;      // Cover art placeholder (top)
    QLabel* coverArtLabelWave;  // Cover art placeholder (waveform row)
    QPushButton* playPauseBtn;
    QPushButton* loadBtn;
    QPushButton* unloadBtn;
    QPushButton* cueBtn;
    QPushButton* keylockBtn;
    QPushButton* quantizeBtn;
    QPushButton* syncBtn;
    QPushButton* tempoRangeBtn;
    QSlider* speedSlider;
    QLabel* speedLabel;
    QLabel* tempoValueLabel;
    QDoubleSpinBox* tempoSpin;
    QLabel* bpmDefaultLabel; // Shows detected/default BPM
    QLabel* bpmCurrentLabel; // Shows speed-adjusted BPM
    QMenu* tempoRangeMenu{nullptr};
    PerformancePads* pads{nullptr};
    bool playing{false};
    QString currentFilePath;
    double detectedBpm{0.0};
    QTimer* statusTimer; // Timer for status synchronization
    double cuePosition{0.0}; // Stored cue point
    bool isCueing{false}; // True when cue button is held down
    QTimer* cueClickTimer; // Timer for detecting double-clicks on cue
    bool cueClickPending{false}; // True when waiting for potential second click
    std::chrono::steady_clock::time_point lastPlayPressTime{};
    // Dynamic tempo range
    double minTempoFactor{0.8400};
    double maxTempoFactor{1.1600};
    int tempoRangeIndex{2}; // 0: ±6, 1: ±8, 2: ±16, 3: WIDE
    
    // Loop state tracking for change detection
    bool lastLoopEnabled{false};
    double lastLoopStart{-1.0};
    double lastLoopEnd{-1.0};
    ScratchEngine* scratchEngine{nullptr};
    std::atomic<std::uint64_t> waveformTaskGeneration{0};

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
};
