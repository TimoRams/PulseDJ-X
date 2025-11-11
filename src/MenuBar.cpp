#include "MenuBar.h"
#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "AppConfig.h"
#include <QApplication>
#include <QFileDialog>
#include <QStandardPaths>
#include <QJsonDocument>
#include <cmath>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QSettings>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QRegularExpression>
#include <QtGlobal>
#include <QDateTime>
#include <QColor>
#include <QStringList>
#include <QKeySequence>
#include <juce_core/juce_core.h>

namespace {
QString detectCppStandardString()
{
#if defined(_MSC_VER) && defined(_MSVC_LANG)
    const long standard = static_cast<long>(_MSVC_LANG);
#else
    const long standard = static_cast<long>(__cplusplus);
#endif

    struct Entry {
        long value;
        const char* name;
    };

    constexpr Entry entries[] = {
        {202302L, "C++23"},
        {202002L, "C++20"},
        {201703L, "C++17"},
        {201402L, "C++14"},
        {201103L, "C++11"},
        {199711L, "C++03/C++98"}
    };

    for (const Entry& entry : entries) {
        if (standard >= entry.value) {
            return QString("%1 (%2)").arg(QString::fromLatin1(entry.name), QString::number(standard));
        }
    }

    return QString("Unknown (%1)").arg(QString::number(standard));
}

// Reusable styles shared across methods
static const QString kCpuStyleGreen = QStringLiteral(
    "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
    "QProgressBar::chunk { background: #00aa00; border-radius: 2px; }"
);
static const QString kCpuStyleYellow = QStringLiteral(
    "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
    "QProgressBar::chunk { background: #ffaa00; border-radius: 2px; }"
);
static const QString kCpuStyleRed = QStringLiteral(
    "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
    "QProgressBar::chunk { background: #ff4444; border-radius: 2px; }"
);
static const QString kRamStyleBlue = QStringLiteral(
    "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
    "QProgressBar::chunk { background: #0066cc; border-radius: 2px; }"
);
static const QString kRamStyleRed = QStringLiteral(
    "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
    "QProgressBar::chunk { background: #ff4444; border-radius: 2px; }"
);
}

MenuBar::MenuBar(QtMainWindow* parent) 
    : QMenuBar(parent), mainWindow(parent), preferencesDialog(nullptr) {
    
    setNativeMenuBar(false);
    static const QString kMenuBarStyle = QStringLiteral(
        "QMenuBar {"
        "    background-color: #121212;"
        "    border: none;"
        "    padding: 0px;"
        "    color: #e0e0e0;"
        "    font-size: 11px;"
        "}"
        "QMenuBar::item {"
        "    padding: 4px 12px;"
        "    margin: 0px;"
        "    background: transparent;"
        "    color: #e0e0e0;"
        "}"
        "QMenuBar::item:selected {"
        "    background: #2a2a2a;"
        "    border-radius: 2px;"
        "}"
        "QMenu {"
        "    background-color: #1a1a1a;"
        "    color: #e0e0e0;"
        "    border: 1px solid #333;"
        "    border-radius: 4px;"
        "    padding: 4px;"
        "}"
        "QMenu::item {"
        "    padding: 6px 16px;"
        "    border-radius: 2px;"
        "}"
        "QMenu::item:selected {"
        "    background: #2d2d2d;"
        "}"
        "QMenu::separator {"
        "    height: 1px;"
        "    background: #333;"
        "    margin: 4px 0px;"
        "}"
    );
    setStyleSheet(kMenuBarStyle);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    
    setupLogoWidget();
    createMenuActions();
    setupMenus();
    setupSystemMonitoring();
}

void MenuBar::setupLogoWidget() {
    logoWidget = new QWidget(this);
    auto logoLayout = new QHBoxLayout(logoWidget);
    logoLayout->setContentsMargins(10, 2, 15, 2);
    logoLayout->setSpacing(8);
    
    logoText = new QLabel("BetaPulseX", this);
    logoText->setStyleSheet("color: #e0e0e0; font-size: 12px; font-weight: bold;");
    
    versionText = new QLabel("v1.0-beta", this);
    versionText->setStyleSheet("color: #888; font-size: 9px;");
    
    logoLayout->addWidget(logoText);
    logoLayout->addWidget(versionText);
    
    setCornerWidget(logoWidget, Qt::TopLeftCorner);
    registerDragRegion(logoWidget);
}

void MenuBar::createMenuActions() {
    importSettingsAction = new QAction("Import Settings...", this);
    importSettingsAction->setShortcut(QKeySequence::Open);
    importSettingsAction->setStatusTip("Import settings from a file");
    
    exportSettingsAction = new QAction("Export Settings...", this);
    exportSettingsAction->setShortcut(QKeySequence::SaveAs);
    exportSettingsAction->setStatusTip("Export current settings to a file");
    
    exitAction = new QAction("Exit", this);
    exitAction->setShortcut(QKeySequence::Quit);
    exitAction->setStatusTip("Exit BetaPulseX");
    
    performanceModeAction = new QAction("Performance Mode", this);
    performanceModeAction->setCheckable(true);
    performanceModeAction->setChecked(true);
    performanceModeAction->setStatusTip("Optimized for live performance and mixing");
    
    exportModeAction = new QAction("Export Mode", this);
    exportModeAction->setCheckable(true);
    exportModeAction->setStatusTip("Prepare and export your mixes");
    
    editModeAction = new QAction("Edit Mode", this);
    editModeAction->setCheckable(true);
    editModeAction->setStatusTip("Edit tracks, set cue points and loops");
    
    preferencesAction = new QAction("Preferences...", this);
    preferencesAction->setShortcut(QKeySequence::Preferences);
    preferencesAction->setStatusTip("Open preferences dialog");
    
    resetSettingsAction = new QAction("Reset to Defaults", this);
    resetSettingsAction->setStatusTip("Reset all settings to default values");
    
    aboutAction = new QAction("About BetaPulseX", this);
    aboutAction->setStatusTip("Show information about BetaPulseX");
    
    fullScreenAction = new QAction("Full Screen", this);
    fullScreenAction->setShortcut(QKeySequence("F11"));
    fullScreenAction->setStatusTip("Toggle full screen mode");
    fullScreenAction->setCheckable(true);

    alwaysOnTopAction = new QAction("Always On Top", this);
    alwaysOnTopAction->setStatusTip("Keep window always on top");
    alwaysOnTopAction->setCheckable(true);
    
    QActionGroup* modeGroup = new QActionGroup(this);
    modeGroup->addAction(performanceModeAction);
    modeGroup->addAction(exportModeAction);
    modeGroup->addAction(editModeAction);
    modeGroup->setExclusive(true);
    
    connect(performanceModeAction, &QAction::triggered, this, [this]() { 
        modeMenu->setTitle("Mode: Performance"); 
    });
    connect(exportModeAction, &QAction::triggered, this, [this]() { 
        modeMenu->setTitle("Mode: Export"); 
    });
    connect(editModeAction, &QAction::triggered, this, [this]() { 
        modeMenu->setTitle("Mode: Edit"); 
    });
    
    connect(preferencesAction, &QAction::triggered, this, &MenuBar::showPreferences);
    connect(fullScreenAction, &QAction::triggered, this, &MenuBar::toggleFullScreen);
    connect(alwaysOnTopAction, &QAction::triggered, this, &MenuBar::toggleAlwaysOnTop);
    connect(importSettingsAction, &QAction::triggered, this, &MenuBar::importSettings);
    connect(exportSettingsAction, &QAction::triggered, this, &MenuBar::exportSettings);
    connect(resetSettingsAction, &QAction::triggered, this, &MenuBar::resetSettings);
    connect(exitAction, &QAction::triggered, mainWindow, &QWidget::close);
    connect(aboutAction, &QAction::triggered, this, &MenuBar::showAbout);
}

void MenuBar::setupMenus() {
    fileMenu = addMenu("File");
    fileMenu->addAction(importSettingsAction);
    fileMenu->addAction(exportSettingsAction);
    // Preferences and reset live in File for quicker access
    fileMenu->addAction(preferencesAction);
    fileMenu->addAction(resetSettingsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction);

    editMenu = addMenu("Edit");
    QAction* emptyEdit = new QAction("Empty", this);
    emptyEdit->setEnabled(false);
    editMenu->addAction(emptyEdit);

    viewMenu = addMenu("View");
    viewMenu->addAction(fullScreenAction);
    viewMenu->addAction(alwaysOnTopAction);

    toolsMenu = addMenu("Tools");
    QAction* emptyTools = new QAction("Empty", this);
    emptyTools->setEnabled(false);
    toolsMenu->addAction(emptyTools);

    helpMenu = addMenu("Help");
    helpMenu->addAction(aboutAction);

    modeMenu = addMenu("Mode: Performance");
    modeMenu->addAction(performanceModeAction);
    modeMenu->addAction(exportModeAction);
    modeMenu->addAction(editModeAction);

    // Preferences and Reset to Default are not shown in Edit anymore
}

void MenuBar::toggleAlwaysOnTop() {
    if (alwaysOnTopAction->isChecked()) {
        mainWindow->setWindowFlag(Qt::WindowStaysOnTopHint, true);
        mainWindow->show();
        alwaysOnTopAction->setStatusTip("Window is always on top");
    } else {
        mainWindow->setWindowFlag(Qt::WindowStaysOnTopHint, false);
        mainWindow->show();
        alwaysOnTopAction->setStatusTip("Keep window always on top");
    }
}

void MenuBar::setupSystemMonitoring() {
    systemWidget = new QWidget(this);
    auto systemLayout = new QHBoxLayout(systemWidget);
    systemLayout->setContentsMargins(10, 2, 10, 2);
    systemLayout->setSpacing(5);
    
    auto masterWidget = new QWidget();
    auto masterLayout = new QVBoxLayout(masterWidget);
    masterLayout->setContentsMargins(0, 0, 0, 0);
    masterLayout->setSpacing(1);
    
    auto masterLabel = new QLabel("OUT");
    masterLabel->setStyleSheet("color: #888; font-size: 8px; font-weight: bold;");
    masterLabel->setAlignment(Qt::AlignCenter);
    
    masterLeftBar = new QProgressBar();
    masterRightBar = new QProgressBar();
    
    static const QString kLevelBarStyle = QStringLiteral(
        "QProgressBar { background: #333; border: none; height: 4px; width: 25px; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #00ff00, stop:0.7 #ffff00, stop:1 #ff0000); }"
    );
    
    masterLeftBar->setStyleSheet(kLevelBarStyle);
    masterRightBar->setStyleSheet(kLevelBarStyle);
    masterLeftBar->setRange(0, 100);
    masterRightBar->setRange(0, 100);
    masterLeftBar->setTextVisible(false);
    masterRightBar->setTextVisible(false);
    masterLeftBar->setFixedSize(25, 4);
    masterRightBar->setFixedSize(25, 4);
    
    masterLayout->addWidget(masterLabel);
    masterLayout->addWidget(masterLeftBar);
    masterLayout->addWidget(masterRightBar);
    
    cpuBar = new QProgressBar();
    cpuBar->setRange(0, 100);
    cpuBar->setValue(0);
    cpuBar->setFixedSize(30, 12);
    cpuBar->setTextVisible(true);
    static const QString kCpuStyleGreen = QStringLiteral(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #00aa00; border-radius: 2px; }"
    );
    static const QString kCpuStyleYellow = QStringLiteral(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #ffaa00; border-radius: 2px; }"
    );
    static const QString kCpuStyleRed = QStringLiteral(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #ff4444; border-radius: 2px; }"
    );
    cpuBar->setStyleSheet(kCpuStyleGreen);
    
    cpuLabel = new QLabel("CPU");
    cpuLabel->setStyleSheet("color: #888; font-size: 8px;");
    
    ramBar = new QProgressBar();
    ramBar->setRange(0, 100);
    ramBar->setValue(0);
    ramBar->setFixedSize(30, 12);
    ramBar->setTextVisible(true);
    static const QString kRamStyleBlue = QStringLiteral(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #0066cc; border-radius: 2px; }"
    );
    static const QString kRamStyleRed = QStringLiteral(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #ff4444; border-radius: 2px; }"
    );
    ramBar->setStyleSheet(kRamStyleBlue);
    
    ramLabel = new QLabel("RAM");
    ramLabel->setStyleSheet("color: #888; font-size: 8px;");
    
    batteryBar = new QProgressBar();
    batteryBar->setRange(0, 100);
    batteryBar->setValue(100);
    batteryBar->setFixedSize(30, 12);
    batteryBar->setTextVisible(true);
    static const QString kBatteryStyleOrange = QStringLiteral(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #ff8800; border-radius: 2px; }"
    );
    batteryBar->setStyleSheet(kBatteryStyleOrange);
    
    batteryLabel = new QLabel("BAT");
    batteryLabel->setStyleSheet("color: #888; font-size: 8px;");
    
    systemLayout->addWidget(masterWidget);
    systemLayout->addSpacing(10);
    
    auto cpuWidget = new QWidget();
    auto cpuLayout = new QVBoxLayout(cpuWidget);
    cpuLayout->setContentsMargins(0, 0, 0, 0);
    cpuLayout->setSpacing(0);
    cpuLayout->addWidget(cpuBar);
    cpuLayout->addWidget(cpuLabel);
    systemLayout->addWidget(cpuWidget);
    
    auto ramWidget = new QWidget();
    auto ramLayout = new QVBoxLayout(ramWidget);
    ramLayout->setContentsMargins(0, 0, 0, 0);
    ramLayout->setSpacing(0);
    ramLayout->addWidget(ramBar);
    ramLayout->addWidget(ramLabel);
    systemLayout->addWidget(ramWidget);
    
    auto batteryWidget = new QWidget();
    auto batteryLayout = new QVBoxLayout(batteryWidget);
    batteryLayout->setContentsMargins(0, 0, 0, 0);
    batteryLayout->setSpacing(0);
    batteryLayout->addWidget(batteryBar);
    batteryLayout->addWidget(batteryLabel);
    systemLayout->addWidget(batteryWidget);
    
    // Audio latency display
    auto latencyWidget = new QWidget();
    auto latencyLayout = new QVBoxLayout(latencyWidget);
    latencyLayout->setContentsMargins(0, 0, 0, 0);
    latencyLayout->setSpacing(0);
    
    latencyValue = new QLabel("0.0ms · -- kHz · -- smp");
    latencyValue->setStyleSheet("color: #00ccff; font-size: 9px; font-weight: bold;");
    latencyValue->setAlignment(Qt::AlignCenter);
    latencyValue->setFixedWidth(140);
    
    latencyLabel = new QLabel("LATENCY");
    latencyLabel->setStyleSheet("color: #888; font-size: 7px;");
    latencyLabel->setAlignment(Qt::AlignCenter);
    
    latencyLayout->addWidget(latencyValue);
    latencyLayout->addWidget(latencyLabel);
    systemLayout->addWidget(latencyWidget);
    
    systemLayout->addSpacing(15);
    
    windowControlsWidget = new QWidget();
    auto windowControlsLayout = new QHBoxLayout(windowControlsWidget);
    windowControlsLayout->setContentsMargins(0, 0, 0, 0);
    windowControlsLayout->setSpacing(2);
    
    auto minimizeBtn = new QPushButton("−", this);
    auto maximizeBtn = new QPushButton("□", this);
    auto closeBtn = new QPushButton("×", this);
    
    static const QString kBtnStyle = QStringLiteral(
        "QPushButton {"
        "    background-color: transparent;"
        "    border: none;"
        "    color: #e0e0e0;"
        "    font-size: 14px;"
        "    font-weight: bold;"
        "    min-width: 18px;"
        "    max-width: 18px;"
        "    min-height: 18px;"
        "    max-height: 18px;"
        "    padding: 0px;"
        "    margin: 1px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #3a3a3a;"
        "    border-radius: 2px;"
        "}"
    );
    static const QString kBtnStyleClose = QStringLiteral(
        "QPushButton {"
        "    background-color: transparent;"
        "    border: none;"
        "    color: #e0e0e0;"
        "    font-size: 14px;"
        "    font-weight: bold;"
        "    min-width: 18px;"
        "    max-width: 18px;"
        "    min-height: 18px;"
        "    max-height: 18px;"
        "    padding: 0px;"
        "    margin: 1px;"
        "}"
        "QPushButton:hover { background-color: #e74c3c; border-radius: 2px; }"
    );
    
    minimizeBtn->setStyleSheet(kBtnStyle);
    maximizeBtn->setStyleSheet(kBtnStyle);
    closeBtn->setStyleSheet(kBtnStyleClose);
    
    // Connect window control buttons
    connect(minimizeBtn, &QPushButton::clicked, mainWindow, &QWidget::showMinimized);
    connect(maximizeBtn, &QPushButton::clicked, [this]() {
        if (mainWindow->isMaximized()) {
            mainWindow->showNormal();
        } else {
            mainWindow->showMaximized();
        }
    });
    connect(closeBtn, &QPushButton::clicked, mainWindow, &QWidget::close);
    
    windowControlsLayout->addWidget(minimizeBtn);
    windowControlsLayout->addWidget(maximizeBtn);
    windowControlsLayout->addWidget(closeBtn);
    
    systemLayout->addWidget(windowControlsWidget);
    
    setCornerWidget(systemWidget, Qt::TopRightCorner);
    registerDragRegion(systemWidget);
    
    systemTimer = new QTimer(this);
    connect(systemTimer, &QTimer::timeout, this, &MenuBar::updateSystemStats);
    systemTimer->start(2000); // Update every 2 seconds
    
    // Initial update to avoid showing 0% on startup
    QTimer::singleShot(100, this, &MenuBar::updateSystemStats);
}

void MenuBar::updateSystemStats() {
    // Cross-platform CPU usage monitoring
    double cpuUsage = 0.0;
    
#ifdef Q_OS_LINUX
    QFile cpuFile("/proc/stat");
    if (cpuFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&cpuFile);
        QString line = in.readLine();
        if (line.startsWith("cpu ")) {
            QStringList values = line.split(" ", Qt::SkipEmptyParts);
            if (values.size() >= 5) {
                static long long lastIdle = 0, lastTotal = 0;
                long long idle = values[4].toLongLong();
                long long total = 0;
                for (int i = 1; i < values.size(); ++i) {
                    total += values[i].toLongLong();
                }
                
                if (lastTotal > 0) {
                    long long totalDiff = total - lastTotal;
                    long long idleDiff = idle - lastIdle;
                    if (totalDiff > 0) {
                        cpuUsage = 100.0 * (totalDiff - idleDiff) / totalDiff;
                    }
                }
                
                lastIdle = idle;
                lastTotal = total;
            }
        }
        cpuFile.close();
    }
#elif defined(Q_OS_WIN)
    // Windows: Use JUCE SystemStats
    cpuUsage = juce::SystemStats::getCpuUsage() * 100.0;
#elif defined(Q_OS_MACOS)
    // macOS: Use JUCE SystemStats
    cpuUsage = juce::SystemStats::getCpuUsage() * 100.0;
#endif
    
    updateCpuUsage(cpuUsage);
    
    // Cross-platform RAM usage monitoring
    double ramUsage = 0.0;
    
#ifdef Q_OS_LINUX
    QFile memFile("/proc/meminfo");
    if (memFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        long long totalMem = 0, availMem = 0;
        
        while (!memFile.atEnd()) {
            QString line = QString::fromLatin1(memFile.readLine()).trimmed();
            
            if (line.startsWith("MemTotal:")) {
                // Parse: "MemTotal:       32295968 kB"
                QString numStr = line.mid(9).trimmed(); // Remove "MemTotal:"
                numStr = numStr.split(' ').first(); // Get first token (the number)
                totalMem = numStr.toLongLong();
            } else if (line.startsWith("MemAvailable:")) {
                // Parse: "MemAvailable:   20393164 kB"
                QString numStr = line.mid(13).trimmed(); // Remove "MemAvailable:"
                numStr = numStr.split(' ').first(); // Get first token (the number)
                availMem = numStr.toLongLong();
            }
            
            // Break early if we have both values
            if (totalMem > 0 && availMem > 0) {
                break;
            }
        }
        
        memFile.close();
        
        if (totalMem > 0 && availMem > 0) {
            long long usedMem = totalMem - availMem;
            ramUsage = 100.0 * static_cast<double>(usedMem) / static_cast<double>(totalMem);
        }
    }
#elif defined(Q_OS_WIN)
    // Windows: Use JUCE SystemStats
    const juce::int64 totalRAM = juce::SystemStats::getMemorySizeInMegabytes();
    const juce::int64 freeRAM = juce::SystemStats::getMemorySizeInMegabytes() - 
                                (juce::SystemStats::getMemorySizeInMegabytes() * juce::SystemStats::getCpuUsage());
    if (totalRAM > 0) {
        ramUsage = 100.0 * (totalRAM - freeRAM) / totalRAM;
    }
#elif defined(Q_OS_MACOS)
    // macOS: Use system command
    QProcess process;
    process.start("vm_stat");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    
    long long pageSize = 4096; // Default page size
    long long pagesActive = 0, pagesWired = 0, pagesFree = 0, pagesInactive = 0;
    
    for (const QString& line : output.split('\n')) {
        if (line.contains("Pages active:")) {
            pagesActive = line.split(':')[1].trimmed().remove('.').toLongLong();
        } else if (line.contains("Pages wired down:")) {
            pagesWired = line.split(':')[1].trimmed().remove('.').toLongLong();
        } else if (line.contains("Pages free:")) {
            pagesFree = line.split(':')[1].trimmed().remove('.').toLongLong();
        } else if (line.contains("Pages inactive:")) {
            pagesInactive = line.split(':')[1].trimmed().remove('.').toLongLong();
        }
    }
    
    long long usedPages = pagesActive + pagesWired;
    long long totalPages = usedPages + pagesFree + pagesInactive;
    
    if (totalPages > 0) {
        ramUsage = 100.0 * usedPages / totalPages;
    }
#endif
    
    updateRamUsage(ramUsage);
    
    // Cross-platform battery monitoring
    int batteryLevel = 100;
    bool isCharging = false;
    
#ifdef Q_OS_LINUX
    // Try BAT0 first, then BAT1
    for (const QString& batteryPath : {"/sys/class/power_supply/BAT0", "/sys/class/power_supply/BAT1"}) {
        QFile batteryCapacityFile(batteryPath + "/capacity");
        if (batteryCapacityFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&batteryCapacityFile);
            batteryLevel = in.readLine().trimmed().toInt();
            batteryCapacityFile.close();
            
            QFile batteryStatusFile(batteryPath + "/status");
            if (batteryStatusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream statusIn(&batteryStatusFile);
                QString status = statusIn.readLine().trimmed();
                isCharging = (status == "Charging" || status == "Full");
                batteryStatusFile.close();
            }
            
            break; // Found a battery
        }
    }
#elif defined(Q_OS_WIN)
    // Windows: Use system power status
    QProcess process;
    process.start("powershell", QStringList() << "-Command" << 
                  "(Get-WmiObject Win32_Battery).EstimatedChargeRemaining");
    process.waitForFinished();
    QString output = process.readAllStandardOutput().trimmed();
    if (!output.isEmpty()) {
        batteryLevel = output.toInt();
    }
    
    QProcess chargingProcess;
    chargingProcess.start("powershell", QStringList() << "-Command" << 
                         "(Get-WmiObject Win32_Battery).BatteryStatus");
    chargingProcess.waitForFinished();
    QString chargingOutput = chargingProcess.readAllStandardOutput().trimmed();
    isCharging = (chargingOutput == "2"); // 2 = Charging
#elif defined(Q_OS_MACOS)
    // macOS: Use pmset command
    QProcess process;
    process.start("pmset", QStringList() << "-g" << "batt");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    
    // Parse output like: "Now drawing from 'Battery Power' -InternalBattery-0 (id=1234567)  85%; discharging; 3:45 remaining"
    QRegularExpression batteryRegex("(\\d+)%");
    QRegularExpressionMatch match = batteryRegex.match(output);
    if (match.hasMatch()) {
        batteryLevel = match.captured(1).toInt();
    }
    
    isCharging = output.contains("charging") || output.contains("AC Power");
#endif
    
    updateBatteryLevel(batteryLevel, isCharging);
    updateMasterLevels(0.0, 0.0);
}

void MenuBar::showPreferences() {
    if (!preferencesDialog) {
        preferencesDialog = new PreferencesDialog(mainWindow);
        
        if (mainWindow && mainWindow->getPlayerA() && mainWindow->getPlayerB()) {
            preferencesDialog->setPlayerReferences(mainWindow->getPlayerA(), mainWindow->getPlayerB(), mainWindow);
        } else {
        }
        
        connect(preferencesDialog, &PreferencesDialog::settingsChanged, [this]() {
            (void)this; /* placeholder to keep lambda non-empty without side effects */
        });
    }
    
    preferencesDialog->show();
    preferencesDialog->raise();
    preferencesDialog->activateWindow();
}

void MenuBar::exportSettings() {
    QString defaultPath = AppConfig::instance().getSettingsExportPath();
    QString fileName = QFileDialog::getSaveFileName(this, "Export Settings", defaultPath, "JSON Files (*.json)");
    
    if (!fileName.isEmpty()) {
        QSettings config(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat);
        
        QJsonObject jsonObj;
        
        QStringList groups = {"Audio", "Decks", "Interface", "Library", "Performance", "Advanced"};
        
        for (const QString& group : groups) {
            config.beginGroup(group);
            QJsonObject groupObj;
            
            for (const QString& key : config.childKeys()) {
                QVariant value = config.value(key);
                
                if (value.metaType() == QMetaType::fromType<QColor>()) {
                    QColor color = value.value<QColor>();
                    groupObj[key] = color.name();
                } else if (value.canConvert<QString>()) {
                    groupObj[key] = value.toString();
                } else if (value.canConvert<int>()) {
                    groupObj[key] = value.toInt();
                } else if (value.canConvert<double>()) {
                    groupObj[key] = value.toDouble();
                } else if (value.canConvert<bool>()) {
                    groupObj[key] = value.toBool();
                }
            }
            
            config.endGroup();
            jsonObj[group] = groupObj;
        }
        
        QJsonObject metadata;
        metadata["version"] = "1.0";
        metadata["exportDate"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        metadata["application"] = "BetaPulseX";
        jsonObj["metadata"] = metadata;
        
        QJsonDocument doc(jsonObj);
        QFile file(fileName);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(doc.toJson());
            file.close();
            
            QMessageBox::information(this, "Export Successful", 
                QString("Settings exported successfully to:\n%1").arg(fileName));
        } else {
            QMessageBox::warning(this, "Export Failed", 
                QString("Failed to write settings to:\n%1").arg(fileName));
        }
    }
}

void MenuBar::importSettings() {
    QString fileName = QFileDialog::getOpenFileName(this, "Import Settings", 
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), 
        "JSON Files (*.json)");
    
    if (!fileName.isEmpty()) {
        QFile file(fileName);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            file.close();
            
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(data, &error);
            
            if (error.error != QJsonParseError::NoError) {
                QMessageBox::warning(this, "Import Failed", 
                    QString("Failed to parse JSON file:\n%1").arg(error.errorString()));
                return;
            }
            
            QJsonObject jsonObj = doc.object();
            QSettings config(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat);
            
            QStringList groups = {"Audio", "Decks", "Interface", "Library", "Performance", "Advanced"};
            
            for (const QString& group : groups) {
                if (jsonObj.contains(group)) {
                    QJsonObject groupObj = jsonObj[group].toObject();
                    config.beginGroup(group);
                    
                    for (auto it = groupObj.begin(); it != groupObj.end(); ++it) {
                        QString key = it.key();
                        QJsonValue value = it.value();
                        
                        if (value.isString()) {
                            config.setValue(key, value.toString());
                        } else if (value.isDouble()) {
                            config.setValue(key, value.toDouble());
                        } else if (value.isBool()) {
                            config.setValue(key, value.toBool());
                        }
                    }
                    
                    config.endGroup();
                }
            }
            
            QMessageBox::information(this, "Import Successful", 
                "Settings imported successfully.\nRestart the application to apply all changes.");
        } else {
            QMessageBox::warning(this, "Import Failed", 
                QString("Failed to read settings file:\n%1").arg(fileName));
        }
    }
}

void MenuBar::resetSettings() {
    int reply = QMessageBox::question(this, "Reset Settings", 
        "Are you sure you want to reset all settings to their default values?\nThis action cannot be undone.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    
    if (reply == QMessageBox::Yes) {
        QSettings config(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat);
        config.clear();
        config.sync();
        
        QMessageBox::information(this, "Settings Reset", 
            "All settings have been reset to default values.\nRestart the application to apply the changes.");
    }
}

void MenuBar::showAbout() {
    QMessageBox about(this);
    about.setWindowTitle("About BetaPulseX");
    about.setTextFormat(Qt::RichText);
    const QString qtVersion = QString::fromLatin1(qVersion());
    const QString juceVersion = QString::fromStdString(juce::SystemStats::getJUCEVersion().toStdString());
    const QString cppVersion = detectCppStandardString();
    const QString aboutText = QStringLiteral(
        "<h3>BetaPulseX v1.0-beta</h3>"
        "<p>Professional DJ Software Suite</p>"
        "<p><b>Framework Versions</b></p>"
        "<ul>"
        "<li>Qt %1</li>"
        "<li>JUCE %2</li>"
        "<li>C++ %3</li>"
        "</ul>"
        "<br>"
        "<p><b>Highlights:</b></p>"
        "<ul>"
        "<li>High-quality audio engine with RubberBand keylock</li>"
        "<li>Advanced waveform analysis and visualization</li>"
        "<li>Professional mixing controls and effects</li>"
        "<li>Library management with smart playlists</li>"
        "</ul>"
        "<br>"
        "<p>Copyright © 2025 BetaPulseX Development Team</p>"
    ).arg(qtVersion, juceVersion, cppVersion);
    about.setText(aboutText);
    about.setStandardButtons(QMessageBox::Ok);
    about.exec();
}

void MenuBar::toggleFullScreen() {
    if (mainWindow->isFullScreen()) {
        mainWindow->showNormal();
        fullScreenAction->setChecked(false);
        fullScreenAction->setText("Full Screen");
        fullScreenAction->setStatusTip("Enter full screen mode");
    } else {
        mainWindow->showFullScreen();
        fullScreenAction->setChecked(true);
        fullScreenAction->setText("Exit Full Screen");
        fullScreenAction->setStatusTip("Exit full screen mode");
    }
}

void MenuBar::updateCpuUsage(double percentage) {
    const int p = static_cast<int>(percentage);
    cpuBar->setValue(p);
    cpuBar->setFormat(QString::number(p) + QLatin1Char('%'));
    if (percentage > 80) cpuBar->setStyleSheet(kCpuStyleRed);
    else if (percentage > 60) cpuBar->setStyleSheet(kCpuStyleYellow);
    else cpuBar->setStyleSheet(kCpuStyleGreen);
}

void MenuBar::updateRamUsage(double percentage) {
    const int p = static_cast<int>(percentage);
    ramBar->setValue(p);
    ramBar->setFormat(QString::number(p) + QLatin1Char('%'));
    if (percentage > 85) ramBar->setStyleSheet(kRamStyleRed);
    else ramBar->setStyleSheet(kRamStyleBlue);
}

void MenuBar::updateBatteryLevel(int percentage, bool isCharging) {
    batteryBar->setValue(percentage);
    batteryBar->setFormat(QString::number(percentage) + QLatin1Char('%'));
    
    QString color = "#ff8800"; // Default orange
    if (isCharging) {
        color = "#00aa00"; // Green when charging
    } else if (percentage < 20) {
        color = "#ff4444"; // Red when low
    } else if (percentage < 50) {
        color = "#ffaa00"; // Yellow when medium
    }
    
    batteryBar->setStyleSheet(
        QString("QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
                "QProgressBar::chunk { background: %1; border-radius: 2px; }").arg(color)
    );
}

void MenuBar::updateMasterLevels(double leftLevel, double rightLevel) {
    masterLeftBar->setValue(static_cast<int>(leftLevel * 100));
    masterRightBar->setValue(static_cast<int>(rightLevel * 100));
}

void MenuBar::updateAudioLatency(double latencyMs, double sampleRateHz, int bufferSizeSamples) {
    if (!latencyValue) return;

    QStringList segments;

    // Latency segment with adaptive precision
    QString latencyText;
    if (latencyMs < 1.0) {
        latencyText = QString("%1ms").arg(latencyMs, 0, 'f', 2);
    } else if (latencyMs < 10.0) {
        latencyText = QString("%1ms").arg(latencyMs, 0, 'f', 1);
    } else {
        latencyText = QString("%1ms").arg(static_cast<int>(latencyMs + 0.5));
    }
    segments << latencyText;

    // Sample rate segment (in kHz with sensible precision)
    if (sampleRateHz > 0.0) {
        const double srKHz = sampleRateHz / 1000.0;
        const int decimals = (std::abs(std::round(srKHz) - srKHz) < 0.05) ? 0 : 1;
        segments << QString("%1kHz").arg(srKHz, 0, 'f', decimals);
    } else {
        segments << QStringLiteral("-- kHz");
    }

    // Buffer size segment (always show something)
    if (bufferSizeSamples > 0) {
        segments << QString("%1 smp").arg(bufferSizeSamples);
    } else {
        segments << QStringLiteral("-- smp");
    }

    latencyValue->setText(segments.join(QStringLiteral(" · ")));

    // Color coding based on latency range
    QString color;
    if (latencyMs < 10.0) {
        color = "#00ff88";  // Green - excellent latency
    } else if (latencyMs < 20.0) {
        color = "#00ccff";  // Cyan - good latency
    } else if (latencyMs < 50.0) {
        color = "#ffaa00";  // Orange - acceptable
    } else {
        color = "#ff4444";  // Red - high latency
    }

    latencyValue->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold;").arg(color));
}

void MenuBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && mainWindow) {
        pressedAction = actionAt(event->pos());
        dragPending = true;
    dragStartGlobal = event->globalPosition().toPoint();

        if (!pressedAction) {
            const QPoint globalPoint = event->globalPosition().toPoint();
            beginWindowDrag(globalPoint);
            continueWindowDrag(globalPoint);
            event->accept();
            return;
        }
    } else {
        cancelPendingDrag();
    }

    QMenuBar::mousePressEvent(event);
}

void MenuBar::mouseMoveEvent(QMouseEvent* event) {
    if ((event->buttons() & Qt::LeftButton) && mainWindow) {
        if (draggingWindow) {
            continueWindowDrag(event->globalPosition().toPoint());
            event->accept();
            return;
        }

        if (dragPending) {
            const int distance = (event->globalPosition().toPoint() - dragStartGlobal).manhattanLength();
            if (distance >= QApplication::startDragDistance()) {
                if (pressedAction) {
                    setActiveAction(nullptr);
                }
                const QPoint globalPoint = event->globalPosition().toPoint();
                beginWindowDrag(globalPoint);
                continueWindowDrag(globalPoint);
                event->accept();
                return;
            }
        }
    }

    QMenuBar::mouseMoveEvent(event);
}

void MenuBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (draggingWindow) {
            endWindowDrag();
            event->accept();
            return;
        }
    }

    QMenuBar::mouseReleaseEvent(event);

    if (event->button() == Qt::LeftButton) {
        cancelPendingDrag();
    }
}

bool MenuBar::eventFilter(QObject* watched, QEvent* event) {
    if (!mainWindow) {
        return QMenuBar::eventFilter(watched, event);
    }

    auto watchedWidget = qobject_cast<QWidget*>(watched);
    if (windowControlsWidget) {
        if (watchedWidget == windowControlsWidget ||
            (windowControlsWidget && windowControlsWidget->isAncestorOf(watchedWidget))) {
            return QMenuBar::eventFilter(watched, event);
        }
    }

    switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                cancelPendingDrag();
                beginWindowDrag(mouseEvent->globalPosition().toPoint());
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->buttons() & Qt::LeftButton) {
                continueWindowDrag(mouseEvent->globalPosition().toPoint());
                if (draggingWindow) {
                    event->accept();
                    return true;
                }
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && draggingWindow) {
                endWindowDrag();
                cancelPendingDrag();
                event->accept();
                return true;
            }
            break;
        }
        default:
            break;
    }

    return QMenuBar::eventFilter(watched, event);
}

void MenuBar::registerDragRegion(QWidget* widget) {
    if (!widget || widget == windowControlsWidget || qobject_cast<QPushButton*>(widget)) {
        return;
    }

    widget->installEventFilter(this);

    const auto children = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : children) {
        registerDragRegion(child);
    }
}

void MenuBar::beginWindowDrag(const QPoint& globalPos) {
    if (!mainWindow) {
        draggingWindow = false;
        return;
    }

    mainWindow->beginExternalWindowDrag(globalPos);
    draggingWindow = true;
    cancelPendingDrag();
}

void MenuBar::continueWindowDrag(const QPoint& globalPos) {
    if (!draggingWindow || !mainWindow) {
        return;
    }

    mainWindow->updateExternalWindowDrag(globalPos);
}

void MenuBar::endWindowDrag() {
    if (draggingWindow && mainWindow) {
        mainWindow->endExternalWindowDrag();
    }
    draggingWindow = false;
}

void MenuBar::cancelPendingDrag() {
    dragPending = false;
    pressedAction = nullptr;
}
