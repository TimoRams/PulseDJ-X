#include "QtMainWindow.h"
#include "DJAudioPlayer.h"
#include "BpmAnalyzer.h"
#include "WaveformDisplay.h"
#include "BeatIndicator.h"
#include "PreferencesDialog.h"
#include <iostream>
#include <QApplication>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QVector>
#include <QProgressDialog>
#include <array>
#include <algorithm>
#include <cmath>

// StereoAudioCallback - Main JUCE audio callback method
void StereoAudioCallback::audioDeviceIOCallback(const float* const* inputChannelData, int numInputChannels,
                                               float* const* outputChannelData, int numOutputChannels, 
                                               int numSamples) {
    // Create dummy context and call the full implementation
    juce::AudioIODeviceCallbackContext context;
    audioDeviceIOCallbackWithContext(inputChannelData, numInputChannels, 
                                    outputChannelData, numOutputChannels, 
                                    numSamples, context);
}

// StereoAudioCallback implementation for proper stereo mixing of both decks
void StereoAudioCallback::audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                                          float* const* outputChannelData, int numOutputChannels,
                                                          int numSamples, const juce::AudioIODeviceCallbackContext& context) {
    static int callCount = 0;
    static bool infoLogged = false;
    
    // Log device info once, then reduce spam
    if (!infoLogged) {
        std::cout << "Stereo Mixer callback: outputChannels=" << numOutputChannels 
                  << ", samples=" << numSamples << std::endl;
        infoLogged = true;
    }
    
    if (++callCount % 5000 == 0) {  // Less frequent logging
        std::cout << "Mixer running (" << callCount << " callbacks)" << std::endl;
    }
    
    if (numSamples <= 0) {
        // Clear output if no samples
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            if (outputChannelData[ch]) {
                juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            }
        }
        return;
    }
    
    // Ensure buffers are sized correctly for stereo
    const int bufferChannels = std::max(2, numOutputChannels);
    if (tempBufferA.getNumChannels() != bufferChannels || tempBufferA.getNumSamples() < numSamples) {
        tempBufferA.setSize(bufferChannels, numSamples, false, false, true);
    }
    if (tempBufferB.getNumChannels() != bufferChannels || tempBufferB.getNumSamples() < numSamples) {
        tempBufferB.setSize(bufferChannels, numSamples, false, false, true);
    }
    
    // Clear buffers
    tempBufferA.clear();
    tempBufferB.clear();
    
    // Get audio from both players
    juce::AudioSourceChannelInfo bufferInfoA;
    bufferInfoA.buffer = &tempBufferA;
    bufferInfoA.startSample = 0;
    bufferInfoA.numSamples = numSamples;
    
    juce::AudioSourceChannelInfo bufferInfoB;
    bufferInfoB.buffer = &tempBufferB;
    bufferInfoB.startSample = 0;
    bufferInfoB.numSamples = numSamples;
    
    // Get audio from Player A
    if (audioPlayerA) {
        audioPlayerA->getNextAudioBlock(bufferInfoA);
    }
    
    // Get audio from Player B
    if (audioPlayerB) {
        audioPlayerB->getNextAudioBlock(bufferInfoB);
    }
    
    // Clear all output channels first
    for (int ch = 0; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch]) {
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        }
    }
    
    // Load mixer parameters (atomic reads)
    const float volA = volumeA.load();
    const float volB = volumeB.load();
    const float crossfader = crossfaderPos.load();
    const float master = masterVolume.load();
    
    // Calculate crossfader gains (equal power law for smooth transitions)
    float gainA, gainB;
    if (crossfader <= 0.0f) {
        // Left side: A stays full, B fades out
        float fadePos = std::abs(crossfader); // 0.0 to 1.0
        gainA = 1.0f;
        gainB = std::cos(fadePos * M_PI * 0.5f); // Cosine fade
    } else {
        // Right side: A fades out, B stays full  
        float fadePos = crossfader; // 0.0 to 1.0
        gainA = std::cos(fadePos * M_PI * 0.5f); // Cosine fade
        gainB = 1.0f;
    }
    
    // Apply volume controls
    gainA *= volA;
    gainB *= volB;
    
    // Mix both players to output with proper stereo channel mapping
    for (int ch = 0; ch < std::min(numOutputChannels, 2); ++ch) {
        if (outputChannelData[ch]) {
            // Mix Player A
            if (ch < tempBufferA.getNumChannels()) {
                juce::FloatVectorOperations::addWithMultiply(
                    outputChannelData[ch], 
                    tempBufferA.getReadPointer(ch), 
                    gainA, 
                    numSamples
                );
            }
            
            // Mix Player B
            if (ch < tempBufferB.getNumChannels()) {
                juce::FloatVectorOperations::addWithMultiply(
                    outputChannelData[ch], 
                    tempBufferB.getReadPointer(ch), 
                    gainB, 
                    numSamples
                );
            }
            
            // Apply master volume
            if (master != 1.0f) {
                juce::FloatVectorOperations::multiply(outputChannelData[ch], master, numSamples);
            }
        }
    }
    
    // If more than 2 output channels, copy stereo to remaining channels
    for (int ch = 2; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch] && outputChannelData[ch % 2]) {
            juce::FloatVectorOperations::copy(outputChannelData[ch], outputChannelData[ch % 2], numSamples);
        }
    }
}

void StereoAudioCallback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    std::cout << "StereoAudioCallback: Device starting - " << device->getName().toStdString() 
              << ", Channels: " << device->getActiveOutputChannels().toInteger() 
              << ", Sample Rate: " << device->getCurrentSampleRate() << std::endl;
}

void StereoAudioCallback::audioDeviceStopped() {
    std::cout << "StereoAudioCallback: Device stopped" << std::endl;
}
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDir>
#include <QListWidgetItem>
#include <QStringList>
#include <QFileDialog>
#include <QPushButton>
#include <QApplication>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QThreadPool>
#include <QRunnable>
#include <QPointer>
#include <cmath>
#include <QThread>
#include <QSettings>
#include <QMenuBar>
#include <QAction>
#include <QMessageBox>
#include <QRegularExpression>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <iostream>
#include "WaveformGenerator.h"
#include "AppConfig.h"
#include "DeckSettings.h"
#include "AudioFormatGuard.h"

// Static members for shared format manager
juce::AudioFormatManager* QtMainWindow::sharedFormatManager = nullptr;
int QtMainWindow::formatManagerRefCount = 0;

// NEW: Thread-safe audio file loading for non-blocking UI
class AudioFileLoadTask : public QRunnable {
public:
    AudioFileLoadTask(QtMainWindow* mainWindow, QString filePath, bool isDeckA) 
        : window(mainWindow), filePath(std::move(filePath)), isDeckA(isDeckA) {
        setAutoDelete(true);
    }
    
    void run() override {
        if (!window) return;
        
        try {
            // PERFORMANCE: Set lower thread priority to prevent UI blocking
            QThread::currentThread()->setPriority(QThread::LowPriority);
            
            // Load audio file data in background thread
            juce::File audioFile(filePath.toStdString());
            std::unique_ptr<juce::AudioFormatReader> reader;
            {
                AudioFormatManagerGuard formatGuard;
                reader.reset(window->sharedFormatManager->createReaderFor(audioFile));
            }
            
            if (reader) {
                // Create the reader source in background thread
                auto readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
                double sampleRate = readerSource->getAudioFormatReader()->sampleRate;
                
                // Thread-safe UI update: Apply loaded file to player on main thread
                QMetaObject::invokeMethod(window, [=, source = readerSource.release()]() mutable {
                    if (window) {
                        DJAudioPlayer* player = isDeckA ? window->playerA : window->playerB;
                        QtDeckWidget* deckWidget = isDeckA ? window->deckA : window->deckB;
                        if (player && deckWidget) {
                            // Apply the pre-loaded audio source to the player
                            std::unique_ptr<juce::AudioFormatReaderSource> sourcePtr(source);
                            player->applyLoadedSource(std::move(sourcePtr), sampleRate);
                            
                            // Update UI elements on main thread
                            deckWidget->onFileLoadingComplete(filePath);
                        }
                    }
                }, Qt::QueuedConnection);
                
            } else {
                // Thread-safe error handling
                QMetaObject::invokeMethod(window, [=]() {
                    if (window) {
                        QFileInfo fi(filePath);
                        window->setStatusTip(QString("Failed to load audio file: %1").arg(fi.fileName()));
                    }
                }, Qt::QueuedConnection);
            }
            
        } catch (const std::exception& e) {
            // Thread-safe error handling
            QMetaObject::invokeMethod(window, [=, error = QString::fromStdString(e.what())]() {
                if (window) {
                    QFileInfo fi(filePath);
                    window->setStatusTip(QString("Audio loading error: %1 - %2").arg(fi.fileName()).arg(error));
                }
            }, Qt::QueuedConnection);
        }
    }
    
private:
    QPointer<QtMainWindow> window;
    QString filePath;
    bool isDeckA;
};

// Enhanced threaded BPM analysis task with better performance and UI responsiveness
class BpmAnalysisTask : public QRunnable {
public:
    enum class Target { DeckA, DeckB, LibraryOnly };

    BpmAnalysisTask(QtMainWindow* mainWindow, juce::File file, Target target,
                    double minBpm = 40.0, double maxBpm = 260.0)
        : window(mainWindow),
          audioFile(std::move(file)),
          target(target),
          minBpm(std::min(minBpm, maxBpm)),
          maxBpm(std::max(minBpm, maxBpm))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QPointer<QtMainWindow> wptr = window;
        if (!wptr)
            return;

        const bool deckTask = target != Target::LibraryOnly;
        const bool deckIsA = target == Target::DeckA;
        const QString filePath = QString::fromStdString(audioFile.getFullPathName().toStdString());
        const QString displayName = QString::fromStdString(audioFile.getFileNameWithoutExtension().toStdString());

        if (auto windowRaw = wptr.data()) {
            QMetaObject::invokeMethod(windowRaw, [wptr, deckTask, deckIsA, displayName, filePath]() {
                if (auto window = wptr.data()) {
                    window->setStatusTip(QString("Analyzing BPM: %1...").arg(displayName));

                    if (window->libraryManager)
                        window->libraryManager->notifyAnalysisStarted(filePath);

                    if (deckTask)
                    {
                        if (deckIsA)
                        {
                            window->analysisActiveA = true;
                            window->analysisFailedA = false;
                            window->analysisProgressA = 0.0;
                        }
                        else
                        {
                            window->analysisActiveB = true;
                            window->analysisFailedB = false;
                            window->analysisProgressB = 0.0;
                        }

                        window->updateOverviewLabel(deckIsA);
                    }
                }
            }, Qt::QueuedConnection);
        }

        try
        {
            std::vector<double> beatsSec;
            double totalSec = 0.0;
            std::string algorithm;
            double firstBeatOffset = 0.0;

            QThread::currentThread()->setPriority(QThread::LowPriority);

            if (auto windowRaw = wptr.data()) {
                QMetaObject::invokeMethod(windowRaw, [wptr, deckTask, deckIsA]() {
                    if (!deckTask)
                        return;

                    if (auto window = wptr.data()) {
                        WaveformDisplay* wf = deckIsA ? window->overviewTopA : window->overviewTopB;
                        if (wf)
                        {
                            wf->setAnalysisFailed(false);
                            wf->setAnalysisActive(true);
                            wf->setAnalysisProgress(0.0);
                        }
                    }
                }, Qt::QueuedConnection);
            }

            auto progressCb = [wptr = QPointer<QtMainWindow>(window), deckTask, deckIsA, filePath](double p) {
                if (!wptr)
                    return;

                QMetaObject::invokeMethod(wptr, [wptr, p, deckTask, deckIsA, filePath]() {
                    if (!wptr)
                        return;

                    if (deckTask)
                    {
                        WaveformDisplay* wf = deckIsA ? wptr->overviewTopA : wptr->overviewTopB;
                        if (wf)
                            wf->setAnalysisProgress(p);

                        if (deckIsA)
                            wptr->analysisProgressA = p;
                        else
                            wptr->analysisProgressB = p;

                        wptr->updateOverviewLabel(deckIsA);
                    }

                    if (wptr->libraryManager)
                        wptr->libraryManager->notifyAnalysisProgress(filePath, p);
                }, Qt::QueuedConnection);
            };

            auto errorCb = [wptr = QPointer<QtMainWindow>(window), deckTask, deckIsA, filePath](const std::string&) {
                if (!wptr)
                    return;

                QMetaObject::invokeMethod(wptr, [wptr, deckTask, deckIsA, filePath]() {
                    if (!wptr)
                        return;

                    if (deckTask)
                    {
                        WaveformDisplay* wf = deckIsA ? wptr->overviewTopA : wptr->overviewTopB;
                        if (wf)
                        {
                            wf->setAnalysisFailed(true);
                            wf->setAnalysisActive(false);
                        }

                        if (deckIsA)
                        {
                            wptr->analysisFailedA = true;
                            wptr->analysisActiveA = false;
                        }
                        else
                        {
                            wptr->analysisFailedB = true;
                            wptr->analysisActiveB = false;
                        }

                        wptr->updateOverviewLabel(deckIsA);
                    }

                    if (wptr->libraryManager)
                        wptr->libraryManager->notifyAnalysisFinished(filePath, false);
                }, Qt::QueuedConnection);
            };

            const double minRange = std::clamp(minBpm, 30.0, 400.0);
            const double maxRange = std::clamp(maxBpm, std::max(minRange + 1.0, 30.5), 420.0);
            double bpm = window->bpmAnalyzer->analyzeFile(audioFile,
                                                          120.0,
                                                          &beatsSec,
                                                          &totalSec,
                                                          &algorithm,
                                                          &firstBeatOffset,
                                                          progressCb,
                                                          errorCb,
                                                          minRange,
                                                          maxRange);

            if (auto windowRaw = wptr.data()) {
                QMetaObject::invokeMethod(windowRaw, [wptr, deckTask, deckIsA, displayName, bpm, beatsSec, totalSec, algorithm, firstBeatOffset, filePath]() {
                    if (auto window = wptr.data()) {
                        window->setStatusTip(QString("Analysis complete: %1 (%2 BPM)")
                                                  .arg(displayName)
                                                  .arg(QString::number(bpm, 'f', 1)));

                        if (deckTask)
                        {
                            window->handleBpmAnalysisResult(bpm, beatsSec, totalSec, algorithm, firstBeatOffset, deckIsA);

                            WaveformDisplay* wf = deckIsA ? window->overviewTopA : window->overviewTopB;
                            if (wf)
                            {
                                wf->setAnalysisActive(false);
                                wf->setAnalysisFailed(bpm <= 0.0);
                                wf->setAnalysisProgress(1.0);
                            }

                            if (deckIsA)
                            {
                                window->analysisActiveA = false;
                                window->analysisFailedA = (bpm <= 0.0);
                                window->analysisProgressA = 1.0;
                            }
                            else
                            {
                                window->analysisActiveB = false;
                                window->analysisFailedB = (bpm <= 0.0);
                                window->analysisProgressB = 1.0;
                            }

                            window->updateOverviewLabel(deckIsA);
                        }
                        else if (window->libraryManager)
                        {
                            QVector<double> beatVector;
                            beatVector.reserve(static_cast<int>(beatsSec.size()));
                            for (double beat : beatsSec)
                                beatVector.append(beat);

                            window->libraryManager->applyAnalysisResult(filePath,
                                                                         bpm,
                                                                         firstBeatOffset,
                                                                         totalSec,
                                                                         beatVector,
                                                                         QString::fromStdString(algorithm),
                                                                         bpm <= 0.0);

                            bool reAppliedToDeck = false;
                            if (window->deckA && window->deckA->getCurrentFilePath() == filePath)
                            {
                                window->reapplyStoredDeckMetadata(true);
                                reAppliedToDeck = true;
                            }
                            if (window->deckB && window->deckB->getCurrentFilePath() == filePath)
                            {
                                window->reapplyStoredDeckMetadata(false);
                                reAppliedToDeck = true;
                            }

                            if (reAppliedToDeck)
                            {
                                window->setStatusTip(QStringLiteral("Reapplied analysis to loaded deck: %1")
                                                         .arg(displayName));
                            }
                        }

                        if (window->libraryManager)
                            window->libraryManager->notifyAnalysisFinished(filePath, bpm > 0.0);
                    }
                }, Qt::QueuedConnection);
            }
        }
        catch (const std::exception& e)
        {
            if (auto windowRaw = wptr.data()) {
                QMetaObject::invokeMethod(windowRaw, [wptr, deckTask, deckIsA, displayName, error = QString::fromStdString(e.what()), filePath]() {
                    if (auto window = wptr.data()) {
                        window->setStatusTip(QString("Analysis failed: %1 - %2").arg(displayName).arg(error));

                        if (window->libraryManager)
                            window->libraryManager->notifyAnalysisFinished(filePath, false);

                        if (deckTask)
                        {
                            WaveformDisplay* wf = deckIsA ? window->overviewTopA : window->overviewTopB;
                            if (wf)
                            {
                                wf->setAnalysisFailed(true);
                                wf->setAnalysisActive(false);
                            }

                            if (deckIsA)
                            {
                                window->analysisFailedA = true;
                                window->analysisActiveA = false;
                            }
                            else
                            {
                                window->analysisFailedB = true;
                                window->analysisActiveB = false;
                            }

                            window->updateOverviewLabel(deckIsA);
                        }
                    }
                }, Qt::QueuedConnection);
            }
        }
    }

private:
    QPointer<QtMainWindow> window;
    juce::File audioFile;
    Target target;
    double minBpm;
    double maxBpm;
};

// NEW: Background task to generate WaveformDisplay data without blocking UI
class TopWaveformDisplayTask : public QRunnable {
public:
    TopWaveformDisplayTask(QtMainWindow* mainWindow, QString filePath, bool isDeckA)
        : window(mainWindow), filePath(std::move(filePath)), isDeckA(isDeckA) {
        setAutoDelete(true);
    }

    void run() override {
        if (!window) return;
        try {
            QThread::currentThread()->setPriority(QThread::LowestPriority);
            WaveformGenerator gen;
            WaveformGenerator::Result res;
            // High-res bins for smooth top overview
            const int binCount = 16000;
            if (!gen.generate(juce::File(filePath.toStdString()), binCount, res)) return;

            auto maxBins = std::make_shared<std::vector<float>>(res.maxBins);
            auto minBins = std::make_shared<std::vector<float>>(res.minBins);

            const double audioStart = res.audioStartOffsetSec;
            const double lengthSec = res.lengthSeconds;

            const bool onDeckA = isDeckA;
            QMetaObject::invokeMethod(window, [w = window, maxBins, minBins, audioStart, lengthSec, onDeckA, filePath = this->filePath]() {
                if (!w) return;
                // Ensure deck still has the same file loaded to avoid race after unload/reload
                QtDeckWidget* deck = onDeckA ? w->deckA : w->deckB;
                if (!deck) return;
                if (deck->getCurrentFilePath() != filePath) return;
                WaveformDisplay* wf = onDeckA ? w->overviewTopA : w->overviewTopB;
                if (wf)
                {
                    wf->setSourceBins(*maxBins, *minBins, audioStart, lengthSec);
                    w->reapplyStoredDeckMetadata(onDeckA);
                }
            }, Qt::QueuedConnection);
        } catch (...) {
            // Ignore errors; overview is non-critical
        }
    }

private:
    QPointer<QtMainWindow> window;
    QString filePath;
    bool isDeckA;
};

// NEW: Threaded Waveform Loading Task to prevent UI blocking
class WaveformLoadTask : public QRunnable {
public:
    WaveformLoadTask(QtMainWindow* mainWindow, QString filePath, bool isDeckA) 
        : window(mainWindow), filePath(std::move(filePath)), isDeckA(isDeckA) {
        setAutoDelete(true);
    }
    
    void run() override {
        if (!window) return;
        
        try {
            // PERFORMANCE: Set lower thread priority to prevent UI blocking
            QThread::currentThread()->setPriority(QThread::LowPriority);
            
            // Signal loading start
            QMetaObject::invokeMethod(window, [this]() {
                if (window) {
                    QString filename = QFileInfo(filePath).baseName();
                    window->setStatusTip(QString("Loading waveform: %1...").arg(filename));
                }
            }, Qt::QueuedConnection);
            
            // Get track length for immediate UI update
            juce::File f(filePath.toStdString());
            juce::AudioFormatReader* reader = nullptr;
            {
                AudioFormatManagerGuard formatGuard;
                reader = QtMainWindow::sharedFormatManager->createReaderFor(f);
            }
            double trackLength = 0.0;
            if (reader) {
                trackLength = reader->lengthInSamples / reader->sampleRate;
                delete reader;
            }
            
            // Thread-safe waveform loading completion
            QMetaObject::invokeMethod(window, [=]() {
                if (window) {
                    // Get the appropriate waveform display
                    QtDeckWidget* deck = isDeckA ? window->deckA : window->deckB;
                    if (!deck || deck->getCurrentFilePath() != filePath) return;
                    WaveformDisplay* waveform = isDeckA ? window->overviewTopA : window->overviewTopB;
                    if (waveform) {
                        // Load waveform and set up defaults
                        waveform->loadFile(filePath);
                        waveform->setPlayhead(0.0);
                        waveform->setTrackLength(trackLength);
                        waveform->update();
                        
                        QString filename = QFileInfo(filePath).baseName();
                        window->setStatusTip(QString("Waveform loaded: %1").arg(filename));
                    }
                }
            }, Qt::QueuedConnection);
            
        } catch (const std::exception& e) {
            // Thread-safe error handling
            QMetaObject::invokeMethod(window, [this, error = QString::fromStdString(e.what())]() {
                if (window) {
                    QString filename = QFileInfo(filePath).baseName();
                    window->setStatusTip(QString("Waveform loading failed: %1 - %2").arg(filename).arg(error));
                }
            }, Qt::QueuedConnection);
        }
    }
    
private:
    QPointer<QtMainWindow> window;
    QString filePath;
    bool isDeckA;
};

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
        if (auto* player = isDeckA ? playerA : playerB)
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

    if (auto* player = isDeckA ? playerA : playerB)
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
        bpmAnalyzer = new BpmAnalyzer(*sharedFormatManager);

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
        bpmAnalyzer = new BpmAnalyzer(*sharedFormatManager);

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

QtMainWindow::QtMainWindow(QWidget* parent) : QWidget(parent)
{
    std::cout << "=== QtMainWindow CONSTRUCTOR STARTING ===" << std::endl;
    setWindowTitle("BetaPulseX - Professional DJ Software");
    
    // Remove window decorations and make frameless
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    
    // BetaPulseX: Setup Menu System
    menuBar = new MenuBar(this);
    
    // Initialize or reuse shared format manager for better performance
    if (!sharedFormatManager) {
        sharedFormatManager = new juce::AudioFormatManager();
        
        // Register JUCE's basic formats (includes built-in MP3 support with JUCE_USE_MP3AUDIOFORMAT=1)
        sharedFormatManager->registerBasicFormats();
        
        std::cout << "Audio format manager initialized with " 
                  << sharedFormatManager->getNumKnownFormats() << " formats" << std::endl;
        
        // List supported formats
        for (int i = 0; i < sharedFormatManager->getNumKnownFormats(); ++i) {
            auto* format = sharedFormatManager->getKnownFormat(i);
            std::cout << "Supported format: " << format->getFormatName().toStdString() 
                      << " (" << format->getFileExtensions().joinIntoString(", ").toStdString() << ")" << std::endl;
        }
    }
    formatManagerRefCount++;

        // Enhanced thread pool configuration for BPM analysis and waveform loading performance
    bpmThreadPool = std::make_unique<QThreadPool>();
    
    // PERFORMANCE: Optimize thread count for both BMP analysis and waveform loading
    int idealThreads = QThread::idealThreadCount();
    int maxBmpThreads = std::min(4, std::max(2, idealThreads / 2)); // Increased to 4 max for waveform+BMP
    bpmThreadPool->setMaxThreadCount(maxBmpThreads);
    
    // Set expiry timeout to clean up idle threads (saves memory)
    bpmThreadPool->setExpiryTimeout(30000); // 30 seconds
    
    std::cout << "Audio processing thread pool: " << maxBmpThreads 
              << " threads (system has " << idealThreads << " cores)" << std::endl;

    playerA = new DJAudioPlayer(*sharedFormatManager);
    playerB = new DJAudioPlayer(*sharedFormatManager);

    qDebug() << "QtMainWindow: About to create deck widgets";
    std::cout << "=== CREATING DECK WIDGETS ===" << std::endl;
    deckA = new QtDeckWidget(playerA, this, "DECK 1", true);  // left deck
    qDebug() << "QtMainWindow: Deck A created";
    std::cout << "=== DECK A CREATED ===" << std::endl;
    deckB = new QtDeckWidget(playerB, this, "DECK 2", false); // right deck
    qDebug() << "QtMainWindow: Deck B created";
    std::cout << "=== DECK B CREATED ===" << std::endl;

    // BetaPulseX: Lade und wende gespeicherte Deck-Einstellungen an (verzögert)
    // Verwende QTimer::singleShot um sicherzustellen, dass alle Widgets initialisiert sind
    std::cout << "*** SCHEDULING applyDeckSettings() with 100ms delay ***" << std::endl;
    QTimer::singleShot(100, this, [this]() {
        std::cout << "*** QTimer callback FIRED - calling applyDeckSettings() ***" << std::endl;
        applyDeckSettings();
    });

    // Top overview waveforms (two stacked, centered playhead)
    overviewTopA = new WaveformDisplay(this);
    overviewTopB = new WaveformDisplay(this);
    overviewTopA->setScrollMode(true);
    overviewTopB->setScrollMode(true);
    // Click-to-seek on top overview waveforms (works while paused)
    connect(overviewTopA, &WaveformDisplay::positionClicked, this, [this](double absRel){
        if (!playerA) return;
        absRel = std::clamp(absRel, 0.0, 1.0);
        playerA->setPositionRelative(absRel);
        // Update visuals immediately so paused seeking feels instant
        overviewTopA->setPlayhead(absRel);
        if (deckA && deckA->getWaveform()) deckA->getWaveform()->setPlayhead(absRel);
        // Also update beat indicator time to audible-relative base (no latency here on paused seek)
        if (beatIndicator) {
            double len = std::max(1e-9, playerA->getLengthInSeconds());
            beatIndicator->setTrackPositionDeckA(absRel * len);
        }
    });
    connect(overviewTopB, &WaveformDisplay::positionClicked, this, [this](double absRel){
        if (!playerB) return;
        absRel = std::clamp(absRel, 0.0, 1.0);
        playerB->setPositionRelative(absRel);
        overviewTopB->setPlayhead(absRel);
        if (deckB && deckB->getWaveform()) deckB->getWaveform()->setPlayhead(absRel);
        if (beatIndicator) {
            double len = std::max(1e-9, playerB->getLengthInSeconds());
            beatIndicator->setTrackPositionDeckB(absRel * len);
        }
    });

    // Beat indicator for showing current beat position
    beatIndicator = new BeatIndicator(this);
    
    // Connect beat indicator to deck widgets for performance pads
    deckA->setBeatIndicator(beatIndicator);
    deckB->setBeatIndicator(beatIndicator);

    // Scratch interactions for overview waveforms - proper vinyl-style scratching
    connect(overviewTopA, &WaveformDisplay::scratchStart, this, [this]() {
        if (!playerA) return;
        if (scratchInertiaActiveA) {
            stopScratchInertia(true, scratchInertiaResumeA);
        }
        const bool wasAudible = playerA->isAudible();
        scratchWasPlayingA = wasAudible;
        playerA->enableScratch(true);
        if (!wasAudible) {
            playerA->ensureScratchAudible();
        }
    });
    connect(overviewTopA, &WaveformDisplay::scratchMove, this, [this](double absRel) {
        applyScratchPosition(true, absRel);
    });
    connect(overviewTopA, &WaveformDisplay::scratchVelocityChanged, this, [this](double velocity) {
        if (!playerA) return;
        playerA->setScratchVelocity(velocity);
    });
    connect(overviewTopA, &WaveformDisplay::scratchEnd, this, [this]() {
        if (!playerA || !overviewTopA) return;

        lastScratchEndA = QDateTime::currentMSecsSinceEpoch();

        double releaseVelocity = overviewTopA->getLastScratchVelocity();
        playerA->setScratchVelocity(0.0);

        if (scratchWasPlayingA) {
            playerA->enableScratch(false);
            if (!playerA->isAudible()) {
                playerA->ensureScratchAudible();
            }
            scratchInertiaResumeA = true;
        } else {
            scratchInertiaResumeA = false;
            startScratchInertia(true, releaseVelocity, false);
        }
    });

    connect(overviewTopB, &WaveformDisplay::scratchStart, this, [this]() {
        if (!playerB) return;
        if (scratchInertiaActiveB) {
            stopScratchInertia(false, scratchInertiaResumeB);
        }
        const bool wasAudible = playerB->isAudible();
        scratchWasPlayingB = wasAudible;
        playerB->enableScratch(true);
        if (!wasAudible) {
            playerB->ensureScratchAudible();
        }
    });
    connect(overviewTopB, &WaveformDisplay::scratchMove, this, [this](double absRel) {
        applyScratchPosition(false, absRel);
    });
    connect(overviewTopB, &WaveformDisplay::scratchVelocityChanged, this, [this](double velocity) {
        if (!playerB) return;
        playerB->setScratchVelocity(velocity);
    });
    connect(overviewTopB, &WaveformDisplay::scratchEnd, this, [this]() {
        if (!playerB || !overviewTopB) return;

        lastScratchEndB = QDateTime::currentMSecsSinceEpoch();

        double releaseVelocity = overviewTopB->getLastScratchVelocity();
        playerB->setScratchVelocity(0.0);

        if (scratchWasPlayingB) {
            playerB->enableScratch(false);
            if (!playerB->isAudible()) {
                playerB->ensureScratchAudible();
            }
            scratchInertiaResumeB = true;
        } else {
            scratchInertiaResumeB = false;
            startScratchInertia(false, releaseVelocity, false);
        }
    });

    // NEW: Handle threaded audio file loading to prevent UI blocking
    connect(deckA, &QtDeckWidget::fileLoadingStarted, [this](const QString& filePath) {
        if (!filePath.isEmpty()) {
            // Start audio file loading in background thread
            bpmThreadPool->start(new AudioFileLoadTask(this, filePath, true));
        }
    });
    
    connect(deckB, &QtDeckWidget::fileLoadingStarted, [this](const QString& filePath) {
        if (!filePath.isEmpty()) {
            // Start audio file loading in background thread
            bpmThreadPool->start(new AudioFileLoadTask(this, filePath, false));
        }
    });

        // When deck files load, forward to overview waveforms and reset position
    connect(deckA, &QtDeckWidget::fileLoadingStarted, this, [this](const QString&) {
        if (overviewTopA)
            overviewTopA->clearCuePoints();
    });

    connect(deckA, &QtDeckWidget::fileLoaded, [this]() {
        QString filePath = deckA->getCurrentFilePath();
        if (!filePath.isEmpty()) {
            // PERFORMANCE FIX: Generate top overview waveform in background
            bpmThreadPool->start(new TopWaveformDisplayTask(this, filePath, true));
            
            // Start BPM analysis only if stored metadata isn't usable
            startDeckAnalysisIfNeeded(filePath, true);
        }
    });

    connect(deckA, &QtDeckWidget::fileLoaded, this, [this]() {
        applyStoredCuePoints(deckA, true);
        applyStoredBeatGrid(deckA, true);
    });

    connect(deckB, &QtDeckWidget::fileLoadingStarted, this, [this](const QString&) {
        if (overviewTopB)
            overviewTopB->clearCuePoints();
    });

    connect(deckB, &QtDeckWidget::fileLoaded, [this]() {
        QString filePath = deckB->getCurrentFilePath();
        if (!filePath.isEmpty()) {
            // Generate top overview waveform in background immediately (waveform visible)
            bpmThreadPool->start(new TopWaveformDisplayTask(this, filePath, false));

            // Start BPM analysis only if stored metadata isn't usable
            startDeckAnalysisIfNeeded(filePath, false);
        }
    });

    connect(deckB, &QtDeckWidget::fileLoaded, this, [this]() {
        applyStoredCuePoints(deckB, false);
        applyStoredBeatGrid(deckB, false);
    });

    // Clear visuals and indicator when a deck unloads
    connect(deckA, &QtDeckWidget::fileUnloaded, this, [this]() {
        if (overviewTopA) {
            overviewTopA->clearDisplay();
        }
        if (beatIndicator) {
            beatIndicator->setBeatGridAvailableDeckA(false);
        }
        analysisActiveA = false; analysisFailedA = false; analysisProgressA = 0.0; algorithmA.clear();
        updateOverviewLabel(true);
    });
    connect(deckB, &QtDeckWidget::fileUnloaded, this, [this]() {
        if (overviewTopB) {
            overviewTopB->clearDisplay();
        }
        if (beatIndicator) {
            beatIndicator->setBeatGridAvailableDeckB(false);
        }
        analysisActiveB = false; analysisFailedB = false; analysisProgressB = 0.0; algorithmB.clear();
        updateOverviewLabel(false);
    });
    // When playhead updates on deck, update overview playhead and beat indicator
    connect(deckA, &QtDeckWidget::playheadUpdated, this, [this](double relative) {
        double deviceLatencySec = 0.0;
        if (auto* dev = deviceManager.getCurrentAudioDevice()) {
            const double sr = dev->getCurrentSampleRate();
            if (sr > 0.0) {
                // Prefer device-reported output latency, else approximate with 1.5x buffer size
                const int buf = dev->getCurrentBufferSizeSamples();
                const int outLat = dev->getOutputLatencyInSamples();
                if (outLat > 0) deviceLatencySec = outLat / sr; else deviceLatencySec = (buf > 0 ? (1.5 * buf) / sr : 0.0);
            }
        }
        double pipelineLatencySec = playerA ? playerA->getPipelineLatencySeconds() : 0.0;
        // Align center with audible output: subtract total playback latency
        double visualDelay = std::clamp(pipelineLatencySec + deviceLatencySec, 0.0, 0.25);
        // Predict frame/display pipeline delay to the next vsync to avoid visual lead
        constexpr double uiFudgeSec = 0.012; // ~12 ms safety (display/vsync)
        // Apply optional user trim (positive delays visuals more)
        double totalDelay = visualDelay + uiFudgeSec + std::clamp(userVisualTrimA, -0.05, 0.05);
        // Compute audible-relative playhead and feed it directly
        if (playerA) {
            double displayRel = relative;
            if (relative >= 0.0) {
                double len = playerA->getLengthInSeconds();
                if (len > 1e-6) {
                    displayRel = relative - (totalDelay / len);
                }
                displayRel = std::clamp(displayRel, 0.0, 1.0);
            }
            overviewTopA->setPlayhead(displayRel);
            if (deckA && deckA->getWaveform()) deckA->getWaveform()->setPlayhead(displayRel);
        } else {
            overviewTopA->setPlayhead(relative);
            if (deckA && deckA->getWaveform()) deckA->getWaveform()->setPlayhead(relative);
        }
        // Update beat indicator with audible time in seconds (support preroll)
        if (playerA) {
            double curSec = playerA->getCurrentPositionSeconds();
            double audibleTimeSec = curSec - totalDelay; // do not clamp; may be negative in preroll
            beatIndicator->setTrackPositionDeckA(audibleTimeSec);
        }
    });
    connect(deckB, &QtDeckWidget::playheadUpdated, this, [this](double relative) {
        double deviceLatencySec = 0.0;
        if (auto* dev = deviceManager.getCurrentAudioDevice()) {
            const double sr = dev->getCurrentSampleRate();
            if (sr > 0.0) {
                const int buf = dev->getCurrentBufferSizeSamples();
                const int outLat = dev->getOutputLatencyInSamples();
                if (outLat > 0) deviceLatencySec = outLat / sr; else deviceLatencySec = (buf > 0 ? (1.5 * buf) / sr : 0.0);
            }
        }
        double pipelineLatencySec = playerB ? playerB->getPipelineLatencySeconds() : 0.0;
    double visualDelay = std::clamp(pipelineLatencySec + deviceLatencySec, 0.0, 0.25);
    constexpr double uiFudgeSec = 0.012; // ~12 ms safety (display/vsync)
    double totalDelay = visualDelay + uiFudgeSec + std::clamp(userVisualTrimB, -0.05, 0.05);
        if (playerB) {
            double displayRel = relative;
            if (relative >= 0.0) {
                double len = playerB->getLengthInSeconds();
                if (len > 1e-6) {
                    displayRel = relative - (totalDelay / len);
                }
                displayRel = std::clamp(displayRel, 0.0, 1.0);
            }
            overviewTopB->setPlayhead(displayRel);
            if (deckB && deckB->getWaveform()) deckB->getWaveform()->setPlayhead(displayRel);
        } else {
            overviewTopB->setPlayhead(relative);
            if (deckB && deckB->getWaveform()) deckB->getWaveform()->setPlayhead(relative);
        }
        // Update beat indicator with audible time in seconds (support preroll)
        if (playerB) {
            double curSec = playerB->getCurrentPositionSeconds();
            double audibleTimeSec = curSec - totalDelay; // do not clamp; may be negative in preroll
            beatIndicator->setTrackPositionDeckB(audibleTimeSec);
        }
    });

    // CRITICAL FIX: Connect tempo factor changes to waveform displays
    connect(deckA, &QtDeckWidget::tempoFactorChanged, overviewTopA, &WaveformDisplay::setTempoFactor);
    connect(deckB, &QtDeckWidget::tempoFactorChanged, overviewTopB, &WaveformDisplay::setTempoFactor);
    
    // Connect cue points from performance pads to top waveform displays
    if (deckA->getPerformancePads()) {
        connect(deckA->getPerformancePads(), &PerformancePads::cuePointsChanged, overviewTopA, &WaveformDisplay::setCuePoints);
        connect(deckA->getPerformancePads(), &PerformancePads::cuePointsChanged, this, [this](const std::array<double, 8>& cues){
            if (!libraryManager || !deckA)
                return;
            const QString filePath = deckA->getCurrentFilePath();
            if (filePath.isEmpty())
                return;
            libraryManager->saveCuePointsForTrack(filePath, cues);
        });
    }
    if (deckB->getPerformancePads()) {
        connect(deckB->getPerformancePads(), &PerformancePads::cuePointsChanged, overviewTopB, &WaveformDisplay::setCuePoints);
        connect(deckB->getPerformancePads(), &PerformancePads::cuePointsChanged, this, [this](const std::array<double, 8>& cues){
            if (!libraryManager || !deckB)
                return;
            const QString filePath = deckB->getCurrentFilePath();
            if (filePath.isEmpty())
                return;
            libraryManager->saveCuePointsForTrack(filePath, cues);
        });
    }
    
    // Connect cue points from performance pads to deck waveform overviews
    if (deckA->getPerformancePads() && deckA->getWaveform()) {
        connect(deckA->getPerformancePads(), &PerformancePads::cuePointsChanged, deckA->getWaveform(), &DeckWaveformOverview::setCuePoints);
    }
    if (deckB->getPerformancePads() && deckB->getWaveform()) {
        connect(deckB->getPerformancePads(), &PerformancePads::cuePointsChanged, deckB->getWaveform(), &DeckWaveformOverview::setCuePoints);
    }
    
    // Connect loop status from decks to top waveform displays
    connect(deckA, &QtDeckWidget::loopChanged, overviewTopA, &WaveformDisplay::setLoopRegion);
    connect(deckB, &QtDeckWidget::loopChanged, overviewTopB, &WaveformDisplay::setLoopRegion);
    
    // Connect loop status from decks to deck waveform overviews
    if (deckA->getWaveform()) {
        connect(deckA, &QtDeckWidget::loopChanged, deckA->getWaveform(), &DeckWaveformOverview::setLoopRegion);
    }
    if (deckB->getWaveform()) {
        connect(deckB, &QtDeckWidget::loopChanged, deckB->getWaveform(), &DeckWaveformOverview::setLoopRegion);
    }
    
    // Connect ghost loop status from performance pads to top waveform displays
    if (deckA->getPerformancePads()) {
        connect(deckA->getPerformancePads(), &PerformancePads::ghostLoopChanged, overviewTopA, &WaveformDisplay::setGhostLoopRegion);
    }
    if (deckB->getPerformancePads()) {
        connect(deckB->getPerformancePads(), &PerformancePads::ghostLoopChanged, overviewTopB, &WaveformDisplay::setGhostLoopRegion);
    }
    
    // Connect ghost loop status from performance pads to deck waveform overviews
    if (deckA->getPerformancePads() && deckA->getWaveform()) {
        connect(deckA->getPerformancePads(), &PerformancePads::ghostLoopChanged, deckA->getWaveform(), &DeckWaveformOverview::setGhostLoopRegion);
    }
    if (deckB->getPerformancePads() && deckB->getWaveform()) {
        connect(deckB->getPerformancePads(), &PerformancePads::ghostLoopChanged, deckB->getWaveform(), &DeckWaveformOverview::setGhostLoopRegion);
    }
    
    // Also update BeatIndicator per-deck when tempo factor changes and follow sync
    connect(deckA, &QtDeckWidget::tempoFactorChanged, this, [this](double factor){
        if (beatIndicator) beatIndicator->setTempoFactorDeckA(factor);
        // FEEDBACK PROTECTION: Prevent sync loops
        if (syncUpdateInProgress) return;
        // If Deck B is set to follow (sync enabled on B), update B to A
        if (syncBEnabled && deckB && deckA) {
            double masterBpm = deckA->getDetectedBpm();
            double masterEff = masterBpm > 0 ? masterBpm * deckA->getTempoFactor() : 0.0;
            double targetBpm = deckB->getDetectedBpm();
            if (masterEff > 0.0 && targetBpm > 0.0) {
                double desired = masterEff / targetBpm;
                syncUpdateInProgress = true; // Prevent feedback loop
                
                // SYNC FIX: Apply to both audio and visual immediately
                deckB->setTempoFactor(desired);
                if (playerB) playerB->setSpeed(desired); // Direct audio sync
                
                // Force immediate waveform update for visual sync
                if (overviewTopB) {
                    overviewTopB->setTempoFactor(desired);
                    overviewTopB->update();
                }
                syncUpdateInProgress = false;
            }
        }
    });
    connect(deckB, &QtDeckWidget::tempoFactorChanged, this, [this](double factor){
        if (beatIndicator) beatIndicator->setTempoFactorDeckB(factor);
        // FEEDBACK PROTECTION: Prevent sync loops
        if (syncUpdateInProgress) return;
        // If Deck A is set to follow (sync enabled on A), update A to B
        if (syncAEnabled && deckA && deckB) {
            double masterBpm = deckB->getDetectedBpm();
            double masterEff = masterBpm > 0 ? masterBpm * deckB->getTempoFactor() : 0.0;
            double targetBpm = deckA->getDetectedBpm();
            if (masterEff > 0.0 && targetBpm > 0.0) {
                double desired = masterEff / targetBpm;
                syncUpdateInProgress = true; // Prevent feedback loop
                
                // SYNC FIX: Apply to both audio and visual immediately
                deckA->setTempoFactor(desired);
                if (playerA) playerA->setSpeed(desired); // Direct audio sync
                
                // Force immediate waveform update for visual sync
                if (overviewTopA) {
                    overviewTopA->setTempoFactor(desired);
                    overviewTopA->update();
                }
                syncUpdateInProgress = false;
            }
        }
    });

    // Synchronize zoom levels between all waveform displays
    connect(overviewTopA, &WaveformDisplay::zoomLevelChanged, this, [this](int newLevel) {
        // Synchronize all other waveform displays to the same zoom level
        if (overviewTopB) {
            overviewTopB->setBeatGridZoomLevel(newLevel);
        }
    });
    connect(overviewTopB, &WaveformDisplay::zoomLevelChanged, this, [this](int newLevel) {
        // Synchronize all other waveform displays to the same zoom level
        if (overviewTopA) {
            overviewTopA->setBeatGridZoomLevel(newLevel);
        }
    });

    // SYNC wiring: when a deck requests sync, match its tempo and phase to the other deck
    auto doSync = [this](QtDeckWidget* requester){
        if (!requester || !deckA || !deckB || !playerA || !playerB) return;
        // Determine master and target
        QtDeckWidget* masterDeck = (requester == deckA) ? deckB : deckA;
        QtDeckWidget* targetDeck = requester;
        DJAudioPlayer* masterPlayer = (requester == deckA) ? playerB : playerA;
        DJAudioPlayer* targetPlayer = (requester == deckA) ? playerA : playerB;

        // Compute target tempo factor so target effective BPM equals master's effective BPM
        double masterBpm = masterDeck->getDetectedBpm();
        double masterFactor = masterDeck->getTempoFactor();
        double targetBpm = targetDeck->getDetectedBpm();
        if (masterBpm <= 0.0 || targetBpm <= 0.0) return; // need BPM info on both
        double masterEffective = masterBpm * masterFactor;
        double desiredFactor = masterEffective / targetBpm;
        
        // Apply tempo precisely (not limited by slider quantization)
        targetDeck->setTempoFactor(desiredFactor);

        // CRITICAL: Force waveform displays to update with new tempo factor immediately
        if (requester == deckA && overviewTopA) {
            overviewTopA->setTempoFactor(desiredFactor);
            overviewTopA->update();
        } else if (requester == deckB && overviewTopB) {
            overviewTopB->setTempoFactor(desiredFactor);
            overviewTopB->update();
        }

        // Optional: phase align target to master strong beat (beat 1) while keeping play state
        double mBpm = masterPlayer->getTrackBpm();
        double mOffset = masterPlayer->getFirstBeatOffset();
        double tBpm = targetPlayer->getTrackBpm();
        double tOffset = targetPlayer->getFirstBeatOffset();
        if (mBpm > 0.0 && tBpm > 0.0) {
            double mBeatLen = 60.0 / (mBpm * masterFactor);
            // Current absolute times
            double mTime = masterPlayer->getCurrentPositionSeconds();
            double tTime = targetPlayer->getCurrentPositionSeconds();
            // Phase within bar [0, beatLen)
            auto phase = [](double time, double offset, double beatLen){
                double rel = time - offset;
                double mod = std::fmod(rel, beatLen);
                if (mod < 0) mod += beatLen;
                return mod;
            };
            double masterPhase = phase(mTime, mOffset, mBeatLen);
            // Target beat length at its new speed equals master beat length (by construction)
            double targetPhase = phase(tTime, tOffset, mBeatLen);
            double delta = masterPhase - targetPhase; // seconds to nudge
            // Snap delta to nearest beat to avoid large jumps
            if (std::abs(delta) > mBeatLen/2.0) {
                if (delta > 0) delta -= mBeatLen; else delta += mBeatLen;
            }
            double newTime = tTime + delta;
            // Clamp within track
            newTime = std::clamp(newTime, 0.0, targetPlayer->getLengthInSeconds());
            targetPlayer->setPositionSeconds(newTime);
        }
    };
    connect(deckA, &QtDeckWidget::syncRequested, this, doSync);
    connect(deckB, &QtDeckWidget::syncRequested, this, doSync);

    // SYNC toggle: enable/disable follow mode
    connect(deckA, &QtDeckWidget::syncToggled, this, [this, doSync](QtDeckWidget* who, bool enabled){
        // who == deckA means A wants to follow B when enabled
        syncAEnabled = enabled;
        if (enabled) doSync(who); // immediate align
    });
    connect(deckB, &QtDeckWidget::syncToggled, this, [this, doSync](QtDeckWidget* who, bool enabled){
        // who == deckB means B wants to follow A when enabled
        syncBEnabled = enabled;
        if (enabled) doSync(who); // immediate align
    });

    // Update overview labels to show only original analyzed BPM (not speed-scaled)
    connect(deckA, &QtDeckWidget::displayedBpmChanged, this, [this](double displayed){
    Q_UNUSED(displayed);
    updateOverviewLabel(true);
    });
    connect(deckB, &QtDeckWidget::displayedBpmChanged, this, [this](double displayed){
    Q_UNUSED(displayed);
    updateOverviewLabel(false);
    });

    // Defer audio device initialization until after Qt setup
    QTimer::singleShot(100, this, [this]() {
        initializeAudio();
    });

    // Initialize new LibraryManager with ID3 tag support
    libraryManager = new LibraryManager(sharedFormatManager, this);
    
    // Connect library manager signals to deck loading
    connect(libraryManager, &LibraryManager::fileSelected, this, [this](const QString& filePath) {
        // Double-click loads to the currently focused deck or deck A by default
        if (deckA && deckA->hasFocus()) {
            deckA->loadFile(filePath);
        } else if (deckB && deckB->hasFocus()) {
            deckB->loadFile(filePath);
        } else {
            // Default to deck A if no deck has focus
            if (deckA) deckA->loadFile(filePath);
        }
    });

    // Explicit context-menu deck loading
    connect(libraryManager, &LibraryManager::loadToDeck, this, &QtMainWindow::onLibraryLoadToDeck);
    connect(libraryManager, &LibraryManager::analyzeTracksRequested, this, &QtMainWindow::onAnalyzeTracksRequested);
    connect(libraryManager, &LibraryManager::analyzeTracksAdvancedRequested, this, &QtMainWindow::onAnalyzeTracksAdvancedRequested);
    
    // Auto-populate with user's Music folder on startup
    QTimer::singleShot(500, this, [this]() {
        QDir musicDir(QDir::homePath());
        musicDir.cd("Music");
        if (musicDir.exists()) {
            libraryManager->addDirectory(musicDir.absolutePath(), false); // Non-recursive for quick startup
        }
    });

    crossfader = new QSlider(Qt::Horizontal, this);
    crossfader->setRange(0, 100);
    crossfader->setValue(50);
    connect(crossfader, &QSlider::valueChanged, this, &QtMainWindow::onCrossfader);

    // Rekordbox-style layout with Serato-style overview waveforms at top
    // Top section: Two stacked overview waveforms (Serato style)
    auto overviewLayout = new QVBoxLayout;
    
    // Style the overview waveforms (increase height ~2x as requested)
    overviewTopA->setFixedHeight(70);
    overviewTopB->setFixedHeight(70);
    overviewTopA->setStyleSheet("border: 1px solid #333; background-color: #0a0a0a;");
    overviewTopB->setStyleSheet("border: 1px solid #333; background-color: #0a0a0a;");
    
    // Add labels for the overviews (smaller)
    deckALabel = new QLabel("DECK A - OVERVIEW", this);
    deckBLabel = new QLabel("DECK B - OVERVIEW", this);
    deckALabel->setStyleSheet("color: #0088ff; font-weight: bold; font-size: 9px; padding: 1px;");
    deckBLabel->setStyleSheet("color: #ff8800; font-weight: bold; font-size: 9px; padding: 1px;");
    // Keep overview labels from changing the main window size when their text changes
    deckALabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    deckBLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    deckALabel->setFixedHeight(16);
    deckBLabel->setFixedHeight(16);
    
    overviewLayout->setSpacing(2);
    overviewLayout->addWidget(deckALabel);
    overviewLayout->addWidget(overviewTopA);
    overviewLayout->addWidget(deckBLabel);
    overviewLayout->addWidget(overviewTopB);
    
    // Main deck controls side by side (more compact spacing)
    auto decksLayout = new QHBoxLayout;
    decksLayout->setSpacing(8);
    decksLayout->addWidget(deckA->getControlsWidget(), 2);
    
    // Mixer section in the center (more compact)
    auto mixerSection = new QVBoxLayout;
    mixerSection->setSpacing(4);
    auto crossfaderLabel = new QLabel("CROSSFADER", this);
    crossfaderLabel->setAlignment(Qt::AlignCenter);
    crossfaderLabel->setStyleSheet("font-weight: bold; color: #fff; font-size: 10px;");
    crossfaderLabel->setFixedHeight(16);
    mixerSection->addWidget(crossfaderLabel);
    mixerSection->addWidget(crossfader);
    
    // Add EQ knobs: top to bottom High, Mid, Low, then Filter (smaller)
    auto eqLayout = new QHBoxLayout;
    eqLayout->setSpacing(4);
    auto leftEqLayout = new QVBoxLayout;
    leftEqLayout->setSpacing(2);
    auto rightEqLayout = new QVBoxLayout;
    rightEqLayout->setSpacing(2);

    leftHigh = new QDial(this);
    leftHigh->setRange(-100, 100);
    leftHigh->setNotchesVisible(true);
    leftHigh->setToolTip("Left High");
    leftMid = new QDial(this);
    leftMid->setRange(-100, 100);
    leftMid->setNotchesVisible(true);
    leftMid->setToolTip("Left Mid");
    leftLow = new QDial(this);
    leftLow->setRange(-100, 100);
    leftLow->setNotchesVisible(true);
    leftLow->setToolTip("Left Low");
    leftFilter = new QDial(this);
    leftFilter->setRange(-100, 100);
    leftFilter->setNotchesVisible(true);
    leftFilter->setToolTip("Left Filter");

    rightHigh = new QDial(this);
    rightHigh->setRange(-100, 100);
    rightHigh->setNotchesVisible(true);
    rightHigh->setToolTip("Right High");
    rightMid = new QDial(this);
    rightMid->setRange(-100, 100);
    rightMid->setNotchesVisible(true);
    rightMid->setToolTip("Right Mid");
    rightLow = new QDial(this);
    rightLow->setRange(-100, 100);
    rightLow->setNotchesVisible(true);
    rightLow->setToolTip("Right Low");
    rightFilter = new QDial(this);
    rightFilter->setRange(-100, 100);
    rightFilter->setNotchesVisible(true);
    rightFilter->setToolTip("Right Filter");

    // Set all knobs to start centered (0) and make them smaller
    leftHigh->setValue(0); leftMid->setValue(0); leftLow->setValue(0); leftFilter->setValue(0);
    rightHigh->setValue(0); rightMid->setValue(0); rightLow->setValue(0); rightFilter->setValue(0);
    
    // Make knobs smaller
    leftHigh->setFixedSize(35, 35); leftMid->setFixedSize(35, 35); 
    leftLow->setFixedSize(35, 35); leftFilter->setFixedSize(35, 35);
    rightHigh->setFixedSize(35, 35); rightMid->setFixedSize(35, 35);
    rightLow->setFixedSize(35, 35); rightFilter->setFixedSize(35, 35);

    // Arrange top-to-bottom: High, Mid, Low, Filter
    leftEqLayout->addWidget(leftHigh);
    leftEqLayout->addWidget(leftMid);
    leftEqLayout->addWidget(leftLow);
    leftEqLayout->addWidget(leftFilter);

    rightEqLayout->addWidget(rightHigh);
    rightEqLayout->addWidget(rightMid);
    rightEqLayout->addWidget(rightLow);
    rightEqLayout->addWidget(rightFilter);

    eqLayout->addLayout(leftEqLayout);
    eqLayout->addLayout(rightEqLayout);

    mixerSection->addLayout(eqLayout);
    
    // Add volume sliders below the filter knobs
    auto volumeLayout = new QHBoxLayout;
    volumeLayout->setSpacing(4);
    
    auto leftVolLayout = new QVBoxLayout;
    leftVolLayout->setSpacing(1);
    leftVolLayout->setAlignment(Qt::AlignCenter);
    auto leftVolLabel = new QLabel("Vol A", this);
    leftVolLabel->setAlignment(Qt::AlignCenter);
    leftVolLabel->setStyleSheet("color: #fff; font-size: 9px; font-weight: bold;");
    leftVolLabel->setFixedHeight(12);
    
    leftVolumeSlider = new QSlider(Qt::Vertical, this);
    leftVolumeSlider->setRange(0, 100);
    leftVolumeSlider->setValue(100);
    leftVolumeSlider->setFixedHeight(60);
    leftVolumeSlider->setFixedWidth(20);
    
    leftVolLayout->addWidget(leftVolLabel);
    leftVolLayout->addWidget(leftVolumeSlider);
    
    auto rightVolLayout = new QVBoxLayout;
    rightVolLayout->setSpacing(1);
    rightVolLayout->setAlignment(Qt::AlignCenter);
    auto rightVolLabel = new QLabel("Vol B", this);
    rightVolLabel->setAlignment(Qt::AlignCenter);
    rightVolLabel->setStyleSheet("color: #fff; font-size: 9px; font-weight: bold;");
    rightVolLabel->setFixedHeight(12);
    
    rightVolumeSlider = new QSlider(Qt::Vertical, this);
    rightVolumeSlider->setRange(0, 100);
    rightVolumeSlider->setValue(100);
    rightVolumeSlider->setFixedHeight(60);
    rightVolumeSlider->setFixedWidth(20);
    
    rightVolLayout->addWidget(rightVolLabel);
    rightVolLayout->addWidget(rightVolumeSlider);
    
    volumeLayout->addLayout(leftVolLayout);
    volumeLayout->addLayout(rightVolLayout);
    
    mixerSection->addLayout(volumeLayout);
    mixerSection->addStretch();
    
    auto mixerWidget = new QWidget(this);
    mixerWidget->setLayout(mixerSection);
    mixerWidget->setFixedWidth(130);
    mixerWidget->setStyleSheet("background-color: #2a2a2a; border: 1px solid #555; border-radius: 0px;");
    
    decksLayout->addWidget(mixerWidget, 1);
    decksLayout->addWidget(deckB->getControlsWidget(), 2);

    // Connect knobs to slots to control EQ and filter
    connect(leftHigh, &QDial::valueChanged, this, &QtMainWindow::onLeftHighChanged);
    connect(leftMid, &QDial::valueChanged, this, &QtMainWindow::onLeftMidChanged);
    connect(leftLow, &QDial::valueChanged, this, &QtMainWindow::onLeftLowChanged);
    connect(leftFilter, &QDial::valueChanged, this, &QtMainWindow::onLeftFilterChanged);

    connect(rightHigh, &QDial::valueChanged, this, &QtMainWindow::onRightHighChanged);
    connect(rightMid, &QDial::valueChanged, this, &QtMainWindow::onRightMidChanged);
    connect(rightLow, &QDial::valueChanged, this, &QtMainWindow::onRightLowChanged);
    connect(rightFilter, &QDial::valueChanged, this, &QtMainWindow::onRightFilterChanged);
    
    // Add double-click reset functionality for all mixer controls
    // EQ controls reset to 0 (neutral)
    leftHigh->installEventFilter(this);
    leftMid->installEventFilter(this);
    leftLow->installEventFilter(this);
    leftFilter->installEventFilter(this);
    rightHigh->installEventFilter(this);
    rightMid->installEventFilter(this);
    rightLow->installEventFilter(this);
    rightFilter->installEventFilter(this);
    
    // Volume sliders reset to 100 (full volume)
    leftVolumeSlider->installEventFilter(this);
    rightVolumeSlider->installEventFilter(this);
    
    // Crossfader resets to 50 (center)
    crossfader->installEventFilter(this);
    
    // Connect volume sliders
    connect(leftVolumeSlider, &QSlider::valueChanged, this, &QtMainWindow::onLeftVolumeChanged);
    connect(rightVolumeSlider, &QSlider::valueChanged, this, &QtMainWindow::onRightVolumeChanged);
    
    // Bottom section: Library (now with LibraryManager)
    auto libLayout = new QVBoxLayout;
    auto libraryLabel = new QLabel("MUSIC LIBRARY", this);
    libraryLabel->setStyleSheet("font-weight: bold; font-size: 14px; color: #fff; padding: 5px;");
    libLayout->addWidget(libraryLabel);
    libLayout->addWidget(libraryManager, 1);

    // Main layout: Vertical stack (Compact Serato style)
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(3);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    
    // Add menu bar to the top of the layout
    mainLayout->addWidget(menuBar);
    
    // Beat indicator at the very top
    auto beatIndicatorLayout = new QHBoxLayout;
    beatIndicatorLayout->addStretch();
    beatIndicatorLayout->addWidget(beatIndicator);
    beatIndicatorLayout->addStretch();
    mainLayout->addLayout(beatIndicatorLayout, 0);
    
    mainLayout->addLayout(overviewLayout, 0);    // Overview waveforms at top (fixed size)
    mainLayout->addLayout(decksLayout, 2);       // Deck controls + mixer (reduced from 4 to 2)
    mainLayout->addLayout(libLayout, 2);         // Library at bottom (increased from 1 to 2)
    setLayout(mainLayout);

    // PREROLL SUPPORT: Timer for automatic position updates during playback
    positionUpdateTimer = new QTimer(this);
    positionUpdateTimer->setInterval(80); // Slower for even more stability (12.5 FPS)
    connect(positionUpdateTimer, &QTimer::timeout, this, &QtMainWindow::updatePlaybackPositions);
    positionUpdateTimer->start();
    std::cout << "Position update timer started at 12.5 FPS for maximum stability" << std::endl;

    // BetaPulseX: Lade alle Deck-Einstellungen aus zentraler Config
    {
        // Stelle sicher, dass Verzeichnisse existieren
        AppConfig::instance().createDirectories();
        
        // Lade zentrale Deck-Settings
        DeckSettings::instance().loadSettings();
        
        // Extrahiere Visual Trim für Legacy-Kompatibilität
        userVisualTrimA = std::clamp(DeckSettings::instance().getDeckA().visualTrim, -0.05, 0.05);
        userVisualTrimB = std::clamp(DeckSettings::instance().getDeckB().visualTrim, -0.05, 0.05);
        
        updateOverviewLabel(true);
        updateOverviewLabel(false);
        
        qDebug() << "BetaPulseX: All deck settings loaded successfully";
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
        bpmAnalyzer = new BpmAnalyzer(*sharedFormatManager);

    juce::File file(filePath.toStdString());
    const auto target = isDeckA ? BpmAnalysisTask::Target::DeckA : BpmAnalysisTask::Target::DeckB;
    bpmThreadPool->start(new BpmAnalysisTask(this, file, target));
}

void QtMainWindow::initializeAudio()
{
    // LINUX-FRIENDLY: Initialize audio like a normal Linux application
    // Don't try to monopolize the audio device - work with PulseAudio/PipeWire
    try {
        std::cout << "Initializing audio as normal Linux application (shared audio)..." << std::endl;
        
        // Clean any previous callbacks
        if (stereoCallback) {
            deviceManager.removeAudioCallback(stereoCallback.get());
        }
        deviceManager.removeAudioCallback(&masterLevelMonitor);
        
        // SIMPLE APPROACH: Just use default devices without trying to configure them
        // This lets PulseAudio/PipeWire handle the device management
        std::cout << "Using default audio devices (letting system handle device management)..." << std::endl;
        
        juce::String audioError = deviceManager.initialiseWithDefaultDevices(0, 2);
        if (audioError.isNotEmpty()) {
            std::cout << "Default audio init error: " << audioError.toStdString() << std::endl;
            return;
        }
        
        // DON'T try to reconfigure the device - just use what the system gives us
        auto* currentDevice = deviceManager.getCurrentAudioDevice();
        if (currentDevice) {
            std::cout << "Using system default audio device: " << currentDevice->getName().toStdString() << std::endl;
            std::cout << "Sample rate: " << currentDevice->getCurrentSampleRate() << " Hz" << std::endl;
            std::cout << "Buffer size: " << currentDevice->getCurrentBufferSizeSamples() << " samples" << std::endl;
            std::cout << "Available output channels: " << currentDevice->getActiveOutputChannels().toInteger() << std::endl;
            
            // Accept whatever the system provides - don't force anything
            int availableChannels = currentDevice->getActiveOutputChannels().toInteger();
            if (availableChannels >= 2) {
                std::cout << "SUCCESS: System provides stereo (" << availableChannels << " channels)" << std::endl;
            } else {
                std::cout << "INFO: System provides " << availableChannels << " channel(s) - will work fine" << std::endl;
            }
        } else {
            std::cout << "WARNING: No audio device available" << std::endl;
            return;
        }
        
        // Prepare players with current device settings
        if (playerA) {
            playerA->prepareToPlay(deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples(),
                                   deviceManager.getCurrentAudioDevice()->getCurrentSampleRate());
        }
        if (playerB) {
            playerB->prepareToPlay(deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples(),
                                   deviceManager.getCurrentAudioDevice()->getCurrentSampleRate());
        }
        
        // Use simple audio callback - let JUCE handle the channel mapping
        std::cout << "Setting up audio callback for playerA" << std::endl;
        stereoCallback = std::make_unique<StereoAudioCallback>(playerA, playerB);
        deviceManager.addAudioCallback(stereoCallback.get());
        
        // Add master level monitor
        deviceManager.addAudioCallback(&masterLevelMonitor);
        
        std::cout << "Audio initialization complete - app ready to play audio like normal Linux application" << std::endl;
        std::cout << "IMPORTANT: Load an audio file before pressing Play!" << std::endl;

    // Keep mixer available for future use but don't use it now
    // mixer.setMasterGain(1.0f);
    // mixer.setGainA(leftVolumeSlider ? juce::jlimit(0.0f, 1.0f, leftVolumeSlider->value() / 100.0f) : 1.0f);
    // mixer.setGainB(rightVolumeSlider ? juce::jlimit(0.0f, 1.0f, rightVolumeSlider->value() / 100.0f) : 1.0f);
    if (crossfader) onCrossfader(crossfader->value());
        
        // Optional: log play state changes without toggling routing
        connect(deckA, &QtDeckWidget::playStateChanged, this, [this](bool playing){
            std::cout << "Deck A: " << (playing ? "Playing" : "Stopped") << std::endl;
            // Ensure the window doesn't accidentally close on play state changes
            if (playing) {
                std::cout << "Starting playback - ensuring UI remains active" << std::endl;
            }
        });
        connect(deckB, &QtDeckWidget::playStateChanged, this, [this](bool playing){
            std::cout << "Deck B: " << (playing ? "Playing" : "Stopped") << std::endl;
            if (playing) {
                std::cout << "Starting playback - ensuring UI remains active" << std::endl;
            }
        });
        
        std::cout << "Audio system fully initialized" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "Exception during audio initialization: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "Unknown exception during audio initialization" << std::endl;
    }
}

QtMainWindow::~QtMainWindow()
{
    std::cout << "QtMainWindow destructor called" << std::endl;
    // Only clean up if closeEvent hasn't already done it
    if (!cleanupCompleted) {
        performCleanup();
    }
}

void QtMainWindow::performCleanup()
{
    if (cleanupCompleted) return; // Prevent double cleanup
    
    std::cout << "Performing cleanup..." << std::endl;
    try {
        // 1. Stop all audio players
        if (playerA) {
            playerA->stop();
            std::cout << "Player A stopped" << std::endl;
        }
        if (playerB) {
            playerB->stop();
            std::cout << "Player B stopped" << std::endl;
        }
        
        // 2. Remove audio callbacks before closing device
        // deviceManager.removeAudioCallback(&mixer); // Removed - no longer using mixer
        if (stereoCallback) {
            deviceManager.removeAudioCallback(stereoCallback.get());
        }
        deviceManager.removeAudioCallback(&masterLevelMonitor);
        std::cout << "Audio callbacks removed" << std::endl;
        
        // 4. No sources to disconnect (using custom callback now)
        std::cout << "No sources to disconnect (using stereo callback)" << std::endl;
        
        // 5. Close audio device
        deviceManager.closeAudioDevice();
        std::cout << "Audio device closed" << std::endl;
        
        // 6. Wait for any pending BPM analysis
        if (bpmThreadPool) {
            bpmThreadPool->waitForDone(1000); // Reduced timeout
            std::cout << "BPM thread pool finished" << std::endl;
        }
        
        // 7. Delete players safely
        delete playerA;
        playerA = nullptr;
        std::cout << "Player A deleted" << std::endl;
        
        delete playerB;
        playerB = nullptr;
        std::cout << "Player B deleted" << std::endl;
        
        delete bpmAnalyzer;
        bpmAnalyzer = nullptr;
        std::cout << "BPM analyzer deleted" << std::endl;
        
        // 8. Handle shared format manager cleanup
        formatManagerRefCount--;
        if (formatManagerRefCount <= 0 && sharedFormatManager) {
            delete sharedFormatManager;
            sharedFormatManager = nullptr;
            formatManagerRefCount = 0;
            std::cout << "Format manager cleaned up" << std::endl;
        }
        
        cleanupCompleted = true;
        std::cout << "Cleanup complete" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "Exception during cleanup: " << e.what() << std::endl;
        cleanupCompleted = true; // Mark as completed even if there was an error
    } catch (...) {
        std::cout << "Unknown exception during cleanup" << std::endl;
        cleanupCompleted = true;
    }
}

void QtMainWindow::closeEvent(QCloseEvent* event)
{
    if (shutdownInitiated)
    {
        event->accept();
        return;
    }

    QMessageBox confirmBox(this);
    confirmBox.setIcon(QMessageBox::Question);
    confirmBox.setWindowTitle(tr("BetaPulseX beenden?"));
    confirmBox.setText(tr("Möchtest du BetaPulseX wirklich schließen?\nAlle laufenden Analysen werden gestoppt und die Datenbank wird sauber getrennt."));
    confirmBox.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    confirmBox.setDefaultButton(QMessageBox::Ok);

    if (confirmBox.exec() != QMessageBox::Ok)
    {
        event->ignore();
        return;
    }

    shutdownInitiated = true;

    QProgressDialog shutdownProgress(tr("Programm wird beendet..."), QString(), 0, 0, this);
    shutdownProgress.setWindowTitle(tr("Beenden"));
    shutdownProgress.setCancelButton(nullptr);
    shutdownProgress.setWindowModality(Qt::ApplicationModal);
    shutdownProgress.setAutoClose(false);
    shutdownProgress.setAutoReset(false);
    shutdownProgress.setMinimumDuration(0);
    shutdownProgress.setLabelText(tr("Deck-Einstellungen werden gespeichert..."));
    shutdownProgress.show();
    QApplication::processEvents();

    std::cout << "QtMainWindow::closeEvent called - shutting down..." << std::endl;

    // BetaPulseX: Speichere alle Deck-Einstellungen
    try {
        DeckSettings::instance().setVisualTrim(0, userVisualTrimA);  // Deck A
        DeckSettings::instance().setVisualTrim(1, userVisualTrimB);  // Deck B

        DeckSettings::instance().saveSettings();
        qDebug() << "BetaPulseX: All deck settings saved successfully";
    } catch (...) {
        qWarning() << "Failed to save deck settings";
    }

    shutdownProgress.setLabelText(tr("Decks werden deaktiviert..."));
    QApplication::processEvents();

    if (deckA) deckA->getControlsWidget()->setEnabled(false);
    if (deckB) deckB->getControlsWidget()->setEnabled(false);

    shutdownProgress.setLabelText(tr("Audio-Engine wird heruntergefahren..."));
    QApplication::processEvents();

    performCleanup();

    shutdownProgress.setLabelText(tr("Aufräumen abgeschlossen"));
    QApplication::processEvents();
    shutdownProgress.close();

    std::cout << "Accepting close event and quitting application" << std::endl;
    event->accept();
    QApplication::quit();
}

void QtMainWindow::onCrossfader(int v) {
    std::cout << "Crossfader changed to: " << v << std::endl;
    // v: 0 => full A (left), 50 => center, 100 => full B (right)
    // Convert to -1.0f (full A) to +1.0f (full B)
    float crossPos = (float(v) - 50.0f) / 50.0f;  // -1.0 to +1.0
    if (stereoCallback) {
        stereoCallback->setCrossfader(crossPos);
    }
}

// MIDI control access method
void QtMainWindow::setCrossfaderPosition(float normalizedValue) {
    // normalizedValue: 0.0 = full A, 1.0 = full B
    // Convert to slider range: 0-100
    int sliderValue = static_cast<int>(normalizedValue * 100.0f);
    sliderValue = std::max(0, std::min(100, sliderValue));
    
    if (crossfader) {
        crossfader->setValue(sliderValue);
        // This will trigger onCrossfader automatically through the signal connection
        std::cout << "MIDI: Crossfader set to " << sliderValue << " (normalized: " << normalizedValue << ")" << std::endl;
    }
}

// MIDI control access methods for Play/Pause
void QtMainWindow::setDeckAPlayPause(bool shouldPlay) {
    if (deckA) {
        // The onPlayPause() method handles both play and pause - it toggles the state
        // For MIDI, we can trigger it when shouldPlay changes the current state
        deckA->onPlayPause();
        std::cout << "MIDI: Deck A Play/Pause triggered (target state: " << (shouldPlay ? "PLAY" : "PAUSE") << ")" << std::endl;
    }
}

void QtMainWindow::setDeckBPlayPause(bool shouldPlay) {
    if (deckB) {
        // The onPlayPause() method handles both play and pause - it toggles the state  
        // For MIDI, we can trigger it when shouldPlay changes the current state
        deckB->onPlayPause();
        std::cout << "MIDI: Deck B Play/Pause triggered (target state: " << (shouldPlay ? "PLAY" : "PAUSE") << ")" << std::endl;
    }
}

// MIDI control access methods for Tempo/Pitch
void QtMainWindow::setDeckATempo(float normalizedValue) {
    if (playerA && deckA) {
        // Get the current tempo range from the deck widget
        double minTempo = deckA->getMinTempoFactor();
        double maxTempo = deckA->getMaxTempoFactor();
        
        // Convert 0.0-1.0 MIDI range to the actual tempo range of the deck
        // 0.0 = minimum tempo, 0.5 = normal (1.0), 1.0 = maximum tempo
        double pitchValue;
        if (normalizedValue <= 0.5f) {
            // Scale from min to 1.0
            pitchValue = minTempo + (normalizedValue * 2.0f) * (1.0 - minTempo);
        } else {
            // Scale from 1.0 to max
            pitchValue = 1.0 + ((normalizedValue - 0.5f) * 2.0f) * (maxTempo - 1.0);
        }
        
        // Clamp to the deck's actual range
        pitchValue = std::max(minTempo, std::min(maxTempo, pitchValue));
        
        // Update both the player and the UI via deck widget
        deckA->setTempoFactor(pitchValue);
        std::cout << "MIDI: Deck A Tempo set to " << pitchValue << " (normalized: " << normalizedValue 
                  << ", range: " << minTempo << " - " << maxTempo << ")" << std::endl;
    }
}

void QtMainWindow::setDeckBTempo(float normalizedValue) {
    if (playerB && deckB) {
        // Get the current tempo range from the deck widget
        double minTempo = deckB->getMinTempoFactor();
        double maxTempo = deckB->getMaxTempoFactor();
        
        // Convert 0.0-1.0 MIDI range to the actual tempo range of the deck
        // 0.0 = minimum tempo, 0.5 = normal (1.0), 1.0 = maximum tempo
        double pitchValue;
        if (normalizedValue <= 0.5f) {
            // Scale from min to 1.0
            pitchValue = minTempo + (normalizedValue * 2.0f) * (1.0 - minTempo);
        } else {
            // Scale from 1.0 to max
            pitchValue = 1.0 + ((normalizedValue - 0.5f) * 2.0f) * (maxTempo - 1.0);
        }
        
        // Clamp to the deck's actual range
        pitchValue = std::max(minTempo, std::min(maxTempo, pitchValue));
        
        // Update both the player and the UI via deck widget
        deckB->setTempoFactor(pitchValue);
        std::cout << "MIDI: Deck B Tempo set to " << pitchValue << " (normalized: " << normalizedValue 
                  << ", range: " << minTempo << " - " << maxTempo << ")" << std::endl;
    }
}

// MIDI control access methods for Volume
void QtMainWindow::setDeckAVolume(float normalizedValue) {
    if (playerA && stereoCallback) {
        // Convert 0.0-1.0 to volume range: 0.0 = mute, 1.0 = full volume
        float volumeValue = std::max(0.0f, std::min(1.0f, normalizedValue));
        
        // Update the mixer volume for Deck A
        stereoCallback->setVolumeA(volumeValue);
        std::cout << "MIDI: Deck A Volume set to " << volumeValue << " (normalized: " << normalizedValue << ")" << std::endl;
    }
}

void QtMainWindow::setDeckBVolume(float normalizedValue) {
    if (playerB && stereoCallback) {
        // Convert 0.0-1.0 to volume range: 0.0 = mute, 1.0 = full volume
        float volumeValue = std::max(0.0f, std::min(1.0f, normalizedValue));
        
        // Update the mixer volume for Deck B
        stereoCallback->setVolumeB(volumeValue);
        std::cout << "MIDI: Deck B Volume set to " << volumeValue << " (normalized: " << normalizedValue << ")" << std::endl;
    }
}

// EQ/filter slot implementations
void QtMainWindow::onLeftHighChanged(int v) {
    std::cout << "onLeftHighChanged called with value: " << v << std::endl;
    // map -100..100 to -1.0..1.0
    double val = v / 100.0;
    if (playerA) {
        std::cout << "  Calling playerA->setHighGain(" << val << ")" << std::endl;
        playerA->setHighGain(val);
    } else {
        std::cout << "  ERROR: playerA is null!" << std::endl;
    }
}

void QtMainWindow::onLeftMidChanged(int v) {
    std::cout << "onLeftMidChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerA) {
        std::cout << "  Calling playerA->setMidGain(" << val << ")" << std::endl;
        playerA->setMidGain(val);
    } else {
        std::cout << "  ERROR: playerA is null!" << std::endl;
    }
}

void QtMainWindow::onLeftLowChanged(int v) {
    std::cout << "onLeftLowChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerA) playerA->setLowGain(val);
}

void QtMainWindow::onLeftFilterChanged(int v) {
    std::cout << "onLeftFilterChanged called with value: " << v << std::endl;
    // map -100..100 to -1..1 (center 0 = bypass)
    double norm = v / 100.0;
    if (playerA) {
        std::cout << "  Calling playerA->setFilterCutoff(" << norm << ")" << std::endl;
        playerA->setFilterCutoff(norm);
    } else {
        std::cout << "  ERROR: playerA is null!" << std::endl;
    }
}

void QtMainWindow::onRightHighChanged(int v) {
    std::cout << "onRightHighChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerB) playerB->setHighGain(val);
}

void QtMainWindow::onRightMidChanged(int v) {
    std::cout << "onRightMidChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerB) playerB->setMidGain(val);
}

void QtMainWindow::onRightLowChanged(int v) {
    std::cout << "onRightLowChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerB) playerB->setLowGain(val);
}

void QtMainWindow::onRightFilterChanged(int v) {
    // map -100..100 to -1..1 (center 0 = bypass)
    double norm = v / 100.0;
    if (playerB) playerB->setFilterCutoff(norm);
}

void QtMainWindow::onLeftVolumeChanged(int v) {
    std::cout << "Left volume changed to: " << v << std::endl;
    if (stereoCallback) {
        float volume = juce::jlimit(0.0f, 1.0f, (float)v / 100.0f);
        stereoCallback->setVolumeA(volume);
    }
}

void QtMainWindow::onRightVolumeChanged(int v) {
    std::cout << "Right volume changed to: " << v << std::endl;
    if (stereoCallback) {
        float volume = juce::jlimit(0.0f, 1.0f, (float)v / 100.0f);
        stereoCallback->setVolumeB(volume);
    }
}

void QtMainWindow::keyPressEvent(QKeyEvent* event) {
    // Check if focus is on a line edit or text widget to avoid interfering with text input
    QWidget* focusWidget = QApplication::focusWidget();
    if (focusWidget && (qobject_cast<QLineEdit*>(focusWidget) || 
                       qobject_cast<QTextEdit*>(focusWidget) ||
                       qobject_cast<QPlainTextEdit*>(focusWidget))) {
        // Let the focused text widget handle the key event
        QWidget::keyPressEvent(event);
        return;
    }

    // Global keyboard shortcuts for beat grid zoom
    switch (event->key()) {
        case Qt::Key_F5: // Deck A: -1 ms
            userVisualTrimA = std::clamp(userVisualTrimA - 0.001, -0.05, 0.05);
            updateOverviewLabel(true);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckA", userVisualTrimA);
            }
            event->accept();
            break;
        case Qt::Key_F6: // Deck A: +1 ms
            userVisualTrimA = std::clamp(userVisualTrimA + 0.001, -0.05, 0.05);
            updateOverviewLabel(true);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckA", userVisualTrimA);
            }
            event->accept();
            break;
        case Qt::Key_F7: // Deck B: -1 ms
            userVisualTrimB = std::clamp(userVisualTrimB - 0.001, -0.05, 0.05);
            updateOverviewLabel(false);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckB", userVisualTrimB);
            }
            event->accept();
            break;
        case Qt::Key_F8: // Deck B: +1 ms
            userVisualTrimB = std::clamp(userVisualTrimB + 0.001, -0.05, 0.05);
            updateOverviewLabel(false);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckB", userVisualTrimB);
            }
            event->accept();
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:  // Handle both + and = key (same physical key)
            // Increase beat grid zoom on both waveforms
            if (overviewTopA) {
                overviewTopA->increaseBeatGridZoom();
            }
            if (overviewTopB) {
                overviewTopB->increaseBeatGridZoom();
            }
            event->accept();
            break;
            
        case Qt::Key_Minus:
        case Qt::Key_Underscore:  // Handle both - and _ key (same physical key)
            // Decrease beat grid zoom on both waveforms
            if (overviewTopA) {
                overviewTopA->decreaseBeatGridZoom();
            }
            if (overviewTopB) {
                overviewTopB->decreaseBeatGridZoom();
            }
            event->accept();
            break;
            
        case Qt::Key_0:
            // Reset beat grid zoom on both waveforms
            if (overviewTopA) {
                overviewTopA->resetBeatGridZoom();
            }
            if (overviewTopB) {
                overviewTopB->resetBeatGridZoom();
            }
            event->accept();
            break;
            
        default:
            // Let the base class handle other keys
            QWidget::keyPressEvent(event);
            break;
    }
}

void QtMainWindow::handleBpmAnalysisResult(double bpm, const std::vector<double>& beatsSec, double totalSec, 
                                         const std::string& algorithm, double firstBeatOffset, bool isDeckA) {
    QString analyzedFilePath;

    if (isDeckA) {
        // Skip applying if deck has no file anymore
        if (!deckA || deckA->getCurrentFilePath().isEmpty()) {
            if (beatIndicator) beatIndicator->setBeatGridAvailableDeckA(false);
            return;
        }
        analyzedFilePath = deckA->getCurrentFilePath();
        // Handle Deck A results
        if (deckA) deckA->setDetectedBpm(bpm);
        if (deckA && deckA->getWaveform()) {
            deckA->getWaveform()->setBeatInfo(bpm, firstBeatOffset, totalSec);
        }
        if (playerA) {
            playerA->setBeatInfo(bpm, firstBeatOffset, totalSec);
        }
        // Update beat indicator with per-deck BPM and first beat offset
        if (beatIndicator) {
            beatIndicator->setBpmDeckA(bpm);
            beatIndicator->setFirstBeatOffsetDeckA(firstBeatOffset);
        }
        if (overviewTopA) {
            overviewTopA->setOriginalBpm(bpm, totalSec);
            if (totalSec > 0.0 && !beatsSec.empty()) {
                QVector<double> rel;
                rel.reserve((int)beatsSec.size());
                for (double t : beatsSec) rel.append(t / totalSec);
                overviewTopA->setBeats(rel);
            }
        }
        if (beatIndicator) { beatIndicator->setBeatGridAvailableDeckA(bpm > 0.0); }
        if (deckALabel) {
            algorithmA = QString::fromStdString(algorithm);
            updateOverviewLabel(true);
        }
    } else {
        if (!deckB || deckB->getCurrentFilePath().isEmpty()) {
            if (beatIndicator) beatIndicator->setBeatGridAvailableDeckB(false);
            return;
        }
        analyzedFilePath = deckB->getCurrentFilePath();
        // Handle Deck B results
        if (deckB) deckB->setDetectedBpm(bpm);
        if (deckB && deckB->getWaveform()) {
            deckB->getWaveform()->setBeatInfo(bpm, firstBeatOffset, totalSec);
        }
        if (playerB) {
            playerB->setBeatInfo(bpm, firstBeatOffset, totalSec);
        }
        // Update beat indicator with per-deck BPM and first beat offset
        if (beatIndicator) {
            beatIndicator->setBpmDeckB(bpm);
            beatIndicator->setFirstBeatOffsetDeckB(firstBeatOffset);
        }
        if (overviewTopB) {
            overviewTopB->setOriginalBpm(bpm, totalSec);
            if (totalSec > 0.0 && !beatsSec.empty()) {
                QVector<double> rel;
                rel.reserve((int)beatsSec.size());
                for (double t : beatsSec) rel.append(t / totalSec);
                overviewTopB->setBeats(rel);
            }
        }
        if (beatIndicator) { beatIndicator->setBeatGridAvailableDeckB(bpm > 0.0); }
        if (deckBLabel) {
            algorithmB = QString::fromStdString(algorithm);
            updateOverviewLabel(false);
        }
    }

    if (libraryManager && !analyzedFilePath.isEmpty()) {
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
    QLabel* lbl = isDeckA ? deckALabel : deckBLabel;
    if (!lbl) return;
    bool active = isDeckA ? analysisActiveA : analysisActiveB;
    bool failed = isDeckA ? analysisFailedA : analysisFailedB;
    double prog = isDeckA ? analysisProgressA : analysisProgressB;
    double originalBpm = (isDeckA ? overviewTopA : overviewTopB) ? (isDeckA ? overviewTopA->originalBpm : overviewTopB->originalBpm) : 0.0;
    QString algText = (isDeckA ? algorithmA : algorithmB).isEmpty() ? QString("") : QString(" - %1").arg(isDeckA ? algorithmA : algorithmB);
    QString prefix = isDeckA ? "DECK A - OVERVIEW" : "DECK B - OVERVIEW";
    QString suffix;
    if (active) {
        int percent = (int)std::round(prog * 100.0);
        suffix = QString(" (Analyzing %1%)").arg(percent);
    } else if (failed) {
        suffix = QString(" (Analysis failed)");
    } else {
    const double trimMs = (isDeckA ? userVisualTrimA : userVisualTrimB) * 1000.0;
    QString trimText = (std::abs(trimMs) > 0.0001) ? QString("  |  trim %1 ms").arg(QString::number(trimMs, 'f', 1)) : QString("");
    suffix = QString(" (BPM: %1%2)%3")
             .arg(originalBpm > 0.0 ? QString::number((int)std::round(originalBpm)) : QString("--"))
             .arg(algText)
             .arg(trimText);
    }
    lbl->setText(prefix + "  " + suffix);
}

// Window drag functionality for frameless window
void QtMainWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        // Only allow dragging from the menubar area (top 30 pixels)
        if (event->pos().y() <= 30) {
            isDragging = true;
            dragStartPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void QtMainWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (isDragging && (event->buttons() & Qt::LeftButton)) {
    move(event->globalPosition().toPoint() - dragStartPosition);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void QtMainWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        isDragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

// BetaPulseX: Wendet geladene Deck-Einstellungen auf die UI-Controls an
void QtMainWindow::applyDeckSettings() {
    std::cout << "*** applyDeckSettings() CALLED ***" << std::endl;
    if (!deckA || !deckB) {
        std::cout << "*** ERROR: Cannot apply deck settings - deck widgets not created yet ***" << std::endl;
        qWarning() << "Cannot apply deck settings - deck widgets not created yet";
        return;
    }
    
    qDebug() << "BetaPulseX: Applying deck settings to UI controls";
    
    // Deck A Settings anwenden
    const auto& configA = DeckSettings::instance().getDeckA();
    
    // Keylock & Quantize für Deck A
    if (deckA->getKeylockButton()) {
        deckA->getKeylockButton()->setChecked(configA.keylockEnabled);
        // Trigger the audio player update directly
        if (playerA) {
            playerA->setKeylockEnabled(configA.keylockEnabled);
        }
    }
    
    if (deckA->getQuantizeButton()) {
        deckA->getQuantizeButton()->setChecked(configA.quantizeEnabled);
        if (playerA) {
            playerA->setQuantizeEnabled(configA.quantizeEnabled);
        }
    }
    
    // Speed für Deck A (mit sicherem Null-Check und Range-Check)
    if (deckA->getSpeedSlider() && deckA->getSpeedSlider()->isEnabled()) {
        int speedValue = (int)(configA.speedFactor * 1000.0);
        speedValue = std::clamp(speedValue, 840, 1160); // Sicherheit: Range-Check
        deckA->getSpeedSlider()->setValue(speedValue);
    }
    
    // EQ für Deck A - IMMER auf neutral (0) setzen, da nicht mehr gespeichert
    if (leftHigh && leftMid && leftLow) {
        leftHigh->setValue(0);    // Neutral position
        leftMid->setValue(0);     // Neutral position  
        leftLow->setValue(0);     // Neutral position
    }
    
    // Filter für Deck A - IMMER auf neutral (0) setzen
    if (leftFilter) {
        leftFilter->setValue(0);  // Neutral position
    }
    
    // Volume für Deck A - IMMER auf 100% setzen, da nicht mehr gespeichert
    if (leftVolumeSlider) {
        leftVolumeSlider->setValue(100);  // Full volume
    }
    
    // Deck B Settings anwenden
    const auto& configB = DeckSettings::instance().getDeckB();
    
    // Keylock & Quantize für Deck B (nur Button-Status und Player direkt setzen)
    if (deckB->getKeylockButton()) {
        deckB->getKeylockButton()->setChecked(configB.keylockEnabled);
        if (playerB) {
            playerB->setKeylockEnabled(configB.keylockEnabled);
        }
    }
    
    if (deckB->getQuantizeButton()) {
        deckB->getQuantizeButton()->setChecked(configB.quantizeEnabled);
        if (playerB) {
            playerB->setQuantizeEnabled(configB.quantizeEnabled);
        }
    }
    
    // Speed für Deck B (mit sicherem Null-Check und Range-Check)
    if (deckB->getSpeedSlider() && deckB->getSpeedSlider()->isEnabled()) {
        int speedValue = (int)(configB.speedFactor * 1000.0);
        speedValue = std::clamp(speedValue, 840, 1160); // Sicherheit: Range-Check
        deckB->getSpeedSlider()->setValue(speedValue);
    }
    
    // EQ für Deck B - IMMER auf neutral (0) setzen, da nicht mehr gespeichert
    if (rightHigh && rightMid && rightLow) {
        rightHigh->setValue(0);   // Neutral position
        rightMid->setValue(0);    // Neutral position
        rightLow->setValue(0);    // Neutral position
    }
    
    // Filter für Deck B - IMMER auf neutral (0) setzen
    if (rightFilter) {
        rightFilter->setValue(0); // Neutral position
    }
    
    // Volume für Deck B - IMMER auf 100% setzen, da nicht mehr gespeichert
    if (rightVolumeSlider) {
        rightVolumeSlider->setValue(100); // Full volume
    }
    
    qDebug() << "BetaPulseX: Deck settings applied successfully";
    qDebug() << "  Deck A: Keylock=" << configA.keylockEnabled << "Quantize=" << configA.quantizeEnabled << "Speed=" << configA.speedFactor;
    qDebug() << "  Deck B: Keylock=" << configB.keylockEnabled << "Quantize=" << configB.quantizeEnabled << "Speed=" << configB.speedFactor;
    
    // Verbinde Settings-Updates für automatisches Speichern
    connectDeckSettings();
}

// BetaPulseX: Verbindet UI-Controls mit dem Settings-System für automatisches Speichern
void QtMainWindow::connectDeckSettings() {
    std::cout << "*** connectDeckSettings() STARTED ***" << std::endl;
    qDebug() << "BetaPulseX: Connecting deck controls to settings system";
    
    // Find EQ controls for both decks first
    QWidget* centralWidget = this;  // QtMainWindow selbst verwenden
    std::cout << "*** Using QtMainWindow as search widget ***" << std::endl;
    
    // Find EQ controls for Deck A
    QDial* leftHigh = centralWidget->findChild<QDial*>("leftHigh");
    QDial* leftMid = centralWidget->findChild<QDial*>("leftMid");
    QDial* leftLow = centralWidget->findChild<QDial*>("leftLow");
    QDial* leftFilter = centralWidget->findChild<QDial*>("leftFilter");
    
    std::cout << "*** EQ Dials Deck A: leftHigh=" << (leftHigh ? "FOUND" : "NOT FOUND") 
              << ", leftMid=" << (leftMid ? "FOUND" : "NOT FOUND")
              << ", leftLow=" << (leftLow ? "FOUND" : "NOT FOUND")
              << ", leftFilter=" << (leftFilter ? "FOUND" : "NOT FOUND") << std::endl;
    
    // Find EQ controls for Deck B  
    QDial* rightHigh = centralWidget->findChild<QDial*>("rightHigh");
    QDial* rightMid = centralWidget->findChild<QDial*>("rightMid");
    QDial* rightLow = centralWidget->findChild<QDial*>("rightLow");
    QDial* rightFilter = centralWidget->findChild<QDial*>("rightFilter");
    
    std::cout << "*** EQ Dials Deck B: rightHigh=" << (rightHigh ? "FOUND" : "NOT FOUND") 
              << ", rightMid=" << (rightMid ? "FOUND" : "NOT FOUND")
              << ", rightLow=" << (rightLow ? "FOUND" : "NOT FOUND")
              << ", rightFilter=" << (rightFilter ? "FOUND" : "NOT FOUND") << std::endl;
    
    // EQ Controls für Deck A - DUPLICATE CONNECTIONS DISABLED TO AVOID CONFLICTS
    /*
    if (leftHigh && leftMid && leftLow) {
        std::cout << "*** Setting up EQ connections for Deck A ***" << std::endl;
        connect(leftHigh, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;  // -10-100..100 -> -1..1
            std::cout << "*** LEFT HIGH EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setHighGain(gain);
                std::cout << "*** Called playerA->setHighGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckA();
            DeckSettings::instance().setEQ(0, gain, config.midGain, config.lowGain);
        });
        
        connect(leftMid, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** LEFT MID EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setMidGain(gain);
                std::cout << "*** Called playerA->setMidGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckA();
            DeckSettings::instance().setEQ(0, config.highGain, gain, config.lowGain);
        });
        
        connect(leftLow, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** LEFT LOW EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setLowGain(gain);
                std::cout << "*** Called playerA->setLowGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckA();
            DeckSettings::instance().setEQ(0, config.highGain, config.midGain, gain);
        });
    } else {
        std::cout << "*** ERROR: EQ dials not found for Deck A!" << std::endl;
    }
    
    // Filter für Deck A
    if (leftFilter) {
        std::cout << "*** Setting up Filter connection for Deck A ***" << std::endl;
        connect(leftFilter, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double pos = value / 100.0;  // -100..100 -> -1..1
            std::cout << "*** LEFT FILTER CHANGED: " << value << " -> " << pos << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setFilterCutoff(pos);
                std::cout << "*** Called playerA->setFilterCutoff(" << pos << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            DeckSettings::instance().setFilter(0, pos);
        });
    } else {
        std::cout << "*** ERROR: Filter dial not found for Deck A!" << std::endl;
    }
    
    // EQ Controls für Deck B
    if (rightHigh && rightMid && rightLow) {
        std::cout << "*** Setting up EQ connections for Deck B ***" << std::endl;
        connect(rightHigh, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** RIGHT HIGH EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setHighGain(gain);
                std::cout << "*** Called playerB->setHighGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckB();
            DeckSettings::instance().setEQ(1, gain, config.midGain, config.lowGain);
        });
        
        connect(rightMid, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** RIGHT MID EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setMidGain(gain);
                std::cout << "*** Called playerB->setMidGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckB();
            DeckSettings::instance().setEQ(1, config.highGain, gain, config.lowGain);
        });
        
        connect(rightLow, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** RIGHT LOW EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setLowGain(gain);
                std::cout << "*** Called playerB->setLowGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckB();
            DeckSettings::instance().setEQ(1, config.highGain, config.midGain, gain);
        });
    } else {
        std::cout << "*** ERROR: EQ dials not found for Deck B!" << std::endl;
    }
    
    // Filter für Deck B
    if (rightFilter) {
        std::cout << "*** Setting up Filter connection for Deck B ***" << std::endl;
        connect(rightFilter, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double pos = value / 100.0;
            std::cout << "*** RIGHT FILTER CHANGED: " << value << " -> " << pos << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setFilterCutoff(pos);
                std::cout << "*** Called playerB->setFilterCutoff(" << pos << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            DeckSettings::instance().setFilter(1, pos);
        });
    } else {
        std::cout << "*** ERROR: Filter dial not found for Deck B!" << std::endl;
    }
    */
    
    // Deck A Connections
    if (deckA) {
        // Keylock & Quantize werden bereits in den Deck-Widgets gehandhabt
        // Wir müssen nur die Settings aktualisieren wenn sie sich ändern
        
        if (deckA->getKeylockButton()) {
            connect(deckA->getKeylockButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setKeylock(0, checked);
                qDebug() << "Deck A Keylock saved:" << checked;
            });
        }
        
        if (deckA->getQuantizeButton()) {
            connect(deckA->getQuantizeButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setQuantize(0, checked);
                qDebug() << "Deck A Quantize saved:" << checked;
            });
        }
        
        if (deckA->getSpeedSlider()) {
            connect(deckA->getSpeedSlider(), &QSlider::valueChanged, [this](int value) {
                double factor = value / 1000.0;
                DeckSettings::instance().setSpeedFactor(0, factor);
            });
        }
    }
    
    // EQ Controls für Deck A - SECOND DUPLICATE SECTION DISABLED
    /*
    if (leftHigh && leftMid && leftLow) {
        connect(leftHigh, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;  // -100..100 -> -1..1
            std::cout << "*** LEFT HIGH EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setHighGain(gain);
                std::cout << "*** Called playerA->setHighGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckA();
            DeckSettings::instance().setEQ(0, gain, config.midGain, config.lowGain);
        });
        
        connect(leftMid, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** LEFT MID EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setMidGain(gain);
                std::cout << "*** Called playerA->setMidGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckA();
            DeckSettings::instance().setEQ(0, config.highGain, gain, config.lowGain);
        });
        
        connect(leftLow, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** LEFT LOW EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setLowGain(gain);
                std::cout << "*** Called playerA->setLowGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckA();
            DeckSettings::instance().setEQ(0, config.highGain, config.midGain, gain);
        });
    } else {
        std::cout << "*** ERROR: EQ dials not found for Deck A!" << std::endl;
    }
    
    // Filter für Deck A - BEHEBT HAUPTPROBLEM: Ruft SOWOHL DeckSettings ALS AUCH Audio-Engine auf
    if (leftFilter) {
        connect(leftFilter, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double pos = value / 100.0;  // -100..100 -> -1..1
            std::cout << "*** LEFT FILTER CHANGED: " << value << " -> " << pos << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerA) {
                playerA->setFilterCutoff(pos);
                std::cout << "*** Called playerA->setFilterCutoff(" << pos << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerA is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            DeckSettings::instance().setFilter(0, pos);
        });
    } else {
        std::cout << "*** ERROR: Filter dial not found for Deck A!" << std::endl;
    }
    
    // Volume für Deck A
    if (leftVolumeSlider) {
        connect(leftVolumeSlider, &QSlider::valueChanged, [this](int value) {
            double gain = value / 100.0;  // 0..100 -> 0..1
            DeckSettings::instance().setGain(0, gain);
        });
    }
    
    // Deck B Connections
    if (deckB) {
        if (deckB->getKeylockButton()) {
            connect(deckB->getKeylockButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setKeylock(1, checked);
                qDebug() << "Deck B Keylock saved:" << checked;
            });
        }
        
        if (deckB->getQuantizeButton()) {
            connect(deckB->getQuantizeButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setQuantize(1, checked);
                qDebug() << "Deck B Quantize saved:" << checked;
            });
        }
        
        if (deckB->getSpeedSlider()) {
            connect(deckB->getSpeedSlider(), &QSlider::valueChanged, [this](int value) {
                double factor = value / 1000.0;
                DeckSettings::instance().setSpeedFactor(1, factor);
            });
        }
    }
    
    // EQ Controls für Deck B - BEHEBT HAUPTPROBLEM: Ruft SOWOHL DeckSettings ALS AUCH Audio-Engine auf
    if (rightHigh && rightMid && rightLow) {
        connect(rightHigh, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** RIGHT HIGH EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setHighGain(gain);
                std::cout << "*** Called playerB->setHighGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckB();
            DeckSettings::instance().setEQ(1, gain, config.midGain, config.lowGain);
        });
        
        connect(rightMid, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** RIGHT MID EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setMidGain(gain);
                std::cout << "*** Called playerB->setMidGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckB();
            DeckSettings::instance().setEQ(1, config.highGain, gain, config.lowGain);
        });
        
        connect(rightLow, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double gain = value / 100.0;
            std::cout << "*** RIGHT LOW EQ CHANGED: " << value << " -> " << gain << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setLowGain(gain);
                std::cout << "*** Called playerB->setLowGain(" << gain << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            auto& config = DeckSettings::instance().getDeckB();
            DeckSettings::instance().setEQ(1, config.highGain, config.midGain, gain);
        });
    } else {
        std::cout << "*** ERROR: EQ dials not found for Deck B!" << std::endl;
    }
    
    // Filter für Deck B - BEHEBT HAUPTPROBLEM: Ruft SOWOHL DeckSettings ALS AUCH Audio-Engine auf
    if (rightFilter) {
        connect(rightFilter, QOverload<int>::of(&QDial::valueChanged), [this](int value) {
            double pos = value / 100.0;
            std::cout << "*** RIGHT FILTER CHANGED: " << value << " -> " << pos << std::endl;
            // 1. Audio-Engine informieren (WICHTIG für sofortigen Effekt!)
            if (playerB) {
                playerB->setFilterCutoff(pos);
                std::cout << "*** Called playerB->setFilterCutoff(" << pos << ")" << std::endl;
            } else {
                std::cout << "*** ERROR: playerB is nullptr!" << std::endl;
            }
            // 2. DeckSettings aktualisieren (aber NICHT speichern)
            DeckSettings::instance().setFilter(1, pos);
        });
    } else {
        std::cout << "*** ERROR: Filter dial not found for Deck B!" << std::endl;
    }
    */
    
    // Volume für Deck B
    if (rightVolumeSlider) {
        connect(rightVolumeSlider, &QSlider::valueChanged, [this](int value) {
            double gain = value / 100.0;
            DeckSettings::instance().setGain(1, gain);
        });
    }
    
    qDebug() << "BetaPulseX: Deck settings connections established";
}

void QtMainWindow::applyScratchPosition(bool isDeckA, double absRel) {
    DJAudioPlayer* player = isDeckA ? playerA : playerB;
    QtDeckWidget* deck = isDeckA ? deckA : deckB;
    WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB;
    if (!player || !overview) return;

    double minRel = overview->isPrerollEnabled() ? -1.0 : 0.0;
    absRel = std::clamp(absRel, minRel, 1.0);

    player->setPositionRelative(absRel);
    overview->setPlayhead(absRel);

    if (deck && deck->getWaveform()) {
        deck->getWaveform()->setPlayhead(absRel);
    }

    if (beatIndicator && deck) {
        double lenSec = std::max(1e-9, player->getLengthInSeconds());
        constexpr double prerollSec = 8.0;
        double seconds = (absRel < 0.0) ? (absRel * prerollSec) : (absRel * lenSec);
        if (isDeckA) {
            beatIndicator->setTrackPositionDeckA(seconds);
            deck->setPlatterSeconds(seconds);
        } else {
            beatIndicator->setTrackPositionDeckB(seconds);
            deck->setPlatterSeconds(seconds);
        }
    }
}

void QtMainWindow::startScratchInertia(bool isDeckA, double initialVelocity, bool resumePlayback) {
    DJAudioPlayer* player = isDeckA ? playerA : playerB;
    WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB;
    if (!player || !overview) return;

    auto& timer = isDeckA ? scratchInertiaTimerA : scratchInertiaTimerB;
    auto& velocity = isDeckA ? scratchInertiaVelocityA : scratchInertiaVelocityB;
    auto& elapsed = isDeckA ? scratchInertiaElapsedA : scratchInertiaElapsedB;
    auto& active = isDeckA ? scratchInertiaActiveA : scratchInertiaActiveB;
    auto& resume = isDeckA ? scratchInertiaResumeA : scratchInertiaResumeB;

    resume = resumePlayback;

    if (std::abs(initialVelocity) < 0.05) {
        stopScratchInertia(isDeckA, resumePlayback);
        return;
    }

    if (!timer) {
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

void QtMainWindow::stopScratchInertia(bool isDeckA, bool resumePlayback) {
    DJAudioPlayer* player = isDeckA ? playerA : playerB;
    auto& timer = isDeckA ? scratchInertiaTimerA : scratchInertiaTimerB;
    auto& active = isDeckA ? scratchInertiaActiveA : scratchInertiaActiveB;
    auto& velocity = isDeckA ? scratchInertiaVelocityA : scratchInertiaVelocityB;

    if (timer) {
        timer->stop();
    }
    active = false;
    velocity = 0.0;

    if (!player) return;

    player->enableScratch(false);

    if (resumePlayback) {
        player->ensureScratchAudible();
    } else {
        player->stop();
    }
}

void QtMainWindow::handleScratchInertiaTick(bool isDeckA) {
    DJAudioPlayer* player = isDeckA ? playerA : playerB;
    WaveformDisplay* overview = isDeckA ? overviewTopA : overviewTopB;
    auto& timer = isDeckA ? scratchInertiaTimerA : scratchInertiaTimerB;
    auto& velocity = isDeckA ? scratchInertiaVelocityA : scratchInertiaVelocityB;
    auto& elapsed = isDeckA ? scratchInertiaElapsedA : scratchInertiaElapsedB;
    auto& active = isDeckA ? scratchInertiaActiveA : scratchInertiaActiveB;
    auto& resume = isDeckA ? scratchInertiaResumeA : scratchInertiaResumeB;

    if (!player || !overview || !timer || !active) {
        return;
    }

    double dt = timer->interval() / 1000.0;
    elapsed += dt;

    const double friction = 0.88;
    velocity *= friction;

    const double maxDuration = resume ? 0.35 : 0.6;
    if (elapsed > maxDuration) {
        velocity = 0.0;
    }

    double totalLength = player->getLengthInSeconds();
    if (totalLength <= 0.0 && overview->trackLengthSec > 0.0) {
        totalLength = overview->trackLengthSec;
    }

    double currentRel = overview->getPlayheadRelative();
    double prerollSec = overview->getPrerollTimeSeconds();
    double currentSec = (currentRel < 0.0) ? (currentRel * prerollSec) : (currentRel * totalLength);
    double deltaSec = velocity * dt;
    double newSec = currentSec + deltaSec;

    double minSec = -prerollSec;
    double maxSec = totalLength;
    newSec = std::clamp(newSec, minSec, maxSec);

    double newRel;
    if (newSec < 0.0) {
        newRel = (prerollSec > 1e-6) ? (newSec / prerollSec) : currentRel;
    } else if (totalLength > 1e-6) {
        newRel = newSec / totalLength;
    } else {
        newRel = currentRel;
    }

    applyScratchPosition(isDeckA, newRel);

    if (std::abs(velocity) < 0.015) {
        stopScratchInertia(isDeckA, resume);
    }
}

// Event filter for double-click reset functionality
bool QtMainWindow::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            // EQ controls - reset to 0 (neutral)
            if (obj == leftHigh || obj == leftMid || obj == leftLow || obj == leftFilter ||
                obj == rightHigh || obj == rightMid || obj == rightLow || obj == rightFilter) {
                QDial *dial = qobject_cast<QDial*>(obj);
                if (dial) {
                    std::cout << "Double-click reset: " << dial->toolTip().toStdString() << " to 0 (neutral)" << std::endl;
                    dial->setValue(0);
                    return true;
                }
            }
            // Volume sliders - reset to 100 (full volume)
            else if (obj == leftVolumeSlider || obj == rightVolumeSlider) {
                QSlider *slider = qobject_cast<QSlider*>(obj);
                if (slider) {
                    std::cout << "Double-click reset: Volume slider to 100 (full volume)" << std::endl;
                    slider->setValue(100);
                    return true;
                }
            }
            // Crossfader - reset to 50 (center)
            else if (obj == crossfader) {
                QSlider *slider = qobject_cast<QSlider*>(obj);
                if (slider) {
                    std::cout << "Double-click reset: Crossfader to 50 (center)" << std::endl;
                    slider->setValue(50);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

// PREROLL SUPPORT: Update playback positions automatically for smooth UI
void QtMainWindow::updatePlaybackPositions() {
    // Only update when not scratching to prevent interference
    static double lastPosA = -999.0;
    static double lastPosB = -999.0;
    static int debugCounter = 0;
    
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    
    // Debug: Print position updates occasionally
    if (++debugCounter % 200 == 0) {
        std::cout << "[Timer] Update positions - A: " << (playerA ? playerA->getPositionRelative() : -999.0) 
                  << ", B: " << (playerB ? playerB->getPositionRelative() : -999.0) << std::endl;
    }
    
    // Update Deck A position - keep UI synced, even when paused in preroll
    bool canUpdateA = playerA && overviewTopA && !overviewTopA->isScratching() && 
                      (currentTime - lastScratchEndA > 100); // 100ms delay after scratch end
    
    if (canUpdateA) {
        double relativePos = playerA->getPositionRelative();
        
        // PREROLL PROTECTION: Only skip when paused in positive song area
        bool isPlaybackUpdate = playerA->isPlaying() || relativePos < 0.0;
        
        // Even smarter threshold: very small in preroll for smooth movement, moderate in song
        double threshold = (relativePos < 0.0) ? 0.0002 : 0.008; // Preroll: 0.0002, Song: 0.008
        
        if (isPlaybackUpdate && std::abs(relativePos - lastPosA) > threshold) {
            lastPosA = relativePos;
            
            // Update waveform displays - be explicit about the position
            overviewTopA->setPlayhead(relativePos);
            if (deckA && deckA->getWaveform()) {
                deckA->getWaveform()->setPlayhead(relativePos);
            }
            
            // UPDATE BEAT INDICATOR: Convert relative position to seconds
            if (beatIndicator && playerA) {
                double lenSec = std::max(1e-9, playerA->getLengthInSeconds());
                constexpr double prerollSec = 8.0; // Keep in sync with WaveformDisplay/DJAudioPlayer
                double seconds = (relativePos < 0.0) ? (relativePos * prerollSec) : (relativePos * lenSec);
                beatIndicator->setTrackPositionDeckA(seconds);
            }
            
            // Debug: Log significant position changes
            if (debugCounter % 50 == 0) {
                std::cout << "[Timer] Deck A position updated to: " << relativePos << std::endl;
            }
        }
    } else if (overviewTopA && overviewTopA->isScratching()) {
        // Update scratch end time when scratching is detected
        lastScratchEndA = currentTime;
    }
    
    // Update Deck B position independently - keep UI synced in preroll too
    bool canUpdateB = playerB && overviewTopB && !overviewTopB->isScratching() && 
                      (currentTime - lastScratchEndB > 100); // 100ms delay after scratch end
    
    if (canUpdateB) {
        double relativePos = playerB->getPositionRelative();
        
        // PREROLL PROTECTION: Only skip when paused in positive song area
        bool isPlaybackUpdate = playerB->isPlaying() || relativePos < 0.0;
        
        // Even smarter threshold: very small in preroll for smooth movement, moderate in song
        double threshold = (relativePos < 0.0) ? 0.0002 : 0.008; // Preroll: 0.0002, Song: 0.008
        
        if (isPlaybackUpdate && std::abs(relativePos - lastPosB) > threshold) {
            lastPosB = relativePos;
            
            // Update waveform displays - be explicit about the position
            overviewTopB->setPlayhead(relativePos);
            if (deckB && deckB->getWaveform()) {
                deckB->getWaveform()->setPlayhead(relativePos);
            }
            
            // UPDATE BEAT INDICATOR: Convert relative position to seconds
            if (beatIndicator && playerB) {
                double lenSec = std::max(1e-9, playerB->getLengthInSeconds());
                constexpr double prerollSec = 8.0; // Keep in sync with WaveformDisplay/DJAudioPlayer
                double seconds = (relativePos < 0.0) ? (relativePos * prerollSec) : (relativePos * lenSec);
                beatIndicator->setTrackPositionDeckB(seconds);
            }
            
            // Debug: Log significant position changes
            if (debugCounter % 50 == 0) {
                std::cout << "[Timer] Deck B position updated to: " << relativePos << std::endl;
            }
        }
    } else if (overviewTopB && overviewTopB->isScratching()) {
        // Update scratch end time when scratching is detected
        lastScratchEndB = currentTime;
    }
}

