#include "BeatIndicator.h"
#include <QPainter>
#include <QColor>
#include <cmath>

BeatIndicator::BeatIndicator(QWidget* parent) : QWidget(parent) {
    // Two rows of flat boxes + padding
    setFixedSize(TOTAL_WIDTH, 2 * BOX_H + ROW_GAP + 2 * 6);
}

void BeatIndicator::setBeatPositionDeckA(double beat) {
    // Keep beat within one bar (0.0 to 4.0)
    currentBeatA = std::fmod(beat, 4.0);
    if (currentBeatA < 0.0) currentBeatA += 4.0;
    update();
}

void BeatIndicator::setBeatPositionDeckB(double beat) {
    // Keep beat within one bar (0.0 to 4.0)
    currentBeatB = std::fmod(beat, 4.0);
    if (currentBeatB < 0.0) currentBeatB += 4.0;
    update();
}

void BeatIndicator::setBpmDeckA(double newBpm) {
    if (newBpm > 0.0) {
        bpmA = newBpm;
    }
}

void BeatIndicator::setBpmDeckB(double newBpm) {
    if (newBpm > 0.0) {
        bpmB = newBpm;
    }
}

void BeatIndicator::setTempoFactorDeckA(double factor) {
    if (factor <= 0.0) factor = 1.0;
    tempoFactorA = factor;
}

void BeatIndicator::setTempoFactorDeckB(double factor) {
    if (factor <= 0.0) factor = 1.0;
    tempoFactorB = factor;
}

void BeatIndicator::setFirstBeatOffsetDeckA(double seconds) { firstBeatOffsetA = seconds; }
void BeatIndicator::setFirstBeatOffsetDeckB(double seconds) { firstBeatOffsetB = seconds; }

// NEW: Calculate beat position from track time using global beat grid
void BeatIndicator::setTrackPositionDeckA(double positionSeconds) {
    // Use original BPM for beat calculation (tempo scaling is already in positionSeconds)
    double baseBpm = (bpmA > 0.0) ? bpmA : GlobalBeatGrid::getInstance().getCurrentBpm();
    if (baseBpm <= 0.0) {
        setBeatPositionDeckA(0.0);
        return;
    }
    
    // Calculate beat position using original BPM since position already reflects tempo
    double beatPeriod = 60.0 / baseBpm;
    double beatPosition = 0.0;
    if (beatPeriod > 0.0) {
        beatPosition = (positionSeconds - firstBeatOffsetA) / beatPeriod;
    }
    
    // Calculate which beat we're on (0-based index matching waveform)
    int beatIndex = (int)std::floor(beatPosition);
    if (beatIndex < 0) beatIndex = 0;
    
    // Calculate position within the current 4-beat cycle
    // Orange lines in waveform appear when beatIndex % 4 == 0
    // Map this to BeatIndicator display: beat 0->1, beat 1->2, beat 2->3, beat 3->4
    int beatInCycle = beatIndex % 4;
    double fractionalBeat = beatPosition - std::floor(beatPosition);
    
    // Set beat position: 0.0-0.99 = beat "1", 1.0-1.99 = beat "2", etc.
    setBeatPositionDeckA(beatInCycle + fractionalBeat);
}

void BeatIndicator::setTrackPositionDeckB(double positionSeconds) {
    // Use original BPM for beat calculation (tempo scaling is already in positionSeconds)
    double baseBpm = (bpmB > 0.0) ? bpmB : GlobalBeatGrid::getInstance().getCurrentBpm();
    if (baseBpm <= 0.0) {
        setBeatPositionDeckB(0.0);
        return;
    }
    
    // Calculate beat position using original BPM since position already reflects tempo
    double beatPeriod = 60.0 / baseBpm;
    double beatPosition = 0.0;
    if (beatPeriod > 0.0) {
        beatPosition = (positionSeconds - firstBeatOffsetB) / beatPeriod;
    }
    
    // Calculate which beat we're on (0-based index matching waveform)
    int beatIndex = (int)std::floor(beatPosition);
    if (beatIndex < 0) beatIndex = 0;
    
    // Calculate position within the current 4-beat cycle
    // Orange lines in waveform appear when beatIndex % 4 == 0
    // Map this to BeatIndicator display: beat 0->1, beat 1->2, beat 2->3, beat 3->4
    int beatInCycle = beatIndex % 4;
    double fractionalBeat = beatPosition - std::floor(beatPosition);
    
    // Set beat position: 0.0-0.99 = beat "1", 1.0-1.99 = beat "2", etc.
    setBeatPositionDeckB(beatInCycle + fractionalBeat);
}

void BeatIndicator::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    
    // Background
    p.fillRect(rect(), QColor(20, 20, 25));
    
    // Calculate which beat we're currently in for each deck (0-3)
    int currentBeatIndexA = (int)std::floor(currentBeatA);
    if (currentBeatIndexA >= 4) currentBeatIndexA = 3;
    if (currentBeatIndexA < 0) currentBeatIndexA = 0;
    
    int currentBeatIndexB = (int)std::floor(currentBeatB);
    if (currentBeatIndexB >= 4) currentBeatIndexB = 3;
    if (currentBeatIndexB < 0) currentBeatIndexB = 0;
    
    // Check if we're on a strong beat (beat 0 = beat 1) to show orange line
    bool strongBeatA = (currentBeatIndexA == 0);
    bool strongBeatB = (currentBeatIndexB == 0);
    
    // Centered grid: exact content width = 3*BOX_SPACING + BOX_W
    const int contentW = 3 * BOX_SPACING + BOX_W;
    const int startX = (int) std::round((width() - contentW) * 0.5);
    const int topY = (int) std::round((height() - (2 * BOX_H + ROW_GAP)) * 0.5);
    const int bottomY = topY + BOX_H + ROW_GAP;
    
    // Orange line for strong beat (sync with white beat grid lines)
    // Strong beat lines removed for a cleaner compact look
    
    for (int i = 0; i < 4; ++i) {
        int x = startX + i * BOX_SPACING;
        
    // Top row (blue boxes for Deck A) - flat rectangles
    QColor topColor = (i == currentBeatIndexA) ? QColor(100, 150, 255) : QColor(40, 60, 80);
        p.setBrush(QBrush(topColor));
    p.setPen(QPen(QColor(200, 200, 255), 1));
    p.drawRect(x, topY, BOX_W, BOX_H);
        
        // Beat number in top box - always show 1,2,3,4
    p.setPen(QPen(QColor(255, 255, 255), 1));
    p.setFont(QFont("Arial", 8, QFont::Bold));
    p.drawText(x, topY, BOX_W, BOX_H, Qt::AlignCenter, QString::number(i + 1));
        
        // Bottom row (orange boxes for Deck B)
        QColor bottomColor = (i == currentBeatIndexB) ? QColor(255, 150, 50) : QColor(80, 50, 20);
        p.setBrush(QBrush(bottomColor));
        p.setPen(QPen(QColor(255, 200, 100), 1));
    p.drawRect(x, bottomY, BOX_W, BOX_H);
        
        // Beat number in bottom box - always show 1,2,3,4
    p.setPen(QPen(QColor(255, 255, 255), 1));
    p.drawText(x, bottomY, BOX_W, BOX_H, Qt::AlignCenter, QString::number(i + 1));
    }
    
    // Draw progress bar showing position within current beat for Deck A
    double beatProgressA = currentBeatA - std::floor(currentBeatA);
    int progressXA = startX + currentBeatIndexA * BOX_SPACING;
    int progressWidthA = (int)(BOX_W * beatProgressA);
    
    // Progress overlay on current beat for Deck A (top row)
    if (progressWidthA > 0) {
        QColor progressColorA(255, 255, 255, 120);
        p.setBrush(QBrush(progressColorA));
        p.setPen(Qt::NoPen);
    p.drawRect(progressXA, topY, progressWidthA, BOX_H);
    }
    
    // Draw progress bar showing position within current beat for Deck B
    double beatProgressB = currentBeatB - std::floor(currentBeatB);
    int progressXB = startX + currentBeatIndexB * BOX_SPACING;
    int progressWidthB = (int)(BOX_W * beatProgressB);
    
    // Progress overlay on current beat for Deck B (bottom row)
    if (progressWidthB > 0) {
        QColor progressColorB(255, 255, 255, 120);
        p.setBrush(QBrush(progressColorB));
        p.setPen(Qt::NoPen);
    p.drawRect(progressXB, bottomY, progressWidthB, BOX_H);
    }
    
    // No A/B labels: nur die Zahlen 1..4 in den Kästchen
    
    // Strong beat indicator text
    // Removed the "BEAT" labels for minimal UI
}
