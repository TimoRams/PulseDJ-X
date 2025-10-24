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
    if (!window) {
        return;
    }

    try {
        QThread::currentThread()->setPriority(QThread::LowestPriority);
        WaveformGenerator gen;
        WaveformGenerator::Result res;
        const int binCount = 16000;
        if (!gen.generate(juce::File(filePath.toStdString()), binCount, res)) {
            return;
        }

        auto maxBins = std::make_shared<std::vector<float>>(res.maxBins);
        auto minBins = std::make_shared<std::vector<float>>(res.minBins);

        const double audioStart = res.audioStartOffsetSec;
        const double lengthSec = res.lengthSeconds;

        const bool onDeckA = isDeckA;
        QMetaObject::invokeMethod(window,
                                  [w = window,
                                   maxBins,
                                   minBins,
                                   audioStart,
                                   lengthSec,
                                   onDeckA,
                                   filePath = this->filePath]() {
                                      if (!w) {
                                          return;
                                      }

                                      QtDeckWidget* deck = onDeckA ? w->deckA : w->deckB;
                                      if (!deck) {
                                          return;
                                      }
                                      if (deck->getCurrentFilePath() != filePath) {
                                          return;
                                      }
                                      WaveformDisplay* wf = onDeckA ? w->overviewTopA : w->overviewTopB;
                                      if (wf) {
                                          wf->setSourceBins(*maxBins, *minBins, audioStart, lengthSec);
                                          w->reapplyStoredDeckMetadata(onDeckA);
                                      }
                                  },
                                  Qt::QueuedConnection);
    } catch (...) {
        // Non-critical task; ignore failures.
    }
}
