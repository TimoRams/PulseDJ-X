#pragma once

#include <JuceHeader.h>
#include <QPointer>
#include <QRunnable>
#include <QString>
#include <vector>

class QtMainWindow;

class AudioFileLoadTask : public QRunnable {
public:
    AudioFileLoadTask(QtMainWindow* mainWindow, QString filePath, bool isDeckA);
    void run() override;

private:
    QPointer<QtMainWindow> window;
    QString filePath;
    bool isDeckA{false};
};

class BpmAnalysisTask : public QRunnable {
public:
    enum class Target { DeckA, DeckB, LibraryOnly };

    BpmAnalysisTask(QtMainWindow* mainWindow,
                    juce::File file,
                    Target target,
                    double minBpm = 40.0,
                    double maxBpm = 260.0);

    void run() override;

private:
    QPointer<QtMainWindow> window;
    juce::File audioFile;
    Target target{Target::LibraryOnly};
    double minBpm{40.0};
    double maxBpm{260.0};
};

class TopWaveformDisplayTask : public QRunnable {
public:
    TopWaveformDisplayTask(QtMainWindow* mainWindow, QString filePath, bool isDeckA);
    void run() override;

private:
    QPointer<QtMainWindow> window;
    QString filePath;
    bool isDeckA{false};
};
