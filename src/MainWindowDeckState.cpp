#include "MainWindow.h"

#include "BpmAnalyzer.h"
#include "DeckWaveformOverview.h"
#include "DJAudioPlayer.h"
#include "MainWindowTasks.h"
#include "WaveformDisplay.h"

#include <QFileInfo>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

// Deck and library state helpers extracted from the original MainWindow implementation.

void QtMainWindow::onLibraryLoadToDeck(int deckIndex, const QString& filePath)
{
	if (deckIndex == 1 && deckA)
	{
		deckA->loadFile(filePath);
	}
	else if (deckIndex == 2 && deckB)
	{
		deckB->loadFile(filePath);
	}
}

void QtMainWindow::applyStoredCuePoints(QtDeckWidget* deck, bool isDeckA)
{
	if (!deck)
		return;

	auto* pads = deck->getPerformancePads();
	if (!pads)
		return;

	DeckWaveformOverview* deckWaveform = deck->getWaveform();
	WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB;

	if (!libraryManager)
	{
		pads->clearAllCuePoints(false);
		if (deckWaveform)
			deckWaveform->clearCuePoints();
		if (overview)
			overview->clearCuePoints();
		return;
	}

	const QString filePath = deck->getCurrentFilePath();
	if (filePath.isEmpty())
	{
		pads->clearAllCuePoints(false);
		if (deckWaveform)
			deckWaveform->clearCuePoints();
		if (overview)
			overview->clearCuePoints();
		return;
	}

	auto storedCues = libraryManager->getCuePointsForTrack(filePath);
	if (storedCues)
	{
		pads->applyCuePoints(*storedCues);
	}
	else
	{
		pads->clearAllCuePoints();
		if (deckWaveform)
			deckWaveform->clearCuePoints();
		if (overview)
			overview->clearCuePoints();
	}
}

void QtMainWindow::applyStoredBeatGrid(QtDeckWidget* deck, bool isDeckA)
{
	if (!deck || !libraryManager)
		return;

	const QString filePath = deck->getCurrentFilePath();
	if (filePath.isEmpty())
		return;

	auto trackInfo = libraryManager->getTrackInfo(filePath);
	if (!trackInfo)
		return;

	const TrackInfo& track = *trackInfo;
	const bool hasBpm = track.bpm > 0.0;
	const bool hasBeats = !track.beatPositions.isEmpty();
	const bool hasAnalysisData = hasBpm || hasBeats;
	if (!hasAnalysisData)
		return;

	double trackLength = track.trackLengthSeconds > 0.0 ? track.trackLengthSeconds : track.duration;
	if (trackLength <= 0.0)
	{
		if (auto* player = playerForDeck(isDeckA))
			trackLength = player->getLengthInSeconds();
	}

	if (trackLength <= 0.0)
		return;

	QVector<double> relativeBeats;
	if (hasBeats)
	{
		relativeBeats.reserve(track.beatPositions.size());
		for (double beatSec : track.beatPositions)
		{
			const double rel = beatSec / trackLength;
			relativeBeats.append(std::clamp(rel, 0.0, 1.0));
		}
	}

	if (auto* deckWaveform = deck->getWaveform())
		deckWaveform->setBeatInfo(track.bpm, track.firstBeatOffset, trackLength);

	deck->setDetectedBpm(track.bpm);

	if (auto* player = playerForDeck(isDeckA))
		player->setBeatInfo(track.bpm, track.firstBeatOffset, trackLength);

	if (WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB)
	{
		overview->setOriginalBpm(track.bpm, trackLength);
		overview->setBeatInfo(track.bpm, track.firstBeatOffset, trackLength);
		overview->setAnalysisActive(false);
		overview->setAnalysisFailed(track.analysisFailed);
		overview->setAnalysisProgress(hasAnalysisData ? 1.0 : 0.0);
		if (!relativeBeats.isEmpty())
			overview->setBeats(relativeBeats);
		else
			overview->refreshBeatGrid();
	}

	if (beatIndicator)
	{
		if (isDeckA)
		{
			beatIndicator->setBpmDeckA(track.bpm);
			beatIndicator->setFirstBeatOffsetDeckA(track.firstBeatOffset);
			beatIndicator->setBeatGridAvailableDeckA(hasBeats || hasBpm);
		}
		else
		{
			beatIndicator->setBpmDeckB(track.bpm);
			beatIndicator->setFirstBeatOffsetDeckB(track.firstBeatOffset);
			beatIndicator->setBeatGridAvailableDeckB(hasBeats || hasBpm);
		}
	}

	if (isDeckA)
	{
		algorithmA = track.analysisAlgorithm;
		analysisActiveA = false;
		analysisFailedA = track.analysisFailed;
		analysisProgressA = hasAnalysisData ? 1.0 : 0.0;
	}
	else
	{
		algorithmB = track.analysisAlgorithm;
		analysisActiveB = false;
		analysisFailedB = track.analysisFailed;
		analysisProgressB = hasAnalysisData ? 1.0 : 0.0;
	}

	updateOverviewLabel(isDeckA);
}

void QtMainWindow::reapplyStoredDeckMetadata(bool isDeckA)
{
	QtDeckWidget* deck = isDeckA ? deckA : deckB;
	if (!deck)
		return;

	const QString filePath = deck->getCurrentFilePath();
	if (filePath.isEmpty())
		return;

	applyStoredBeatGrid(deck, isDeckA);
	applyStoredCuePoints(deck, isDeckA);
}

void QtMainWindow::onAnalyzeTracksRequested(const QStringList& filePaths)
{
	if (filePaths.isEmpty() || !bpmThreadPool)
		return;

	if (!bpmAnalyzer)
		bpmAnalyzer = std::make_unique<BpmAnalyzer>(*sharedFormatManager);

	for (const QString& path : filePaths)
	{
		juce::File file(path.toStdString());
		if (!file.existsAsFile())
		{
			if (libraryManager)
				libraryManager->notifyAnalysisFinished(path, false);
			continue;
		}

		bpmThreadPool->start(new BpmAnalysisTask(this, file, BpmAnalysisTask::Target::LibraryOnly));
	}
}

void QtMainWindow::onAnalyzeTracksAdvancedRequested(const QStringList& filePaths, double minBpm, double maxBpm)
{
	if (filePaths.isEmpty() || !bpmThreadPool)
		return;

	if (!bpmAnalyzer)
		bpmAnalyzer = std::make_unique<BpmAnalyzer>(*sharedFormatManager);

	const double minClamped = std::clamp(minBpm, 30.0, 400.0);
	const double maxClamped = std::clamp(maxBpm, minClamped + 1.0, 420.0);

	for (const QString& path : filePaths)
	{
		juce::File file(path.toStdString());
		if (!file.existsAsFile())
		{
			if (libraryManager)
				libraryManager->notifyAnalysisFinished(path, false);
			continue;
		}

		bpmThreadPool->start(new BpmAnalysisTask(this, file, BpmAnalysisTask::Target::LibraryOnly, minClamped, maxClamped));
	}
}

bool QtMainWindow::shouldAnalyzeTrackOnLoad(const QString& filePath) const
{
	if (filePath.isEmpty())
		return false;

	if (!libraryManager)
		return true;

	auto trackInfo = libraryManager->getTrackInfo(filePath);
	if (!trackInfo)
		return true;

	const TrackInfo& track = *trackInfo;

	if (track.analysisFailed)
		return true;

	if (!track.hasBeatGrid())
		return true;

	if (track.bpm <= 0.0)
		return true;

	QFileInfo fileInfo(filePath);
	if (fileInfo.exists())
	{
		const qint64 currentModified = fileInfo.lastModified().toSecsSinceEpoch();
		if (currentModified > 0 && track.lastModified > 0 && currentModified != track.lastModified)
			return true;
	}

	return false;
}

void QtMainWindow::startDeckAnalysisIfNeeded(const QString& filePath, bool isDeckA)
{
	if (filePath.isEmpty())
		return;

	const bool needsAnalysis = shouldAnalyzeTrackOnLoad(filePath);

	if (!needsAnalysis)
	{
		bool appliedFromLibrary = false;
		if (libraryManager)
		{
			if (auto info = libraryManager->getTrackInfo(filePath))
			{
				if (isDeckA)
				{
					algorithmA = info->analysisAlgorithm;
					analysisFailedA = info->analysisFailed;
					analysisProgressA = 1.0;
				}
				else
				{
					algorithmB = info->analysisAlgorithm;
					analysisFailedB = info->analysisFailed;
					analysisProgressB = 1.0;
				}

				if (QtDeckWidget* deck = isDeckA ? deckA : deckB)
				{
					applyStoredBeatGrid(deck, isDeckA);
					applyStoredCuePoints(deck, isDeckA);
				}

				appliedFromLibrary = true;
			}
		}

		if (WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB)
		{
			overview->setAnalysisActive(false);
			overview->setAnalysisFailed(false);
			overview->setAnalysisProgress(1.0);
		}

		if (isDeckA)
		{
			analysisActiveA = false;
			if (!appliedFromLibrary)
			{
				analysisFailedA = false;
				analysisProgressA = 1.0;
			}
		}
		else
		{
			analysisActiveB = false;
			if (!appliedFromLibrary)
			{
				analysisFailedB = false;
				analysisProgressB = 1.0;
			}
		}

		updateOverviewLabel(isDeckA);

		if (libraryManager)
		{
			if (auto info = libraryManager->getTrackInfo(filePath))
			{
				const QString displayTitle = info->getDisplayTitle();
				setStatusTip(QStringLiteral("Loaded stored analysis for %1").arg(displayTitle));
			}
		}

		return;
	}

	if (!bpmAnalyzer)
		bpmAnalyzer = std::make_unique<BpmAnalyzer>(*sharedFormatManager);

	juce::File file(filePath.toStdString());
	const auto target = isDeckA ? BpmAnalysisTask::Target::DeckA : BpmAnalysisTask::Target::DeckB;
	bpmThreadPool->start(new BpmAnalysisTask(this, file, target));
}

void QtMainWindow::handleBpmAnalysisResult(double bpm, const std::vector<double>& beatsSec, double totalSec,
										 const std::string& algorithm, double firstBeatOffset, bool isDeckA)
{
	QString analyzedFilePath;

	if (isDeckA)
	{
		if (!deckA || deckA->getCurrentFilePath().isEmpty())
		{
			if (beatIndicator) beatIndicator->setBeatGridAvailableDeckA(false);
			return;
		}
		analyzedFilePath = deckA->getCurrentFilePath();
		if (deckA) deckA->setDetectedBpm(bpm);
		if (deckA && deckA->getWaveform())
		{
			deckA->getWaveform()->setBeatInfo(bpm, firstBeatOffset, totalSec);
		}
		if (playerA)
		{
			playerA->setBeatInfo(bpm, firstBeatOffset, totalSec);
		}
		if (beatIndicator)
		{
			beatIndicator->setBpmDeckA(bpm);
			beatIndicator->setFirstBeatOffsetDeckA(firstBeatOffset);
		}
		if (overviewTopA)
		{
			overviewTopA->setOriginalBpm(bpm, totalSec);
			if (totalSec > 0.0 && !beatsSec.empty())
			{
				QVector<double> rel;
				rel.reserve(static_cast<int>(beatsSec.size()));
				for (double t : beatsSec) rel.append(t / totalSec);
				overviewTopA->setBeats(rel);
			}
		}
		if (beatIndicator) { beatIndicator->setBeatGridAvailableDeckA(bpm > 0.0); }
		if (deckA && deckA->getWaveform())
		{
			algorithmA = QString::fromStdString(algorithm);
			updateOverviewLabel(true);
		}
	}
	else
	{
		if (!deckB || deckB->getCurrentFilePath().isEmpty())
		{
			if (beatIndicator) beatIndicator->setBeatGridAvailableDeckB(false);
			return;
		}
		analyzedFilePath = deckB->getCurrentFilePath();
		if (deckB) deckB->setDetectedBpm(bpm);
		if (deckB && deckB->getWaveform())
		{
			deckB->getWaveform()->setBeatInfo(bpm, firstBeatOffset, totalSec);
		}
		if (playerB)
		{
			playerB->setBeatInfo(bpm, firstBeatOffset, totalSec);
		}
		if (beatIndicator)
		{
			beatIndicator->setBpmDeckB(bpm);
			beatIndicator->setFirstBeatOffsetDeckB(firstBeatOffset);
		}
		if (overviewTopB)
		{
			overviewTopB->setOriginalBpm(bpm, totalSec);
			if (totalSec > 0.0 && !beatsSec.empty())
			{
				QVector<double> rel;
				rel.reserve(static_cast<int>(beatsSec.size()));
				for (double t : beatsSec) rel.append(t / totalSec);
				overviewTopB->setBeats(rel);
			}
		}
		if (beatIndicator) { beatIndicator->setBeatGridAvailableDeckB(bpm > 0.0); }
		if (deckB && deckB->getWaveform())
		{
			algorithmB = QString::fromStdString(algorithm);
			updateOverviewLabel(false);
		}
	}

	if (libraryManager && !analyzedFilePath.isEmpty())
	{
		QVector<double> beatVector;
		beatVector.reserve(static_cast<int>(beatsSec.size()));
		for (double beat : beatsSec)
			beatVector.append(beat);

		libraryManager->applyAnalysisResult(
			analyzedFilePath,
			bpm,
			firstBeatOffset,
			totalSec,
			beatVector,
			QString::fromStdString(algorithm),
			bpm <= 0.0);
	}
}

void QtMainWindow::updateOverviewLabel(bool isDeckA)
{
	QtDeckWidget* deck = isDeckA ? deckA : deckB;
	DeckWaveformOverview* overview = deck ? deck->getWaveform() : nullptr;
	if (overview)
		overview->setOverlayStatus(QString(), QString(), false, 0.0, false);

	if (!deck)
		return;

	const QString deckName = isDeckA ? QStringLiteral("Deck A") : QStringLiteral("Deck B");
	QString trackName = QStringLiteral("No Track Loaded");
	QString trackTooltip = trackName;
	QString infoText = QStringLiteral("No track loaded");
	QString infoTooltip = infoText;
	QString infoStyle = QStringLiteral("font-size: 11px; color: #b8bfd0; padding: 2px;");

	const QString filePath = deck->getCurrentFilePath();
	if (!filePath.isEmpty()) {
		QFileInfo fileInfo(filePath);
		trackName = fileInfo.completeBaseName();
		if (trackName.isEmpty())
			trackName = fileInfo.fileName();
		if (trackName.isEmpty())
			trackName = deckName;

		trackTooltip = fileInfo.fileName();
		if (trackTooltip.isEmpty())
			trackTooltip = filePath;

		bool active = isDeckA ? analysisActiveA : analysisActiveB;
		bool failed = isDeckA ? analysisFailedA : analysisFailedB;
		double prog = isDeckA ? analysisProgressA : analysisProgressB;
		double originalBpm = (isDeckA ? overviewTopA : overviewTopB)
								 ? (isDeckA ? overviewTopA->originalBpm : overviewTopB->originalBpm)
								 : 0.0;
		const QString algorithm = isDeckA ? algorithmA : algorithmB;
		const double trimMs = (isDeckA ? userVisualTrimA : userVisualTrimB) * 1000.0;

		QStringList tooltipParts;
		double clampedProgress = std::clamp(prog, 0.0, 1.0);
		int percent = static_cast<int>(std::round(clampedProgress * 100.0));

		const bool showAnalysisState = active && percent < 100;

		if (active && percent >= 100) {
			// Streaming may continue in background; treat as complete for UI once at 100%
			active = false;
		}

		if (showAnalysisState) {
			const QString analyzingText = QStringLiteral("Analyzing %1%").arg(percent);
			tooltipParts << analyzingText;
			infoStyle = QStringLiteral("font-size: 11px; color: #4fb0ff; font-weight: bold; padding: 0px;");
			infoText = analyzingText;
		} else if (failed) {
			const QString failureText = QStringLiteral("Analysis failed");
			tooltipParts << failureText;
			infoStyle = QStringLiteral("font-size: 11px; color: #ff6f6f; font-weight: bold; padding: 0px;");
			infoText = failureText;
		} else {
			if (originalBpm > 0.0) {
				const int roundedBpm = static_cast<int>(std::round(originalBpm));
				infoText = QString::number(roundedBpm);
				tooltipParts << QStringLiteral("Original BPM: %1")
									.arg(QString::number(originalBpm, 'f', 2));
			} else {
				infoText = QStringLiteral("--");
				tooltipParts << QStringLiteral("Original BPM: unknown");
			}

			if (!algorithm.trimmed().isEmpty()) {
				const QString algoText = algorithm.trimmed();
				tooltipParts << QStringLiteral("Algorithm: %1").arg(algoText);
			}

			if (std::abs(trimMs) > 0.0001) {
				const QString trimText = QStringLiteral("trim %1 ms").arg(QString::number(trimMs, 'f', 1));
				tooltipParts << QStringLiteral("Visual trim: %1 ms").arg(QString::number(trimMs, 'f', 1));
			}

			infoStyle = QStringLiteral("font-size: 11px; color: #9ad1ff; font-weight: bold; padding: 0px;");
		}

		if (tooltipParts.isEmpty())
			tooltipParts << infoText;

		const QString tooltipJoin = tooltipParts.join(QStringLiteral("\n"));
		infoTooltip = tooltipJoin.isEmpty() ? infoText : tooltipJoin;
	} else {
		trackName = QStringLiteral("No Track Loaded");
		trackTooltip = trackName;
		infoText = QStringLiteral("No track loaded");
		infoTooltip = infoText;
		infoStyle = QStringLiteral("font-size: 11px; color: #b8bfd0; padding: 0px;");
	}

	deck->setTrackNameDisplay(trackName, trackTooltip);
	deck->setTrackInfoDisplay(infoText, infoStyle, infoTooltip);
}

