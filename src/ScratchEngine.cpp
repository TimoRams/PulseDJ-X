#include "ScratchEngine.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kDefaultFrameSeconds = 1.0 / 60.0;
}

ScratchEngine::ScratchEngine(QObject* parent)
    : QObject(parent)
{
    lastUpdate_ = std::chrono::steady_clock::now();
}

void ScratchEngine::setTrackConfig(const TrackConfig& config) {
    TrackConfig sanitized;
    sanitized.lengthSeconds = std::max(0.0, config.lengthSeconds);
    sanitized.prerollSeconds = std::max(0.0, config.prerollSeconds);
    if (std::abs(sanitized.lengthSeconds - config_.lengthSeconds) < epsilon_ &&
        std::abs(sanitized.prerollSeconds - config_.prerollSeconds) < epsilon_) {
        config_ = sanitized;
        return;
    }
    config_ = sanitized;
    currentSeconds_ = clampSeconds(currentSeconds_);
    lastBroadcastSeconds_ = currentSeconds_;
}

void ScratchEngine::syncExternalPosition(double seconds) {
    if (scratching_) {
        return;
    }
    double clamped = clampSeconds(seconds);
    if (std::isnan(lastBroadcastSeconds_) || std::abs(lastBroadcastSeconds_ - clamped) > 1e-4) {
        currentSeconds_ = clamped;
        lastBroadcastSeconds_ = clamped;
        lastUpdate_ = std::chrono::steady_clock::now();
        resetSmoothing(0.0);
    } else {
        currentSeconds_ = clamped;
    }
}

ScratchEngine::UpdateResult ScratchEngine::beginScratch(Controller controller, double initialSeconds) {
    scratching_ = true;
    activeController_ = controller;
    currentSeconds_ = clampSeconds(initialSeconds);
    lastUpdate_ = std::chrono::steady_clock::now();
    resetSmoothing(0.0);

    UpdateResult result;
    result.seconds = currentSeconds_;
    result.relative = secondsToRelative(currentSeconds_);
    result.velocity = 0.0;
    result.rawDeltaSeconds = 0.0;
    result.clamped = false;

    broadcastPosition(result);
    broadcastVelocity(0.0);
    emit scratchStarted(controller);
    return result;
}

ScratchEngine::UpdateResult ScratchEngine::updateByUnits(Controller controller, double deltaUnits, double secondsPerUnit, double deltaTimeSeconds) {
    if (!scratching_ || controller != activeController_) {
        return UpdateResult{};
    }

    double dt = deltaTimeSeconds;
    if (!(dt > epsilon_)) {
        auto now = std::chrono::steady_clock::now();
        dt = std::chrono::duration<double>(now - lastUpdate_).count();
        if (!(dt > epsilon_)) {
            dt = kDefaultFrameSeconds;
        }
    }

    double deltaSeconds = deltaUnits * secondsPerUnit;
    double controllerMaxVelocity = maxVelocity_;
    double maxDelta = controllerMaxVelocity * dt;
    if (!(maxDelta > epsilon_)) {
        maxDelta = controllerMaxVelocity * kDefaultFrameSeconds;
    }
    deltaSeconds = std::clamp(deltaSeconds, -maxDelta, maxDelta);

    return applyNewSeconds(currentSeconds_ + deltaSeconds, deltaSeconds, dt);
}

ScratchEngine::UpdateResult ScratchEngine::moveToSeconds(Controller controller, double targetSeconds, double deltaTimeSeconds) {
    if (!scratching_ || controller != activeController_) {
        return UpdateResult{};
    }

    double dt = deltaTimeSeconds;
    if (!(dt > epsilon_)) {
        auto now = std::chrono::steady_clock::now();
        dt = std::chrono::duration<double>(now - lastUpdate_).count();
        if (!(dt > epsilon_)) {
            dt = kDefaultFrameSeconds;
        }
    }

    double candidate = targetSeconds;
    double clamped = clampSeconds(candidate);
    double rawDelta = clamped - currentSeconds_;
    double maxDelta = maxVelocity_ * dt;
    if (!(maxDelta > epsilon_)) {
        maxDelta = maxVelocity_ * kDefaultFrameSeconds;
    }
    if (rawDelta > maxDelta) {
        rawDelta = maxDelta;
        clamped = currentSeconds_ + rawDelta;
    } else if (rawDelta < -maxDelta) {
        rawDelta = -maxDelta;
        clamped = currentSeconds_ + rawDelta;
    }

    return applyNewSeconds(clamped, rawDelta, dt);
}

ScratchEngine::UpdateResult ScratchEngine::endScratch(Controller controller, double releaseVelocityHint) {
    UpdateResult result;
    if (!scratching_ || controller != activeController_) {
        result.seconds = currentSeconds_;
        result.relative = secondsToRelative(currentSeconds_);
        result.velocity = std::isfinite(releaseVelocityHint) ? releaseVelocityHint : lastVelocity_;
        result.rawDeltaSeconds = 0.0;
        result.clamped = false;
        return result;
    }

    scratching_ = false;
    activeController_ = Controller::External;

    double releaseVelocity = std::isfinite(releaseVelocityHint) ? releaseVelocityHint : lastVelocity_;
    result.seconds = currentSeconds_;
    result.relative = secondsToRelative(currentSeconds_);
    result.velocity = releaseVelocity;
    result.rawDeltaSeconds = 0.0;
    result.clamped = false;

    broadcastVelocity(0.0);
    emit scratchEnded(releaseVelocity);
    resetSmoothing(0.0);
    lastUpdate_ = std::chrono::steady_clock::now();
    return result;
}

ScratchEngine::UpdateResult ScratchEngine::applyNewSeconds(double candidateSeconds, double rawDeltaSeconds, double deltaTimeSeconds) {
    double clampedSeconds = clampSeconds(candidateSeconds);
    double appliedDelta = clampedSeconds - currentSeconds_;

    double dt = std::max(deltaTimeSeconds, epsilon_);
    double targetVelocity = appliedDelta / dt;
    targetVelocity = std::clamp(targetVelocity, -maxVelocity_, maxVelocity_);

    double smoothing = velocitySmoothing_;
    if (activeController_ == Controller::JogWheel) {
        smoothing = jogWheelSmoothing_;
    }

    smoothing = std::clamp(smoothing, 0.0, 0.999);

    smoothedVelocity_ = smoothing * smoothedVelocity_ + (1.0 - smoothing) * targetVelocity;

    if (activeController_ == Controller::JogWheel && std::abs(targetVelocity) < jogWheelSnapThreshold_) {
        if (std::abs(smoothedVelocity_) < jogWheelSnapThreshold_) {
            smoothedVelocity_ = 0.0;
        }
    }

    lastVelocity_ = smoothedVelocity_;

    currentSeconds_ = clampedSeconds;
    lastUpdate_ = std::chrono::steady_clock::now();

    UpdateResult result;
    result.seconds = currentSeconds_;
    result.relative = secondsToRelative(currentSeconds_);
    result.velocity = lastVelocity_;
    result.rawDeltaSeconds = appliedDelta;
    result.clamped = std::abs(candidateSeconds - clampedSeconds) > epsilon_;

    broadcastPosition(result);
    broadcastVelocity(lastVelocity_);

    return result;
}

double ScratchEngine::clampSeconds(double seconds) const {
    double minSeconds = -config_.prerollSeconds;
    double maxSeconds = config_.lengthSeconds;
    if (maxSeconds <= 0.0) {
        return std::clamp(seconds, minSeconds, 0.0);
    }
    return std::clamp(seconds, minSeconds, maxSeconds);
}

double ScratchEngine::secondsToRelative(double seconds) const {
    if (seconds < 0.0 && config_.prerollSeconds > epsilon_) {
        return std::clamp(seconds / config_.prerollSeconds, -1.0, 0.0);
    }
    if (config_.lengthSeconds > epsilon_) {
        double rel = seconds / config_.lengthSeconds;
        return std::clamp(rel, 0.0, 1.0);
    }
    return 0.0;
}

void ScratchEngine::resetSmoothing(double initialVelocity) {
    smoothedVelocity_ = initialVelocity;
    lastVelocity_ = initialVelocity;
    hasBroadcastVelocity_ = false;
}

void ScratchEngine::broadcastPosition(const UpdateResult& result) {
    if (std::isnan(result.seconds)) {
        return;
    }
    if (std::isnan(lastBroadcastSeconds_) || std::abs(lastBroadcastSeconds_ - result.seconds) > 1e-5) {
        lastBroadcastSeconds_ = result.seconds;
        emit positionChanged(result.seconds, result.relative);
    }
}

void ScratchEngine::broadcastVelocity(double velocity) {
    if (!hasBroadcastVelocity_ || std::abs(lastBroadcastVelocity_ - velocity) > 0.01) {
        lastBroadcastVelocity_ = velocity;
        hasBroadcastVelocity_ = true;
        emit velocityChanged(velocity);
    }
}
