#include "BeatIndicator.h"
#include <QPainter>
#include <QColor>
#include <cmath>
#include <QDebug>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

BeatIndicator::BeatIndicator(QWidget* parent) : QWidget(parent) {
    // Two rows of flat boxes + padding
    setFixedSize(TOTAL_WIDTH, 2 * BOX_H + ROW_GAP + 2 * 6);
}

void BeatIndicator::setBeatPositionDeckA(double beat) {
    // Keep beat within one bar (0.0 to 4.0)
    double newBeatA = std::fmod(beat, 4.0);
    if (newBeatA < 0.0) newBeatA += 4.0;
    
    // Only update if beat position changed significantly (avoid excessive redraws)
    if (std::abs(newBeatA - currentBeatA) > 0.01) {
        currentBeatA = newBeatA;
        update();
    } else {
        currentBeatA = newBeatA; // Update value but don't redraw
    }
}

void BeatIndicator::setBeatPositionDeckB(double beat) {
    // Keep beat within one bar (0.0 to 4.0)
    double newBeatB = std::fmod(beat, 4.0);
    if (newBeatB < 0.0) newBeatB += 4.0;
    
    // Only update if beat position changed significantly (avoid excessive redraws)
    if (std::abs(newBeatB - currentBeatB) > 0.01) {
        currentBeatB = newBeatB;
        update();
    } else {
        currentBeatB = newBeatB; // Update value but don't redraw
    }
}

void BeatIndicator::setBpmDeckA(double newBpm) {
    if (newBpm > 0.0) {
        bpmA = newBpm;
        gridAvailableA = true;
        update();
    }
}

void BeatIndicator::setBpmDeckB(double newBpm) {
    if (newBpm > 0.0) {
        bpmB = newBpm;
        gridAvailableB = true;
        update();
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
    // If no beat grid is available yet, keep it at beat 1 (index 0) and greyed out
    if (!gridAvailableA) { 
        setBeatPositionDeckA(0.0); 
        return; 
    }
    
    // Base BPM from analysis; fall back to global grid if unknown
    double baseBpm = (bpmA > 0.0) ? bpmA : GlobalBeatGrid::getInstance().getCurrentBpm();
    if (baseBpm <= 0.0) {
        setBeatPositionDeckA(0.0);
        return;
    }

    // Calculate beat period from the analyzed BPM. The transport position we
    // receive already reflects any tempo changes, so we must not apply the
    // tempo factor a second time here.
    const double beatPeriod = 60.0 / baseBpm; // seconds per beat in track time
    
    // SYNC WITH WAVEFORM: Use the same calculation as WaveformDisplay
    // In preroll: beatTime = -beatInterval, -2*beatInterval, etc.
    // We want beat 1 to align with the orange lines (every 4th beat)
    
    double beatPosition;
    if (positionSeconds < 0.0) {
        // In preroll: calculate relative to beat intervals like WaveformDisplay
        beatPosition = positionSeconds / beatPeriod;
    } else {
        // Normal playback: use first beat offset
        beatPosition = (positionSeconds - firstBeatOffsetA) / beatPeriod;
    }
    
    // Convert to 4-beat cycle (0.0 to 4.0)
    // Adjust so beat 1 (index 0) aligns with orange lines
    double beatInCycle = std::fmod(beatPosition, 4.0);
    if (beatInCycle < 0.0) beatInCycle += 4.0;
    
    // Debug output for preroll beat calculation (disabled for performance)
    /*
    if (positionSeconds < 0.0) {
        static int debugCounter = 0;
        if ((debugCounter++ % 30) == 0) { // Print every 30th update to avoid spam
            qDebug() << "BEAT A PREROLL: pos=" << positionSeconds 
                     << "bpm=" << effectiveBpm << "period=" << beatPeriod 
                     << "beatPos=" << beatPosition << "cycle=" << beatInCycle
                     << "offset=" << firstBeatOffsetA;
        }
    }
    */
    
    setBeatPositionDeckA(beatInCycle);
}

void BeatIndicator::setTrackPositionDeckB(double positionSeconds) {
    // If no beat grid is available yet, keep it at beat 1 (index 0) and greyed out
    if (!gridAvailableB) { 
        setBeatPositionDeckB(0.0); 
        return; 
    }
    
    // Base BPM from analysis; fall back to global grid if unknown
    double baseBpm = (bpmB > 0.0) ? bpmB : GlobalBeatGrid::getInstance().getCurrentBpm();
    if (baseBpm <= 0.0) {
        setBeatPositionDeckB(0.0);
        return;
    }

    // Calculate beat period from the analyzed BPM. The transport position we
    // receive already reflects any tempo changes, so we must not apply the
    // tempo factor a second time here.
    const double beatPeriod = 60.0 / baseBpm; // seconds per beat in track time
    
    // SYNC WITH WAVEFORM: Use the same calculation as WaveformDisplay
    // In preroll: beatTime = -beatInterval, -2*beatInterval, etc.
    // We want beat 1 to align with the orange lines (every 4th beat)
    
    double beatPosition;
    if (positionSeconds < 0.0) {
        // In preroll: calculate relative to beat intervals like WaveformDisplay
        beatPosition = positionSeconds / beatPeriod;
    } else {
        // Normal playback: use first beat offset
        beatPosition = (positionSeconds - firstBeatOffsetB) / beatPeriod;
    }
    
    // Convert to 4-beat cycle (0.0 to 4.0)
    // Adjust so beat 1 (index 0) aligns with orange lines
    double beatInCycle = std::fmod(beatPosition, 4.0);
    if (beatInCycle < 0.0) beatInCycle += 4.0;
    
    setBeatPositionDeckB(beatInCycle);
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
    
    // Check if we're on a strong beat (beat 0 = beat 1)
    bool strongBeatA = (currentBeatIndexA == 0);
    bool strongBeatB = (currentBeatIndexB == 0);
    
    // Centered grid: exact content width = 3*BOX_SPACING + BOX_W
    const int contentW = 3 * BOX_SPACING + BOX_W;
    const int startX = (int) std::round((width() - contentW) * 0.5);
    const int topY = (int) std::round((height() - (2 * BOX_H + ROW_GAP)) * 0.5);
    const int bottomY = topY + BOX_H + ROW_GAP;
    
    for (int i = 0; i < 4; ++i) {
        int x = startX + i * BOX_SPACING;
        
        // Top row (Deck A)
        const bool aDisabled = !gridAvailableA;
        
        // Enhanced colors for better preroll visibility
        QColor topActive = aDisabled ? QColor(85, 85, 95) : 
                          (strongBeatA && i == 0) ? QColor(255, 100, 100) :  // Red for beat 1
                          QColor(100, 150, 255);  // Blue for other beats
        QColor topInactive = aDisabled ? QColor(55, 55, 65) : QColor(40, 60, 80);
        QColor topBorder = aDisabled ? QColor(120, 120, 130) : 
                          (strongBeatA && i == 0) ? QColor(255, 150, 150) : 
                          QColor(200, 200, 255);
        QColor topText = aDisabled ? QColor(180, 180, 190) : QColor(255, 255, 255);
        
        QColor topColor = (i == currentBeatIndexA) ? topActive : topInactive;
        
        // Add pulsing effect for active beat
        if (i == currentBeatIndexA && gridAvailableA) {
            double beatProgress = currentBeatA - std::floor(currentBeatA);
            int pulse = (int)(20.0 * (0.5 + 0.5 * std::sin(beatProgress * 2.0 * M_PI)));
            topColor = topColor.lighter(100 + pulse);
        }
        
        p.setBrush(QBrush(topColor));
        p.setPen(QPen(topBorder, 2)); // Thicker border for better visibility
        p.drawRect(x, topY, BOX_W, BOX_H);
        
        // Beat number in top box - always show 1,2,3,4
        p.setPen(QPen(topText, 2));
        p.setFont(QFont("Arial", 8, QFont::Bold));
        p.drawText(x, topY, BOX_W, BOX_H, Qt::AlignCenter, QString::number(i + 1));
        
        // Bottom row (Deck B)
        const bool bDisabled = !gridAvailableB;
        
        // Enhanced colors for better preroll visibility
        QColor bottomActive = bDisabled ? QColor(95, 85, 75) : 
                             (strongBeatB && i == 0) ? QColor(255, 100, 50) :  // Orange for beat 1
                             QColor(255, 150, 50);  // Yellow for other beats
        QColor bottomInactive = bDisabled ? QColor(65, 60, 55) : QColor(80, 50, 20);
        QColor bottomBorder = bDisabled ? QColor(130, 120, 110) : 
                             (strongBeatB && i == 0) ? QColor(255, 150, 100) : 
                             QColor(255, 200, 100);
        QColor bottomText = bDisabled ? QColor(190, 180, 170) : QColor(255, 255, 255);
        
        QColor bottomColor = (i == currentBeatIndexB) ? bottomActive : bottomInactive;
        
        // Add pulsing effect for active beat
        if (i == currentBeatIndexB && gridAvailableB) {
            double beatProgress = currentBeatB - std::floor(currentBeatB);
            int pulse = (int)(20.0 * (0.5 + 0.5 * std::sin(beatProgress * 2.0 * M_PI)));
            bottomColor = bottomColor.lighter(100 + pulse);
        }
        
        p.setBrush(QBrush(bottomColor));
        p.setPen(QPen(bottomBorder, 2)); // Thicker border for better visibility
        p.drawRect(x, bottomY, BOX_W, BOX_H);
        
        // Beat number in bottom box - always show 1,2,3,4
        p.setPen(QPen(bottomText, 2));
        p.drawText(x, bottomY, BOX_W, BOX_H, Qt::AlignCenter, QString::number(i + 1));
    }
    
    // Draw enhanced progress bar showing position within current beat for Deck A
    double beatProgressA = currentBeatA - std::floor(currentBeatA);
    int progressXA = startX + currentBeatIndexA * BOX_SPACING;
    int progressWidthA = (int)(BOX_W * beatProgressA);
    
    // Progress overlay on current beat for Deck A (top row)
    if (gridAvailableA && progressWidthA > 0) {
        QColor progressColorA(255, 255, 255, 160); // More opaque for better visibility
        p.setBrush(QBrush(progressColorA));
        p.setPen(Qt::NoPen);
        p.drawRect(progressXA, topY, progressWidthA, BOX_H);
        
        // Add a thin line at the current position
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.drawLine(progressXA + progressWidthA, topY, progressXA + progressWidthA, topY + BOX_H);
    }
    
    // Draw enhanced progress bar showing position within current beat for Deck B
    double beatProgressB = currentBeatB - std::floor(currentBeatB);
    int progressXB = startX + currentBeatIndexB * BOX_SPACING;
    int progressWidthB = (int)(BOX_W * beatProgressB);
    
    // Progress overlay on current beat for Deck B (bottom row)
    if (gridAvailableB && progressWidthB > 0) {
        QColor progressColorB(255, 255, 255, 160); // More opaque for better visibility
        p.setBrush(QBrush(progressColorB));
        p.setPen(Qt::NoPen);
        p.drawRect(progressXB, bottomY, progressWidthB, BOX_H);
        
        // Add a thin line at the current position
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.drawLine(progressXB + progressWidthB, bottomY, progressXB + progressWidthB, bottomY + BOX_H);
    }
}
