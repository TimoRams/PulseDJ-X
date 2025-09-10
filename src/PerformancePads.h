#pragma once

#include <QWidget>
#include <QPushButton>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QLabel>
#include <QTimer>
#include <array>
#include <vector>
#include "GlobalBeatGrid.h"

class DJAudioPlayer;
class BeatIndicator;

class PerformancePads : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Cue = 0, BeatLoop = 1, BeatJump = 2 };
    public:
    enum class DeckId { A, B };
    
    PerformancePads(DeckId deckId, QWidget* parent = nullptr);
    void setAudioPlayer(DJAudioPlayer* player);
    void setBeatIndicator(BeatIndicator* indicator); // NEW: Set beat indicator reference
    
    // NEW: Get current cue points for waveform display
    const std::array<double, 8>& getCuePoints() const { return cuePoints; }

signals:
    void modeChanged(Mode mode);
    void cuePointsChanged(const std::array<double, 8>& cuePoints); // NEW: Signal for cue point updates
    void ghostLoopChanged(bool enabled, double startSec, double endSec); // NEW: Signal for ghost loop updates

private slots:
    void setModeCue();
    void setModeLoop();
    void setModeJump();
    void onPadPressed(int idx);

private:
    void updatePadLabels();
    void refreshPadStyles();
    void storeCue(int idx);
    void recallCue(int idx);
    void triggerLoop(int idx);
    void triggerJump(int idx);
    
    // Beat and BPM utilities
    double getCurrentBpm() const;
    double quantizeToNearestBeat(double positionSeconds) const;
    double getSecondsPerBeat() const;
    double getOriginalSecondsPerBeat() const; // NEW: Get original BPM for loop calculations

    private:
    DJAudioPlayer* player = nullptr;
    BeatIndicator* beatIndicator = nullptr; // NEW: Reference to beat indicator
    DeckId deckId;
    Mode currentMode{Mode::Cue};
    std::array<QPushButton*, 8> pads{};
    std::array<double, 8> cuePoints{}; // seconds; -1 for empty
    int activeLoopPad{-1};
    QPushButton* cueModeBtn{nullptr};
    QPushButton* loopModeBtn{nullptr};
    QPushButton* jumpModeBtn{nullptr};
    QTimer* styleUpdateTimer{nullptr};
    
    // Ghost loop state for visual feedback after loop is disabled
    bool ghostLoopEnabled{false};
    double ghostLoopStartSec{0.0};
    double ghostLoopEndSec{0.0};
};
