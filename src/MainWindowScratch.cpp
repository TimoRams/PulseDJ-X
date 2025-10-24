#include "MainWindow.h"

#include "DJAudioPlayer.h"
#include "DeckWidget.h"
#include "WaveformDisplay.h"
#include "BeatIndicator.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>
#include <cmath>

void QtMainWindow::applyScratchPosition(bool isDeckA, double absRel)
{
    DJAudioPlayer* player = playerForDeck(isDeckA);
    QtDeckWidget* deck = isDeckA ? deckA : deckB;
    WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB;
    if (!player || !overview)
        return;

    double minRel = overview->isPrerollEnabled() ? -1.0 : 0.0;
    absRel = std::clamp(absRel, minRel, 1.0);

    player->setPositionRelative(absRel);
    overview->setPlayhead(absRel);

    if (deck && deck->getWaveform())
        deck->getWaveform()->setPlayhead(absRel);

    if (beatIndicator && deck)
    {
        double lenSec = std::max(1e-9, player->getLengthInSeconds());
        constexpr double prerollSec = 8.0;
        double seconds = (absRel < 0.0) ? (absRel * prerollSec) : (absRel * lenSec);
        if (isDeckA)
        {
            beatIndicator->setTrackPositionDeckA(seconds);
            deck->setPlatterSeconds(seconds);
        }
        else
        {
            beatIndicator->setTrackPositionDeckB(seconds);
            deck->setPlatterSeconds(seconds);
        }
    }
}

void QtMainWindow::handleScratchStart(bool isDeckA)
{
    DJAudioPlayer* player = playerForDeck(isDeckA);
    if (!player)
        return;

    bool& inertiaActive = isDeckA ? scratchInertiaActiveA : scratchInertiaActiveB;
    bool& inertiaResume = isDeckA ? scratchInertiaResumeA : scratchInertiaResumeB;

    if (inertiaActive)
        stopScratchInertia(isDeckA, inertiaResume);

    const bool wasAudible = player->isAudible();
    if (isDeckA)
        scratchWasPlayingA = wasAudible;
    else
        scratchWasPlayingB = wasAudible;

    player->setScratchPlaybackContext(wasAudible);
    player->enableScratch(true);

    if (!wasAudible)
        player->ensureScratchAudible();
}

void QtMainWindow::handleScratchVelocityChanged(bool isDeckA, double velocity)
{
    DJAudioPlayer* player = playerForDeck(isDeckA);
    if (!player)
        return;
    player->setScratchVelocity(velocity);
}

void QtMainWindow::handleScratchEnd(bool isDeckA, double releaseVelocity)
{
    DJAudioPlayer* player = playerForDeck(isDeckA);
    if (!player)
        return;

    if (!std::isfinite(releaseVelocity))
        releaseVelocity = 0.0;

    qint64& lastScratchEnd = isDeckA ? lastScratchEndA : lastScratchEndB;
    bool& scratchWasPlaying = isDeckA ? scratchWasPlayingA : scratchWasPlayingB;
    bool& scratchInertiaResume = isDeckA ? scratchInertiaResumeA : scratchInertiaResumeB;

    lastScratchEnd = QDateTime::currentMSecsSinceEpoch();
    player->setScratchVelocity(0.0);

    if (scratchWasPlaying)
    {
        player->enableScratch(false);
        if (!player->isAudible())
            player->ensureScratchAudible();
        scratchInertiaResume = true;
    }
    else
    {
        scratchInertiaResume = false;
        startScratchInertia(isDeckA, releaseVelocity, false);
    }
}

void QtMainWindow::startScratchInertia(bool isDeckA, double initialVelocity, bool resumePlayback)
{
    DJAudioPlayer* player = playerForDeck(isDeckA);
    WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB;
    if (!player || !overview)
        return;

    auto& timer = isDeckA ? scratchInertiaTimerA : scratchInertiaTimerB;
    auto& velocity = isDeckA ? scratchInertiaVelocityA : scratchInertiaVelocityB;
    auto& elapsed = isDeckA ? scratchInertiaElapsedA : scratchInertiaElapsedB;
    auto& active = isDeckA ? scratchInertiaActiveA : scratchInertiaActiveB;
    auto& resume = isDeckA ? scratchInertiaResumeA : scratchInertiaResumeB;

    resume = resumePlayback;

    if (std::abs(initialVelocity) < 0.05)
    {
        stopScratchInertia(isDeckA, resumePlayback);
        return;
    }

    if (!timer)
    {
        timer = new QTimer(this);
        timer->setInterval(16);
        connect(timer, &QTimer::timeout, this, [this, isDeckA]() {
            handleScratchInertiaTick(isDeckA);
        });
    }

    velocity = initialVelocity;
    elapsed = 0.0;
    active = true;

    player->ensureScratchAudible();
    player->enableScratch(true);

    timer->start();
}

void QtMainWindow::stopScratchInertia(bool isDeckA, bool resumePlayback)
{
    DJAudioPlayer* player = playerForDeck(isDeckA);
    auto& timer = isDeckA ? scratchInertiaTimerA : scratchInertiaTimerB;
    auto& active = isDeckA ? scratchInertiaActiveA : scratchInertiaActiveB;
    auto& velocity = isDeckA ? scratchInertiaVelocityA : scratchInertiaVelocityB;

    if (timer)
        timer->stop();
    active = false;
    velocity = 0.0;

    if (!player)
        return;

    player->enableScratch(false);

    if (resumePlayback)
        player->ensureScratchAudible();
    else
        player->stop();
}

void QtMainWindow::handleScratchInertiaTick(bool isDeckA)
{
    DJAudioPlayer* player = playerForDeck(isDeckA);
    WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB;
    auto& timer = isDeckA ? scratchInertiaTimerA : scratchInertiaTimerB;
    auto& velocity = isDeckA ? scratchInertiaVelocityA : scratchInertiaVelocityB;
    auto& elapsed = isDeckA ? scratchInertiaElapsedA : scratchInertiaElapsedB;
    auto& active = isDeckA ? scratchInertiaActiveA : scratchInertiaActiveB;
    auto& resume = isDeckA ? scratchInertiaResumeA : scratchInertiaResumeB;

    if (!player || !overview || !timer || !active)
        return;

    double dt = timer->interval() / 1000.0;
    elapsed += dt;

    const double friction = 0.88;
    velocity *= friction;

    const double maxDuration = resume ? 0.35 : 0.6;
    if (elapsed > maxDuration)
        velocity = 0.0;

    double totalLength = player->getLengthInSeconds();
    if (totalLength <= 0.0 && overview->trackLengthSec > 0.0)
        totalLength = overview->trackLengthSec;

    double currentRel = overview->getPlayheadRelative();
    double prerollSec = overview->getPrerollTimeSeconds();
    double currentSec = (currentRel < 0.0) ? (currentRel * prerollSec) : (currentRel * totalLength);
    double deltaSec = velocity * dt;
    double newSec = currentSec + deltaSec;

    double minSec = -prerollSec;
    double maxSec = totalLength;
    newSec = std::clamp(newSec, minSec, maxSec);

    double newRel;
    if (newSec < 0.0)
        newRel = (prerollSec > 1e-6) ? (newSec / prerollSec) : currentRel;
    else if (totalLength > 1e-6)
        newRel = newSec / totalLength;
    else
        newRel = currentRel;

    applyScratchPosition(isDeckA, newRel);

    if (std::abs(velocity) < 0.015)
        stopScratchInertia(isDeckA, resume);
}
