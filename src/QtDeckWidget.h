#pragma once

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QDoubleSpinBox>
#include <memory>
#include <QDragEnterEvent>
#include <QDropEvent>
#include "DeckWaveformOverview.h"
#include "QtTurntableWidget.h"
#include "PerformancePads.h"
class DJAudioPlayer;

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
    
    // Getter for Rekordbox-style layout (waveform now integrated into controls)
    QWidget* getControlsWidget() const { return controlsWidget; }
    DeckWaveformOverview* getWaveform() const { return waveform; }
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

private slots:
    void onPlayPause();
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

private:
    DJAudioPlayer* player;
    DeckWaveformOverview* waveform;
    QtTurntableWidget* turntable;
    QWidget* controlsWidget;  // Separate widget for controls
    QLabel* deckTitleLabel;
    QLabel* songNameLabel;
    QPushButton* playPauseBtn;
    QPushButton* loadBtn;
    QPushButton* cueBtn;
    QPushButton* keylockBtn;
    QPushButton* quantizeBtn;
    QPushButton* syncBtn;
    QSlider* speedSlider;
    QLabel* speedLabel;
    QLabel* tempoValueLabel;
    QDoubleSpinBox* tempoSpin;
    QLabel* bpmDefaultLabel; // Shows detected/default BPM
    QLabel* bpmCurrentLabel; // Shows speed-adjusted BPM
    PerformancePads* pads{nullptr};
    bool playing{false};
    QString currentFilePath;
    double detectedBpm{0.0};
    QTimer* statusTimer; // Timer for status synchronization
    double cuePosition{0.0}; // Stored cue point
    bool isCueing{false}; // True when cue button is held down
    QTimer* cueClickTimer; // Timer for detecting double-clicks on cue
    bool cueClickPending{false}; // True when waiting for potential second click
    qint64 lastPlayPressTime{0}; // Timestamp of last play button press
    
    // Loop state tracking for change detection
    bool lastLoopEnabled{false};
    double lastLoopStart{-1.0};
    double lastLoopEnd{-1.0};

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
};
