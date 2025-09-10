#pragma once

#include <QWidget>
#include <QTimer>

class QtTurntableWidget : public QWidget {
    Q_OBJECT
public:
    explicit QtTurntableWidget(QWidget* parent = nullptr);
    void start();
    void stop();
    void setSpeed(double ratio); // 1.0 = normal
    void setBpm(double bpm);
    void setPlayheadPosition(double position); // 0.0 to 1.0 through track
    void setTrackLength(double lengthInSeconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QTimer timer;
    double angle{0.0};
    double speed{1.0};
    double bpm{120.0};
    double playheadPosition{0.0}; // Current position in track (0.0 to 1.0)
    double trackLengthSeconds{0.0};
    bool syncToBeats{true};
    
    // Performance optimization: Cache rendered elements
    mutable QPixmap cachedBackground;
    mutable bool backgroundDirty{true};
    
    void updateBackgroundCache() const;
    void updateRotationFromPosition();
    
private slots:
    void tick();
};
