#pragma once

#include <QWidget>
#include <QTimer>
#include <chrono>

#include "ScratchEngine.h"

class QPointF;

class QtTurntableWidget : public QWidget {
    Q_OBJECT
public:
    explicit QtTurntableWidget(QWidget* parent = nullptr);
    void start();
    void stop();
    void setSpeed(double ratio); // 1.0 = normal
    void setBpm(double bpm);
    void setPlayheadPosition(double position); // 0.0 to 1.0 through track
    void setPositionSeconds(double seconds); // absolute time in seconds (can be negative for preroll)
    void setTrackLength(double lengthInSeconds);
    void setScratchEngine(ScratchEngine* engine) { scratchEngine = engine; }
    ScratchEngine* getScratchEngine() const { return scratchEngine; }
    double getLastScratchVelocity() const { return scratchLastVelocity; }
    void setPrerollSeconds(double seconds) { prerollSeconds = std::max(0.0, seconds); }
    double getPrerollSeconds() const { return prerollSeconds; }
    double getTrackLengthSeconds() const { return trackLengthSeconds; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

signals:
    void scratchStart();
    void scratchMove(double relativePosition);
    void scratchVelocityChanged(double velocity);
    void scratchEnd(double releaseVelocity);

private:
    QTimer timer;
    double angle{0.0};
    double speed{1.0};
    double bpm{120.0};
    double playheadPosition{0.0}; // Current position in track (0.0 to 1.0)
    double trackLengthSeconds{0.0};
    double currentTimeSeconds{0.0}; // absolute time in seconds (supports preroll)
    ScratchEngine* scratchEngine{nullptr};
    bool scratching{false};
    bool platterGrabbed{false};
    bool ignoreExternalPositionUpdate{false};
    double lastPointerAngle{0.0};
    double scratchLastVelocity{0.0};
    std::chrono::steady_clock::time_point lastScratchTimestamp;
    double prerollSeconds{8.0};
    double lastScratchSeconds{0.0};
    static constexpr double baseRpm{33.3333333333};
    static constexpr double pi{3.14159265358979323846};
    static constexpr double radiansPerSecond{baseRpm * 2.0 * pi / 60.0};
    
    // Performance optimization: Cache rendered elements
    mutable QPixmap cachedBackground;
    mutable bool backgroundDirty{true};
    
    void updateBackgroundCache() const;
    void updateRotationFromPosition();
    double relativeFromSeconds(double seconds) const;
    double clampToTrack(double seconds) const;
    double pointerAngleForPos(const QPointF& pos) const;
    static double secondsToAngle(double seconds);
    void applyScratchResult(const ScratchEngine::UpdateResult& result);
    
private slots:
    void tick();
};
