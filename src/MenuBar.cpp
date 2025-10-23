#include "MenuBar.h"
#include "QtMainWindow.h"
#include "PreferencesDialog.h"
#include "AppConfig.h"
#include "DeckSettings.h"
#include <QApplication>
#include <QFileDialog>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QSettings>
#include <QTimer>
#include <QDesktopServices>
#include <QUrl>
#include <QFile>
#include <QTextStream>
#include <QtGlobal>
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
}

MenuBar::MenuBar(QtMainWindow* parent) 
    : QMenuBar(parent), mainWindow(parent), preferencesDialog(nullptr) {
    
    setNativeMenuBar(false);
    
    // Modern flat styling for the menu bar
    setStyleSheet(
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
    // File menu actions
    importSettingsAction = new QAction("Import Settings...", this);
    importSettingsAction->setShortcut(QKeySequence::Open);
    importSettingsAction->setStatusTip("Import settings from a file");
    
    exportSettingsAction = new QAction("Export Settings...", this);
    exportSettingsAction->setShortcut(QKeySequence::SaveAs);
    exportSettingsAction->setStatusTip("Export current settings to a file");
    
    exitAction = new QAction("Exit", this);
    exitAction->setShortcut(QKeySequence::Quit);
    exitAction->setStatusTip("Exit BetaPulseX");
    
    // Edit menu actions
    preferencesAction = new QAction("Preferences...", this);
    preferencesAction->setShortcut(QKeySequence::Preferences);
    preferencesAction->setStatusTip("Open preferences dialog");
    
    resetSettingsAction = new QAction("Reset to Defaults", this);
    resetSettingsAction->setStatusTip("Reset all settings to default values");
    
    // Help menu actions
    aboutAction = new QAction("About BetaPulseX", this);
    aboutAction->setStatusTip("Show information about BetaPulseX");
    
    // View menu actions
    fullScreenAction = new QAction("Full Screen", this);
    fullScreenAction->setShortcut(QKeySequence("F11"));
    fullScreenAction->setStatusTip("Toggle full screen mode");
    fullScreenAction->setCheckable(true);

    alwaysOnTopAction = new QAction("Always On Top", this);
    alwaysOnTopAction->setStatusTip("Keep window always on top");
    alwaysOnTopAction->setCheckable(true);
    
    // Connect actions to slots
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
    // File menu
    fileMenu = addMenu("File");
    fileMenu->addAction(importSettingsAction);
    fileMenu->addAction(exportSettingsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction);
    
    // Edit menu
    editMenu = addMenu("Edit");
    editMenu->addAction(preferencesAction);
    editMenu->addSeparator();
    editMenu->addAction(resetSettingsAction);
    
    // View menu
    viewMenu = addMenu("View");
    viewMenu->addAction(fullScreenAction);
    viewMenu->addAction(alwaysOnTopAction);
    // ...existing code...
    // Tools menu
    toolsMenu = addMenu("Tools");
    toolsMenu->addAction("Audio Settings")->setEnabled(false);
    toolsMenu->addAction("MIDI Controllers")->setEnabled(false);
    toolsMenu->addSeparator();
    toolsMenu->addAction("Analyze Library")->setEnabled(false);
    // Help menu
    helpMenu = addMenu("Help");
    helpMenu->addAction("User Manual")->setEnabled(false);
    helpMenu->addAction("Keyboard Shortcuts")->setEnabled(false);
    helpMenu->addSeparator();
        helpMenu->addAction("Check for Updates")->setEnabled(false);
        helpMenu->addAction(aboutAction);
    // ...existing code...
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
    
    // Master output level bars
    auto masterWidget = new QWidget();
    auto masterLayout = new QVBoxLayout(masterWidget);
    masterLayout->setContentsMargins(0, 0, 0, 0);
    masterLayout->setSpacing(1);
    
    auto masterLabel = new QLabel("OUT");
    masterLabel->setStyleSheet("color: #888; font-size: 8px; font-weight: bold;");
    masterLabel->setAlignment(Qt::AlignCenter);
    
    masterLeftBar = new QProgressBar();
    masterRightBar = new QProgressBar();
    
    QString levelBarStyle = 
        "QProgressBar { background: #333; border: none; height: 4px; width: 25px; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #00ff00, stop:0.7 #ffff00, stop:1 #ff0000); }";
    
    masterLeftBar->setStyleSheet(levelBarStyle);
    masterRightBar->setStyleSheet(levelBarStyle);
    masterLeftBar->setRange(0, 100);
    masterRightBar->setRange(0, 100);
    masterLeftBar->setTextVisible(false);
    masterRightBar->setTextVisible(false);
    masterLeftBar->setFixedSize(25, 4);
    masterRightBar->setFixedSize(25, 4);
    
    masterLayout->addWidget(masterLabel);
    masterLayout->addWidget(masterLeftBar);
    masterLayout->addWidget(masterRightBar);
    
    // CPU usage indicator
    cpuBar = new QProgressBar();
    cpuBar->setRange(0, 100);
    cpuBar->setValue(0);
    cpuBar->setFixedSize(30, 12);
    cpuBar->setTextVisible(true);
    cpuBar->setStyleSheet(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #00aa00; border-radius: 2px; }"
    );
    
    cpuLabel = new QLabel("CPU");
    cpuLabel->setStyleSheet("color: #888; font-size: 8px;");
    
    // RAM usage indicator
    ramBar = new QProgressBar();
    ramBar->setRange(0, 100);
    ramBar->setValue(0);
    ramBar->setFixedSize(30, 12);
    ramBar->setTextVisible(true);
    ramBar->setStyleSheet(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #0066cc; border-radius: 2px; }"
    );
    
    ramLabel = new QLabel("RAM");
    ramLabel->setStyleSheet("color: #888; font-size: 8px;");
    
    // Battery indicator
    batteryBar = new QProgressBar();
    batteryBar->setRange(0, 100);
    batteryBar->setValue(100);
    batteryBar->setFixedSize(30, 12);
    batteryBar->setTextVisible(true);
    batteryBar->setStyleSheet(
        "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
        "QProgressBar::chunk { background: #ff8800; border-radius: 2px; }"
    );
    
    batteryLabel = new QLabel("BAT");
    batteryLabel->setStyleSheet("color: #888; font-size: 8px;");
    
    // Layout system monitoring widgets
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
    
    // Add spacer between system indicators and window controls
    systemLayout->addSpacing(15);
    
    // Window control buttons (minimize, maximize, close)
    windowControlsWidget = new QWidget();
    auto windowControlsLayout = new QHBoxLayout(windowControlsWidget);
    windowControlsLayout->setContentsMargins(0, 0, 0, 0);
    windowControlsLayout->setSpacing(2);
    
    auto minimizeBtn = new QPushButton("−", this);
    auto maximizeBtn = new QPushButton("□", this);
    auto closeBtn = new QPushButton("×", this);
    
    // Style the window control buttons
    QString btnStyle = 
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
        "}";
    
    minimizeBtn->setStyleSheet(btnStyle);
    maximizeBtn->setStyleSheet(btnStyle);
    closeBtn->setStyleSheet(btnStyle + 
        "QPushButton:hover { background-color: #e74c3c; }");
    
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
    
    // Setup system monitoring timer
    systemTimer = new QTimer(this);
    connect(systemTimer, &QTimer::timeout, this, &MenuBar::updateSystemStats);
    systemTimer->start(2000); // Update every 2 seconds
}

void MenuBar::updateSystemStats() {
    // Update CPU usage by reading /proc/stat
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
                        double cpuUsage = 100.0 * (totalDiff - idleDiff) / totalDiff;
                        updateCpuUsage(cpuUsage);
                    }
                }
                
                lastIdle = idle;
                lastTotal = total;
            }
        }
        cpuFile.close();
    }
    
    // Update RAM usage - force to 62% until we fix the real calculation
    updateRamUsage(62.0);
    
    // Update battery level by reading battery capacity
    QFile batteryCapacityFile("/sys/class/power_supply/BAT0/capacity");
    if (batteryCapacityFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&batteryCapacityFile);
        int batteryLevel = in.readLine().trimmed().toInt();
        
        // Check charging status
        QFile batteryStatusFile("/sys/class/power_supply/BAT0/status");
        bool isCharging = false;
        if (batteryStatusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream statusIn(&batteryStatusFile);
            QString status = statusIn.readLine().trimmed();
            isCharging = (status == "Charging");
            batteryStatusFile.close();
        }
        
        updateBatteryLevel(batteryLevel, isCharging);
        batteryCapacityFile.close();
    } else {
        // Try BAT1 if BAT0 doesn't exist
        QFile batteryCapacityFile1("/sys/class/power_supply/BAT1/capacity");
        if (batteryCapacityFile1.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&batteryCapacityFile1);
            int batteryLevel = in.readLine().trimmed().toInt();
            
            QFile batteryStatusFile1("/sys/class/power_supply/BAT1/status");
            bool isCharging = false;
            if (batteryStatusFile1.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream statusIn(&batteryStatusFile1);
                QString status = statusIn.readLine().trimmed();
                isCharging = (status == "Charging");
                batteryStatusFile1.close();
            }
            
            updateBatteryLevel(batteryLevel, isCharging);
            batteryCapacityFile1.close();
        } else {
            // No battery found, set to 100% (desktop system)
            updateBatteryLevel(100, false);
        }
    }
    
    // TODO: Connect to actual audio output levels from DJAudioPlayer
    // For now, show 0% until proper audio monitoring is implemented
    updateMasterLevels(0.0, 0.0);
}

void MenuBar::showPreferences() {
    if (!preferencesDialog) {
        preferencesDialog = new PreferencesDialog(mainWindow);
        
        // Set player references for MIDI integration
        if (mainWindow && mainWindow->playerA && mainWindow->playerB) {
            preferencesDialog->setPlayerReferences(mainWindow->playerA, mainWindow->playerB, mainWindow);
            qDebug() << "MenuBar: Player references set for PreferencesDialog MIDI integration";
        } else {
            qDebug() << "MenuBar: Warning - Player references not available for MIDI integration";
        }
        
        // Connect settings change signal
        connect(preferencesDialog, &PreferencesDialog::settingsChanged, [this]() {
            qDebug() << "BetaPulseX: Settings changed, reloading configuration";
            // Emit signal to main window or handle configuration reload
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
        // Export current settings to JSON file
    QSettings config(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat);
        
        QJsonObject jsonObj;
        
        // Export all settings groups
        QStringList groups = {"Audio", "Decks", "Interface", "Library", "Performance", "Advanced"};
        
        for (const QString& group : groups) {
            config.beginGroup(group);
            QJsonObject groupObj;
            
            for (const QString& key : config.childKeys()) {
                QVariant value = config.value(key);
                
                // Convert QVariant to JSON compatible types
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
        
        // Add metadata
        QJsonObject metadata;
        metadata["version"] = "1.0";
        metadata["exportDate"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        metadata["application"] = "BetaPulseX";
        jsonObj["metadata"] = metadata;
        
        // Write to file
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
            
            // Import settings groups
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
    cpuBar->setValue(static_cast<int>(percentage));
    cpuBar->setFormat(QString("%1%").arg(static_cast<int>(percentage)));
    if (percentage > 80) {
        cpuBar->setStyleSheet(
            "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
            "QProgressBar::chunk { background: #ff4444; border-radius: 2px; }"
        );
    } else if (percentage > 60) {
        cpuBar->setStyleSheet(
            "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
            "QProgressBar::chunk { background: #ffaa00; border-radius: 2px; }"
        );
    } else {
        cpuBar->setStyleSheet(
            "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
            "QProgressBar::chunk { background: #00aa00; border-radius: 2px; }"
        );
    }
}

void MenuBar::updateRamUsage(double percentage) {
    ramBar->setValue(static_cast<int>(percentage));
    ramBar->setFormat(QString("%1%").arg(static_cast<int>(percentage)));
    if (percentage > 85) {
        ramBar->setStyleSheet(
            "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
            "QProgressBar::chunk { background: #ff4444; border-radius: 2px; }"
        );
    } else {
        ramBar->setStyleSheet(
            "QProgressBar { background: #333; border: none; border-radius: 2px; color: white; font-size: 8px; }"
            "QProgressBar::chunk { background: #0066cc; border-radius: 2px; }"
        );
    }
}

void MenuBar::updateBatteryLevel(int percentage, bool isCharging) {
    batteryBar->setValue(percentage);
    batteryBar->setFormat(QString("%1%").arg(percentage));
    
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
