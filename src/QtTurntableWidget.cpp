#include "QtTurntableWidget.h"
#include <QPainter>
#include <cmath>

QtTurntableWidget::QtTurntableWidget(QWidget* parent) : QWidget(parent) {
    connect(&timer, &QTimer::timeout, this, &QtTurntableWidget::tick);
    timer.setInterval(16); // ~60 FPS for smooth animation
    setMinimumSize(100, 100);
}

void QtTurntableWidget::start() { 
    timer.start(); 
}

void QtTurntableWidget::stop() { 
    timer.stop(); 
}

void QtTurntableWidget::setSpeed(double ratio) { 
    speed = ratio; 
}

void QtTurntableWidget::setBpm(double newBpm) {
    if (newBpm > 0.0) {
        bpm = newBpm;
    }
}

void QtTurntableWidget::setPlayheadPosition(double position) {
    playheadPosition = std::clamp(position, 0.0, 1.0);
    updateRotationFromPosition();
}

void QtTurntableWidget::setTrackLength(double lengthInSeconds) {
    trackLengthSeconds = lengthInSeconds;
}

void QtTurntableWidget::updateRotationFromPosition() {
    if (!syncToBeats || bpm <= 0.0 || trackLengthSeconds <= 0.0) {
        return;
    }
    
    // Calculate current time in track
    double currentTimeSeconds = playheadPosition * trackLengthSeconds;
    
    // Calculate beats per second
    double beatsPerSecond = bpm / 60.0;
    
    // Calculate current beat position
    double currentBeat = currentTimeSeconds * beatsPerSecond;
    
    // Each bar = 4 beats, and we want 25% rotation (90°) per bar
    // So each beat = 25% / 4 = 6.25% rotation = 22.5° = π/8 radians
    double radiansPerBeat = M_PI / 8.0; // 22.5 degrees per beat
    
    // Calculate target angle (red beats should be at "up" position = 0 radians)
    // Bar start (red beat) positions: beat 0, 4, 8, 12, etc.
    angle = currentBeat * radiansPerBeat;
    
    // Normalize angle to [0, 2π)
    while (angle >= 2.0 * M_PI) angle -= 2.0 * M_PI;
    while (angle < 0.0) angle += 2.0 * M_PI;
}

void QtTurntableWidget::resizeEvent(QResizeEvent* event) {
    backgroundDirty = true;
    QWidget::resizeEvent(event);
}

void QtTurntableWidget::tick() {
    if (!syncToBeats) {
        // Free-running mode - rotate based on speed
        double rotationSpeed = (2.0 * M_PI / 60.0) * speed; // radians per second at 60 FPS
        angle += rotationSpeed / 60.0;
        if (angle > 2.0 * M_PI) angle -= 2.0 * M_PI;
    }
    // In sync mode, rotation is handled by updateRotationFromPosition()
    update();
}

void QtTurntableWidget::updateBackgroundCache() const {
    if (!backgroundDirty) return;
    
    const int size = std::min(width(), height());
    if (size <= 0) return;
    
    cachedBackground = QPixmap(size, size);
    cachedBackground.fill(Qt::transparent);
    
    QPainter p(&cachedBackground);
    p.setRenderHint(QPainter::Antialiasing, true);
    
    const QRectF rect(0, 0, size, size);
    const QPointF center = rect.center();
    const double radius = size * 0.4;
    
    // Simple dark background circle
    QRadialGradient baseGradient(center, radius * 1.2);
    baseGradient.setColorAt(0.0, QColor(40, 40, 45));
    baseGradient.setColorAt(0.8, QColor(25, 25, 30));
    baseGradient.setColorAt(1.0, QColor(15, 15, 20));
    
    p.setBrush(baseGradient);
    p.setPen(QPen(QColor(20, 20, 25), 1));
    p.drawEllipse(center, radius * 1.2, radius * 1.2);
    
    backgroundDirty = false;
}

void QtTurntableWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    
    // Update background cache if needed
    updateBackgroundCache();
    
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    
    const int size = std::min(width(), height());
    const QRectF rect((width() - size) / 2.0, (height() - size) / 2.0, size, size);
    const QPointF center = rect.center();
    const double radius = size * 0.4;
    
    // Draw cached background (non-rotating platter)
    if (!cachedBackground.isNull()) {
        const QRectF bgRect((width() - size) / 2.0, (height() - size) / 2.0, size, size);
        p.drawPixmap(bgRect.toRect(), cachedBackground);
    }
    
    // Draw rotating circle with cutout synchronized to beats
    p.save();
    p.translate(center);
    p.rotate(angle * 180.0 / M_PI);
    
    // Main circle outline with BEAT-SYNCHRONIZED GAP
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255), 3)); // White circle, thick line
    
    // Calculate gap position based on beat sync
    // Gap should appear at "up" position (12 o'clock) on red beats
    // Gap spans 20 degrees (10 degrees on each side of the beat position)
    int gapSize = 20 * 16;  // 20 degrees in Qt's 16ths of a degree
    int gapStart = (-10) * 16;  // Start 10 degrees before "up" position
    int circleSpan = (360 - 20) * 16;  // Full circle minus the gap
    
    // Draw the circle with the beat-synchronized gap
    p.drawArc(QRectF(-radius, -radius, radius * 2, radius * 2), gapStart + gapSize, circleSpan);
    
    p.restore();
    
    // Draw center point (non-rotating)
    p.setBrush(QBrush(QColor(200, 200, 200)));
    p.setPen(QPen(QColor(150, 150, 150), 1));
    const double centerRadius = 4;
    p.drawEllipse(center, centerRadius, centerRadius);
    
    // Status indicator - show when playing
    if (timer.isActive()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 255, 0, 180), 2)); // Green outline when playing
        const double indicatorRadius = radius * 1.1;
        p.drawEllipse(center, indicatorRadius, indicatorRadius);
    }
}
