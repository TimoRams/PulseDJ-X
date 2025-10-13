#pragma once

#include <QObject>
#include <chrono>
#include <limits>

class ScratchEngine : public QObject {
    Q_OBJECT
public:
    enum class Controller { JogWheel, Waveform, Hardware, External };

    struct TrackConfig {
        double lengthSeconds{0.0};
        double prerollSeconds{8.0};
    };

    struct UpdateResult {
        double seconds{0.0};
        double relative{0.0};
        double velocity{0.0};
        double rawDeltaSeconds{0.0};
        bool clamped{false};
    };

    explicit ScratchEngine(QObject* parent = nullptr);

    void setTrackConfig(const TrackConfig& config);
    TrackConfig trackConfig() const { return config_; }

    void syncExternalPosition(double seconds);
    double currentSeconds() const { return currentSeconds_; }
    bool isScratching() const { return scratching_; }
    Controller activeController() const { return activeController_; }

    UpdateResult beginScratch(Controller controller, double initialSeconds);
    UpdateResult updateByUnits(Controller controller, double deltaUnits, double secondsPerUnit, double deltaTimeSeconds);
    UpdateResult moveToSeconds(Controller controller, double targetSeconds, double deltaTimeSeconds);
    UpdateResult endScratch(Controller controller, double releaseVelocityHint = std::numeric_limits<double>::quiet_NaN());

signals:
    void scratchStarted(ScratchEngine::Controller controller);
    void positionChanged(double seconds, double relative);
    void velocityChanged(double velocity);
    void scratchEnded(double releaseVelocity);

private:
    UpdateResult applyNewSeconds(double candidateSeconds, double rawDeltaSeconds, double deltaTimeSeconds);
    double clampSeconds(double seconds) const;
    double secondsToRelative(double seconds) const;
    void resetSmoothing(double initialVelocity = 0.0);
    void broadcastPosition(const UpdateResult& result);
    void broadcastVelocity(double velocity);

    TrackConfig config_{};
    bool scratching_{false};
    Controller activeController_{Controller::External};
    double currentSeconds_{0.0};
    double lastBroadcastSeconds_{std::numeric_limits<double>::quiet_NaN()};
    double lastVelocity_{0.0};
    double smoothedVelocity_{0.0};
    double lastBroadcastVelocity_{0.0};
    bool hasBroadcastVelocity_{false};
    std::chrono::steady_clock::time_point lastUpdate_{};
    double velocitySmoothing_{0.35};
    double jogWheelSmoothing_{0.08};
    double jogWheelSnapThreshold_{0.02};
    double maxVelocity_{144.0};
    double epsilon_{1e-6};
};
