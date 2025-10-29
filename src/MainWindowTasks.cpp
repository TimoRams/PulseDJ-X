#include "MainWindowTasks.h"

#include "AudioFormatGuard.h"
#include "DeckWidget.h"
#include "DJAudioPlayer.h"
#include "LibraryManager.h"
#include "MainWindow.h"
#include "PerformancePads.h"
#include "BpmAnalyzer.h"
#include "WaveformDisplay.h"
#include "WaveformGenerator.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QVector>

#include <exception>
#include <memory>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

AudioFileLoadTask::AudioFileLoadTask(QtMainWindow* mainWindow, QString filePath, bool isDeckA)
    : window(mainWindow), filePath(std::move(filePath)), isDeckA(isDeckA) {
    setAutoDelete(true);
}

void AudioFileLoadTask::run() {
    if (!window) {
        return;
    }

    const auto windowGuard = window;

    try {
        QThread::currentThread()->setPriority(QThread::LowPriority);

        const juce::File audioFile(filePath.toStdString());
        std::unique_ptr<juce::AudioFormatReader> reader;
        if (auto manager = QtMainWindow::sharedFormatManager) {
            AudioFormatManagerGuard formatGuard;
            reader.reset(manager->createReaderFor(audioFile));
        }

        if (reader) {
            auto readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
            const auto sampleRate = readerSource->getAudioFormatReader()->sampleRate;
            auto sourceWrapper = std::make_shared<std::unique_ptr<juce::AudioFormatReaderSource>>(std::move(readerSource));

            if (auto* target = windowGuard.data()) {
                QMetaObject::invokeMethod(target,
                                          [windowGuard,
                                           deckIsA = isDeckA,
                                           rate = sampleRate,
                                           path = filePath,
                                           sourceWrapper]() mutable {
                                              auto sourcePtr = std::move(*sourceWrapper);
                                              if (auto* windowPtr = windowGuard.data()) {
                                                  if (auto* player = windowPtr->getPlayer(deckIsA)) {
                                                      if (auto* deckWidget = deckIsA ? windowPtr->deckA : windowPtr->deckB) {
                                                          player->applyLoadedSource(std::move(sourcePtr), rate);
                                                          deckWidget->onFileLoadingComplete(path);
                                                          return;
                                                      }
                                                  }
                                              }
                                          },
                                          Qt::QueuedConnection);
            }
        } else if (auto* target = windowGuard.data()) {
            QMetaObject::invokeMethod(target,
                                      [windowGuard, path = filePath]() {
                                          if (auto* windowPtr = windowGuard.data()) {
                                              const QFileInfo fi(path);
                                              windowPtr->setStatusTip(
                                                  QStringLiteral("Failed to load audio file: %1").arg(fi.fileName()));
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    } catch (const std::exception& e) {
        if (auto* target = windowGuard.data()) {
            QMetaObject::invokeMethod(target,
                                      [windowGuard,
                                       path = filePath,
                                       error = QString::fromStdString(e.what())]() {
                                          if (auto* windowPtr = windowGuard.data()) {
                                              const QFileInfo fi(path);
                                              windowPtr->setStatusTip(
                                                  QStringLiteral("Audio loading error: %1 - %2")
                                                      .arg(fi.fileName())
                                                      .arg(error));
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    }
}

BpmAnalysisTask::BpmAnalysisTask(QtMainWindow* mainWindow,
                                 juce::File file,
                                 Target target,
                                 double minBpm,
                                 double maxBpm)
    : window(mainWindow),
      audioFile(std::move(file)),
      target(target),
      minBpm(std::min(minBpm, maxBpm)),
      maxBpm(std::max(minBpm, maxBpm)) {
    setAutoDelete(true);
}

void BpmAnalysisTask::run() {
    QPointer<QtMainWindow> wptr = window;
    if (!wptr) {
        return;
    }

    const bool deckTask = target != Target::LibraryOnly;
    const bool deckIsA = target == Target::DeckA;
    const QString filePath = QString::fromStdString(audioFile.getFullPathName().toStdString());
    const QString displayName = QString::fromStdString(audioFile.getFileNameWithoutExtension().toStdString());

    if (auto windowRaw = wptr.data()) {
        QMetaObject::invokeMethod(windowRaw,
                                  [wptr, deckTask, deckIsA, displayName, filePath]() {
                                      if (auto window = wptr.data()) {
                                          window->setStatusTip(QString("Analyzing BPM: %1...").arg(displayName));

                                          if (window->libraryManager) {
                                              window->libraryManager->notifyAnalysisStarted(filePath);
                                          }

                                          if (deckTask) {
                                              if (deckIsA) {
                                                  window->analysisActiveA = true;
                                                  window->analysisFailedA = false;
                                                  window->analysisProgressA = 0.0;
                                              } else {
                                                  window->analysisActiveB = true;
                                                  window->analysisFailedB = false;
                                                  window->analysisProgressB = 0.0;
                                              }

                                              window->updateOverviewLabel(deckIsA);
                                          }
                                      }
                                  },
                                  Qt::QueuedConnection);
    }

    try {
        std::vector<double> beatsSec;
        double totalSec = 0.0;
        std::string algorithm;
        double firstBeatOffset = 0.0;

        QThread::currentThread()->setPriority(QThread::LowPriority);

        if (auto windowRaw = wptr.data()) {
            QMetaObject::invokeMethod(windowRaw,
                                      [wptr, deckTask, deckIsA]() {
                                          if (!deckTask) {
                                              return;
                                          }

                                          if (auto window = wptr.data()) {
                                              WaveformDisplay* wf = deckIsA ? window->overviewTopA : window->overviewTopB;
                                              if (wf) {
                                                  wf->setAnalysisFailed(false);
                                                  wf->setAnalysisActive(true);
                                                  wf->setAnalysisProgress(0.0);
                                              }
                                          }
                                      },
                                      Qt::QueuedConnection);
        }

        auto progressCb = [wptr = QPointer<QtMainWindow>(window), deckTask, deckIsA, filePath](double p) {
            if (!wptr) {
                return;
            }

            QMetaObject::invokeMethod(wptr,
                                      [wptr, p, deckTask, deckIsA, filePath]() {
                                          if (!wptr) {
                                              return;
                                          }

                                          if (deckTask) {
                                              WaveformDisplay* wf = deckIsA ? wptr->overviewTopA : wptr->overviewTopB;
                                              if (wf) {
                                                  wf->setAnalysisProgress(p);
                                              }

                                              if (deckIsA) {
                                                  wptr->analysisProgressA = p;
                                              } else {
                                                  wptr->analysisProgressB = p;
                                              }

                                              wptr->updateOverviewLabel(deckIsA);
                                          }

                                          if (wptr->libraryManager) {
                                              wptr->libraryManager->notifyAnalysisProgress(filePath, p);
                                          }
                                      },
                                      Qt::QueuedConnection);
        };

        auto errorCb = [wptr = QPointer<QtMainWindow>(window), deckTask, deckIsA, filePath](const std::string&) {
            if (!wptr) {
                return;
            }

            QMetaObject::invokeMethod(wptr,
                                      [wptr, deckTask, deckIsA, filePath]() {
                                          if (!wptr) {
                                              return;
                                          }

                                          if (deckTask) {
                                              WaveformDisplay* wf = deckIsA ? wptr->overviewTopA : wptr->overviewTopB;
                                              if (wf) {
                                                  wf->setAnalysisFailed(true);
                                                  wf->setAnalysisActive(false);
                                              }

                                              if (deckIsA) {
                                                  wptr->analysisFailedA = true;
                                                  wptr->analysisActiveA = false;
                                              } else {
                                                  wptr->analysisFailedB = true;
                                                  wptr->analysisActiveB = false;
                                              }

                                              wptr->updateOverviewLabel(deckIsA);
                                          }

                                          if (wptr->libraryManager) {
                                              wptr->libraryManager->notifyAnalysisFinished(filePath, false);
                                          }
                                      },
                                      Qt::QueuedConnection);
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
            QMetaObject::invokeMethod(windowRaw,
                                      [wptr,
                                       deckTask,
                                       deckIsA,
                                       displayName,
                                       bpm,
                                       beatsSec,
                                       totalSec,
                                       algorithm,
                                       firstBeatOffset,
                                       filePath]() {
                                          if (auto window = wptr.data()) {
                                              window->setStatusTip(QString("Analysis complete: %1 (%2 BPM)")
                                                                       .arg(displayName)
                                                                       .arg(QString::number(bpm, 'f', 1)));

                                              if (deckTask) {
                                                  window->handleBpmAnalysisResult(
                                                      bpm,
                                                      beatsSec,
                                                      totalSec,
                                                      algorithm,
                                                      firstBeatOffset,
                                                      deckIsA);

                                                  WaveformDisplay* wf = deckIsA ? window->overviewTopA : window->overviewTopB;
                                                  if (wf) {
                                                      wf->setAnalysisActive(false);
                                                      wf->setAnalysisFailed(bpm <= 0.0);
                                                      wf->setAnalysisProgress(1.0);
                                                  }

                                                  if (deckIsA) {
                                                      window->analysisActiveA = false;
                                                      window->analysisFailedA = (bpm <= 0.0);
                                                      window->analysisProgressA = 1.0;
                                                  } else {
                                                      window->analysisActiveB = false;
                                                      window->analysisFailedB = (bpm <= 0.0);
                                                      window->analysisProgressB = 1.0;
                                                  }

                                                  window->updateOverviewLabel(deckIsA);
                                              } else if (window->libraryManager) {
                                                  QVector<double> beatVector;
                                                  beatVector.reserve(static_cast<int>(beatsSec.size()));
                                                  for (double beat : beatsSec) {
                                                      beatVector.append(beat);
                                                  }

                                                  window->libraryManager->applyAnalysisResult(
                                                      filePath,
                                                      bpm,
                                                      firstBeatOffset,
                                                      totalSec,
                                                      beatVector,
                                                      QString::fromStdString(algorithm),
                                                      bpm <= 0.0);

                                                  bool reAppliedToDeck = false;
                                                  if (window->deckA && window->deckA->getCurrentFilePath() == filePath) {
                                                      window->reapplyStoredDeckMetadata(true);
                                                      reAppliedToDeck = true;
                                                  }
                                                  if (window->deckB && window->deckB->getCurrentFilePath() == filePath) {
                                                      window->reapplyStoredDeckMetadata(false);
                                                      reAppliedToDeck = true;
                                                  }

                                                  if (reAppliedToDeck) {
                                                      window->setStatusTip(QStringLiteral("Reapplied analysis to loaded deck: %1")
                                                                               .arg(displayName));
                                                  }
                                              }

                                              if (window->libraryManager) {
                                                  window->libraryManager->notifyAnalysisFinished(filePath, bpm > 0.0);
                                              }
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    } catch (const std::exception& e) {
        if (auto windowRaw = wptr.data()) {
            QMetaObject::invokeMethod(windowRaw,
                                      [wptr,
                                       deckTask,
                                       deckIsA,
                                       displayName,
                                       error = QString::fromStdString(e.what()),
                                       filePath]() {
                                          if (auto window = wptr.data()) {
                                              window->setStatusTip(QString("Analysis failed: %1 - %2")
                                                                       .arg(displayName)
                                                                       .arg(error));

                                              if (window->libraryManager) {
                                                  window->libraryManager->notifyAnalysisFinished(filePath, false);
                                              }

                                              if (deckTask) {
                                                  WaveformDisplay* wf = deckIsA ? window->overviewTopA : window->overviewTopB;
                                                  if (wf) {
                                                      wf->setAnalysisFailed(true);
                                                      wf->setAnalysisActive(false);
                                                  }

                                                  if (deckIsA) {
                                                      window->analysisFailedA = true;
                                                      window->analysisActiveA = false;
                                                  } else {
                                                      window->analysisFailedB = true;
                                                      window->analysisActiveB = false;
                                                  }

                                                  window->updateOverviewLabel(deckIsA);
                                              }
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    }
}

TopWaveformDisplayTask::TopWaveformDisplayTask(QtMainWindow* mainWindow, QString filePath, bool isDeckA)
    : window(mainWindow), filePath(std::move(filePath)), isDeckA(isDeckA) {
    setAutoDelete(true);
}

void TopWaveformDisplayTask::run() {
    QPointer<QtMainWindow> windowGuard = window;
    if (!windowGuard) {
        qDebug() << "TopWaveformDisplayTask::run - window guard invalid!";
        return;
    }

    qDebug() << "TopWaveformDisplayTask::run - starting for" << filePath << "deck:" << (isDeckA ? "A" : "B");

    const bool deckIsA = isDeckA;
    const QString path = filePath;

    if (auto* windowRaw = windowGuard.data()) {
        QMetaObject::invokeMethod(windowRaw,
                                  [w = windowGuard, deckIsA, path]() {
                                      if (auto windowPtr = w.data()) {
                                          QtDeckWidget* deck = deckIsA ? windowPtr->deckA : windowPtr->deckB;
                                          if (!deck || deck->getCurrentFilePath() != path) {
                                              return;
                                          }

                                          WaveformDisplay* wf = deckIsA ? windowPtr->overviewTopA : windowPtr->overviewTopB;
                                          if (wf) {
                                              wf->setAnalysisFailed(false);
                                              wf->setAnalysisActive(true);
                                              wf->setAnalysisProgress(0.0);
                                          }

                                          if (deckIsA) {
                                              windowPtr->analysisActiveA = true;
                                              windowPtr->analysisFailedA = false;
                                              windowPtr->analysisProgressA = 0.0;
                                          } else {
                                              windowPtr->analysisActiveB = true;
                                              windowPtr->analysisFailedB = false;
                                              windowPtr->analysisProgressB = 0.0;
                                          }

                                          auto& session = deckIsA ? windowPtr->streamSessionA : windowPtr->streamSessionB;
                                          session = {};
                                          session.valid = false;

                                          windowPtr->updateOverviewLabel(deckIsA);
                                      }
                                  },
                                  Qt::QueuedConnection);
    }

    auto dispatchFailure = [windowGuard, deckIsA, path]() {
        if (auto* windowRaw = windowGuard.data()) {
            QMetaObject::invokeMethod(windowRaw,
                                      [w = windowGuard, deckIsA, path]() {
                                          if (auto windowPtr = w.data()) {
                                              QtDeckWidget* deck = deckIsA ? windowPtr->deckA : windowPtr->deckB;
                                              if (!deck || deck->getCurrentFilePath() != path) {
                                                  return;
                                              }

                                              WaveformDisplay* wf = deckIsA ? windowPtr->overviewTopA : windowPtr->overviewTopB;
                                              if (wf) {
                                                  wf->setAnalysisActive(false);
                                                  wf->setAnalysisFailed(true);
                                              }

                                              if (deckIsA) {
                                                  windowPtr->analysisActiveA = false;
                                                  windowPtr->analysisFailedA = true;
                                              } else {
                                                  windowPtr->analysisActiveB = false;
                                                  windowPtr->analysisFailedB = true;
                                              }

                                              auto& session = deckIsA ? windowPtr->streamSessionA : windowPtr->streamSessionB;
                                              session = {};
                                              session.valid = false;

                                              windowPtr->updateOverviewLabel(deckIsA);
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    };

    try {
        QThread::currentThread()->setPriority(QThread::LowestPriority);
        WaveformGenerator generator;

        WaveformGenerator::AnalysisMetadata metadata;
        if (!generator.analyzeFile(juce::File(path.toStdString()), metadata)) {
            dispatchFailure();
            return;
        }

        const double binsPerSecondTarget = 480.0;
        const double lengthSeconds = std::max(metadata.lengthSeconds, 0.001);
        int totalBins = static_cast<int>(std::ceil(lengthSeconds * binsPerSecondTarget));
        totalBins = std::clamp(totalBins, 4096, 240000);
        const int chunkBinSize = 4096;
        const int firstChunkBins = std::min(chunkBinSize, totalBins);

        std::shared_ptr<std::vector<float>> firstMaxPtr;
        std::shared_ptr<std::vector<float>> firstMinPtr;

        if (firstChunkBins > 0) {
            std::vector<float> maxBins;
            std::vector<float> minBins;
            if (generator.renderBinWindow(juce::File(path.toStdString()),
                                          metadata,
                                          totalBins,
                                          0,
                                          firstChunkBins,
                                          maxBins,
                                          minBins)) {
                firstMaxPtr = std::make_shared<std::vector<float>>(std::move(maxBins));
                firstMinPtr = std::make_shared<std::vector<float>>(std::move(minBins));
            }
        }

        QtMainWindow::WaveformStreamSession session;
        session.filePath = path;
        session.metadata = metadata;
        session.totalBins = totalBins;
        session.lengthSeconds = metadata.lengthSeconds;
        session.binsPerSecond = static_cast<double>(totalBins) / lengthSeconds;
        session.chunkBinSize = chunkBinSize;
    session.cacheCapacityBins = session.chunkBinSize * 6;
        session.valid = true;
        session.cachedStartBin = 0;
        session.cachedEndBin = 0;

        const bool hasInitialChunk = firstMaxPtr && firstMinPtr && !firstMaxPtr->empty();
        if (hasInitialChunk) {
            session.hasCache = true;
            session.cachedStartBin = 0;
            session.cachedEndBin = std::min(totalBins, static_cast<int>(firstMaxPtr->size()));
        }

        const double coverage = session.hasCache
            ? static_cast<double>(session.cachedEndBin - session.cachedStartBin) / std::max(1, session.totalBins)
            : 0.0;

        if (auto* windowRaw = windowGuard.data()) {
            QMetaObject::invokeMethod(windowRaw,
                                      [w = windowGuard,
                                       deckIsA,
                                       path,
                                       session,
                                       firstMaxPtr,
                                       firstMinPtr,
                                       coverage]() {
                                          if (auto windowPtr = w.data()) {
                                              QtDeckWidget* deck = deckIsA ? windowPtr->deckA : windowPtr->deckB;
                                              if (!deck || deck->getCurrentFilePath() != path) {
                                                  return;
                                              }

                                              auto& deckSession = deckIsA ? windowPtr->streamSessionA : windowPtr->streamSessionB;
                                              deckSession = session;
                                              deckSession.pendingChunks.clear();

                                              WaveformDisplay* wf = deckIsA ? windowPtr->overviewTopA : windowPtr->overviewTopB;
                                              if (wf) {
                                                  wf->beginStreaming(session.totalBins,
                                                                     session.metadata.audioStartOffsetSec,
                                                                     session.lengthSeconds,
                                                                     session.chunkBinSize,
                                                                     session.chunkBinSize * 6);

                                                  if (session.hasCache && firstMaxPtr && firstMinPtr) {
                                                      wf->appendStreamBins(0, *firstMaxPtr, *firstMinPtr, false);
                                                      wf->setAnalysisActive(false);
                                                      wf->setAnalysisProgress(std::clamp(coverage, 0.0, 1.0));
                                                  } else {
                                                      wf->setAnalysisActive(true);
                                                      wf->setAnalysisProgress(0.0);
                                                  }
                                                  wf->setAnalysisFailed(false);
                                              }

                                              if (deckIsA) {
                                                  windowPtr->analysisActiveA = !session.hasCache;
                                                  windowPtr->analysisFailedA = false;
                                                  windowPtr->analysisProgressA = std::clamp(coverage, 0.0, 1.0);
                                              } else {
                                                  windowPtr->analysisActiveB = !session.hasCache;
                                                  windowPtr->analysisFailedB = false;
                                                  windowPtr->analysisProgressB = std::clamp(coverage, 0.0, 1.0);
                                              }

                                              if (session.hasCache) {
                                                  windowPtr->reapplyStoredDeckMetadata(deckIsA);
                                              }

                                              windowPtr->updateOverviewLabel(deckIsA);
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    } catch (...) {
        dispatchFailure();
    }
}

WaveformStreamChunkTask::WaveformStreamChunkTask(QtMainWindow* mainWindow,
                                                 QString filePath,
                                                 bool isDeckA,
                                                 WaveformGenerator::AnalysisMetadata metadata,
                                                 int totalBins,
                                                 int startBin,
                                                 int binCount)
    : window(mainWindow),
      filePath(std::move(filePath)),
      isDeckA(isDeckA),
      metadata(std::move(metadata)),
      totalBins(totalBins),
      startBin(startBin),
      binCount(binCount) {
    setAutoDelete(true);
}

void WaveformStreamChunkTask::run() {
    QPointer<QtMainWindow> windowGuard = window;
    if (!windowGuard) {
        return;
    }

    try {
        QThread::currentThread()->setPriority(QThread::LowestPriority);
        WaveformGenerator generator;

        std::shared_ptr<std::vector<float>> maxPtr;
        std::shared_ptr<std::vector<float>> minPtr;

        std::vector<float> maxBins;
        std::vector<float> minBins;
        const bool success = generator.renderBinWindow(juce::File(filePath.toStdString()),
                                                       metadata,
                                                       totalBins,
                                                       startBin,
                                                       binCount,
                                                       maxBins,
                                                       minBins);

        if (success) {
            maxPtr = std::make_shared<std::vector<float>>(std::move(maxBins));
            minPtr = std::make_shared<std::vector<float>>(std::move(minBins));
        }

        if (auto* windowRaw = windowGuard.data()) {
            QMetaObject::invokeMethod(windowRaw,
                                      [w = windowGuard,
                                       deckIsA = isDeckA,
                                       path = filePath,
                                       start = startBin,
                                       maxPtr,
                                       minPtr,
                                       success]() {
                                          if (auto windowPtr = w.data()) {
                                              windowPtr->handleWaveformChunkResult(deckIsA,
                                                                                   path,
                                                                                   start,
                                                                                   maxPtr,
                                                                                   minPtr,
                                                                                   success);
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    } catch (...) {
        if (auto* windowRaw = windowGuard.data()) {
            QMetaObject::invokeMethod(windowRaw,
                                      [w = windowGuard,
                                       deckIsA = isDeckA,
                                       path = filePath,
                                       start = startBin]() {
                                          if (auto windowPtr = w.data()) {
                                              windowPtr->handleWaveformChunkResult(deckIsA, path, start, {}, {}, false);
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    }
}
