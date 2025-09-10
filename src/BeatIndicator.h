#pragma once
#include <QWidget>
#include <QPaintEvent>
#include "GlobalBeatGrid.h"

class BeatIndicator : public QWidget {
    Q_OBJECT

public:
    explicit BeatIndicator(QWidget* parent = nullptr);
    
    void setBeatPositionDeckA(double currentBeat); // 0.0 to 4.0 (one bar)
    void setBeatPositionDeckB(double currentBeat); // 0.0 to 4.0 (one bar)
    // Original analyzed BPM per deck (not speed-scaled)
    void setBpmDeckA(double bpm);
    void setBpmDeckB(double bpm);
    // Per-deck tempo factor (relative speed, e.g. 1.02 for +2%)
    void setTempoFactorDeckA(double factor);
    void setTempoFactorDeckB(double factor);
    // Per-deck first beat offset (seconds) from BPM analysis
    void setFirstBeatOffsetDeckA(double seconds);
    void setFirstBeatOffsetDeckB(double seconds);
    
    // NEW: Use global beat grid for accurate beat calculation
    void setTrackPositionDeckA(double positionSeconds); // Use track position in seconds
    void setTrackPositionDeckB(double positionSeconds); // Use track position in seconds
    
    // NEW: Getter methods for performance pads
    double getBpmDeckA() const { return bpmA; }
    double getBpmDeckB() const { return bpmB; }
    double getTempoFactorDeckA() const { return tempoFactorA; }
    double getTempoFactorDeckB() const { return tempoFactorB; }
    double getEffectiveBpmDeckA() const { return bpmA * tempoFactorA; }
    double getEffectiveBpmDeckB() const { return bpmB * tempoFactorB; }
    
protected:
    void paintEvent(QPaintEvent* event) override;
    
private:
    double currentBeatA = 0.0; // Current beat position for Deck A (0.0 to 4.0)
    double currentBeatB = 0.0; // Current beat position for Deck B (0.0 to 4.0)
    // Original analyzed BPMs (per deck). If 0.0, fall back to GlobalBeatGrid
    double bpmA = 0.0;
    double bpmB = 0.0;
    // Tempo factors per deck (1.0 = original speed)
    double tempoFactorA = 1.0;
    double tempoFactorB = 1.0;
    // First beat offsets per deck (seconds)
    double firstBeatOffsetA = 0.0;
    double firstBeatOffsetB = 0.0;
    
    // Box and layout sizes (rectangular, compact)
    static constexpr int BOX_W = 18;       // width of each beat box
    static constexpr int BOX_H = 10;       // height of each beat box (flatter)
    static constexpr int BOX_SPACING = 22; // distance between box starts (horizontal)
    static constexpr int ROW_GAP = 4;      // vertical gap between rows
    // Exact content width of 4 boxes: last box start at 3*BOX_SPACING, then + BOX_W
    static constexpr int TOTAL_WIDTH = (3 * BOX_SPACING + BOX_W);
};
