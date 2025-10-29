#include "TurntableWidget.h"
#include "FrameTiming.h"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kSecondsPerFrame(int intervalMs) {
    return intervalMs / 1000.0;
}
}

QtTurntableWidget::QtTurntableWidget(QWidget* parent) : QWidget(parent) {
    connect(&timer, &QTimer::timeout, this, &QtTurntableWidget::tick);
    timer.setInterval(FrameTiming::kFrameIntervalMs);
    setMinimumSize(100, 100);
    setMouseTracking(true);
}

void QtTurntableWidget::start() {
    if (!timer.isActive()) {
        timer.start();
    }
}

void QtTurntableWidget::stop() {
    if (timer.isActive()) {
        timer.stop();
    }
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
    if (trackLengthSeconds > 0.0) {
        setPositionSeconds(playheadPosition * trackLengthSeconds);
    } else {
        updateRotationFromPosition();
    }
}

void QtTurntableWidget::setTrackLength(double lengthInSeconds) {
    trackLengthSeconds = std::max(0.0, lengthInSeconds);
}

void QtTurntableWidget::setPositionSeconds(double seconds) {
    if (ignoreExternalPositionUpdate) {
        return;
    }
    currentTimeSeconds = seconds;
    lastScratchSeconds = seconds;
    updateRotationFromPosition();
    if (scratchEngine && !scratchEngine->isScratching()) {
        scratchEngine->syncExternalPosition(seconds);
    }
    update();
}

void QtTurntableWidget::resizeEvent(QResizeEvent* event) {
    backgroundDirty = true;
    QWidget::resizeEvent(event);
}

void QtTurntableWidget::tick() {
    if (!scratching) {
        const double frameSeconds = kSecondsPerFrame(timer.interval());
        const double delta = radiansPerSecond * speed * frameSeconds;
        angle += delta;
    }
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

    // Visual feedback when platter is grabbed for scratching
    if (platterGrabbed) {
        p.setBrush(QColor(80, 120, 255, 40));
        p.setPen(Qt::NoPen);
        p.drawEllipse(center, radius * 1.25, radius * 1.25);
    }

    // Draw rotating circle with beat marker
    p.save();
    p.translate(center);
    double angleDegrees = std::fmod(angle * 180.0 / pi, 360.0);
    if (angleDegrees < 0.0) {
        angleDegrees += 360.0;
    }
    p.rotate(angleDegrees);

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255), 3));

    int gapSize = 20 * 16;
    int gapStart = (-10) * 16;
    int circleSpan = (360 - 20) * 16;
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
        p.setPen(QPen(QColor(0, 255, 0, 180), 2));
        const double indicatorRadius = radius * 1.1;
        p.drawEllipse(center, indicatorRadius, indicatorRadius);
    }
}

void QtTurntableWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (trackLengthSeconds <= 0.0 && prerollSeconds <= 0.0) {
        event->ignore();
        return;
    }

    const QPointF pos = event->position();
    const QPointF center = QPointF(width() / 2.0, height() / 2.0);
    double dx = pos.x() - center.x();
    double dy = pos.y() - center.y();
    double distSq = dx * dx + dy * dy;
    const double size = std::min(width(), height());
    const double radius = size * 0.45;
    if (distSq > radius * radius * 1.2) {
        event->ignore();
        return;
    }

    ignoreExternalPositionUpdate = true;
    scratching = true;
    platterGrabbed = true;
    lastPointerAngle = pointerAngleForPos(pos);
    lastScratchSeconds = currentTimeSeconds;
    scratchLastVelocity = 0.0;
    lastScratchTimestamp = std::chrono::steady_clock::now();
    if (scratchEngine) {
        scratchEngine->beginScratch(ScratchEngine::Controller::JogWheel, currentTimeSeconds);
    } else {
        emit scratchStart();
    }
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void QtTurntableWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!scratching) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const double newAngle = pointerAngleForPos(event->position());
    double delta = newAngle - lastPointerAngle;

    const double twoPi = 2.0 * pi;
    if (delta > pi) {
        delta -= twoPi;
    } else if (delta < -pi) {
        delta += twoPi;
    }

    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastScratchTimestamp).count();
    if (dt <= 0.0001) {
        dt = 0.0001;
    }
    if (scratchEngine) {
        const double secondsPerUnit = 1.0 / radiansPerSecond;
        const auto result = scratchEngine->updateByUnits(
            ScratchEngine::Controller::JogWheel,
            delta,
            secondsPerUnit,
            dt);

        applyScratchResult(result);
        scratchLastVelocity = result.velocity;
    } else {
        const double deltaSeconds = delta / radiansPerSecond;
        const double candidateSeconds = clampToTrack(lastScratchSeconds + deltaSeconds);

        scratchLastVelocity = (candidateSeconds - lastScratchSeconds) / dt;
        lastScratchSeconds = candidateSeconds;
        currentTimeSeconds = candidateSeconds;
        angle = secondsToAngle(currentTimeSeconds);

        emit scratchMove(relativeFromSeconds(candidateSeconds));
        emit scratchVelocityChanged(scratchLastVelocity);
    }

    lastPointerAngle = newAngle;
    lastScratchTimestamp = now;
    update();
    event->accept();
}

void QtTurntableWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && scratching) {
        scratching = false;
        platterGrabbed = false;
        ignoreExternalPositionUpdate = false;
        if (scratchEngine) {
            const auto result = scratchEngine->endScratch(ScratchEngine::Controller::JogWheel, scratchLastVelocity);
            scratchLastVelocity = result.velocity;
        } else {
            emit scratchEnd(scratchLastVelocity);
            scratchLastVelocity = 0.0;
            emit scratchVelocityChanged(0.0);
        }
        setCursor(Qt::ArrowCursor);
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void QtTurntableWidget::applyScratchResult(const ScratchEngine::UpdateResult& result) {
    if (!std::isfinite(result.seconds)) {
        return;
    }
    currentTimeSeconds = clampToTrack(result.seconds);
    lastScratchSeconds = currentTimeSeconds;
    angle = secondsToAngle(currentTimeSeconds);
}

void QtTurntableWidget::updateRotationFromPosition() {
    if (!std::isfinite(currentTimeSeconds)) {
        currentTimeSeconds = 0.0;
    }
    angle = secondsToAngle(currentTimeSeconds);
}

double QtTurntableWidget::relativeFromSeconds(double seconds) const {
    if (seconds < 0.0 && prerollSeconds > 1e-6) {
        return std::clamp(seconds / prerollSeconds, -1.0, 0.0);
    }
    if (trackLengthSeconds > 1e-6) {
        double rel = seconds / trackLengthSeconds;
        return std::clamp(rel, 0.0, 1.0);
    }
    return 0.0;
}

double QtTurntableWidget::clampToTrack(double seconds) const {
    const double minSeconds = -prerollSeconds;
    if (trackLengthSeconds > 0.0) {
        return std::clamp(seconds, minSeconds, trackLengthSeconds);
    }
    return std::max(minSeconds, seconds);
}

double QtTurntableWidget::pointerAngleForPos(const QPointF& pos) const {
    const int size = std::min(width(), height());
    const QRectF rect((width() - size) / 2.0, (height() - size) / 2.0, size, size);
    QPointF center = rect.center();
    double angleRad = std::atan2(pos.y() - center.y(), pos.x() - center.x());
    return angleRad;
}

double QtTurntableWidget::secondsToAngle(double seconds) {
    return seconds * radiansPerSecond;
}
