#pragma once

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QMouseEvent>
#include <QPoint>
#include <QEvent>

class QtMainWindow;
class PreferencesDialog;

class MenuBar : public QMenuBar {
    Q_OBJECT

public:
    explicit MenuBar(QtMainWindow* parent);
    ~MenuBar() = default;

    // Public methods to access specific actions
    QAction* getPreferencesAction() const { return preferencesAction; }
    QAction* getExitAction() const { return exitAction; }
    QAction* getAboutAction() const { return aboutAction; }

    // System monitoring update methods
    void updateCpuUsage(double percentage);
    void updateRamUsage(double percentage);
    void updateBatteryLevel(int percentage, bool isCharging);
    void updateMasterLevels(double leftLevel, double rightLevel);
    void updateAudioLatency(double latencyMs, double sampleRateHz, int bufferSizeSamples);

private slots:
    void updateSystemStats();
    void showPreferences();
    void exportSettings();
    void importSettings();
    void resetSettings();
    void showAbout();
    void toggleFullScreen();
    void toggleAlwaysOnTop();

private:
    void setupMenus();
    void setupLogoWidget();
    void setupSystemMonitoring();
    void createMenuActions();

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void registerDragRegion(QWidget* widget);
    void beginWindowDrag(const QPoint& globalPos);
    void continueWindowDrag(const QPoint& globalPos);
    void endWindowDrag();
    void cancelPendingDrag();

    // Parent window reference
    QtMainWindow* mainWindow;

    // Menus
    QMenu* fileMenu;
    QMenu* modeMenu;
    QMenu* editMenu;
    QMenu* viewMenu;
    QMenu* toolsMenu;
    QMenu* helpMenu;

    // Menu actions
    QAction* preferencesAction;
    QAction* importSettingsAction;
    QAction* exportSettingsAction;
    QAction* resetSettingsAction;
    QAction* exitAction;
    QAction* aboutAction;
    QAction* fullScreenAction;
    QAction* alwaysOnTopAction;
    
    // Mode actions
    QAction* performanceModeAction;
    QAction* exportModeAction;
    QAction* editModeAction;

    // Logo and branding
    QWidget* logoWidget;
    QLabel* logoText;
    QLabel* versionText;

    // System monitoring widgets
    QWidget* systemWidget;
    QProgressBar* masterLeftBar;
    QProgressBar* masterRightBar;
    QProgressBar* cpuBar;
    QProgressBar* ramBar;
    QProgressBar* batteryBar;
    QLabel* cpuLabel;
    QLabel* ramLabel;
    QLabel* batteryLabel;
    QLabel* latencyLabel;
    QLabel* latencyValue;

    // System monitoring timer
    QTimer* systemTimer;

    // Preferences dialog
    PreferencesDialog* preferencesDialog;

    // Window dragging state
    bool draggingWindow = false;
    bool dragPending = false;
    QPoint dragStartGlobal;
    QAction* pressedAction = nullptr;
    QWidget* windowControlsWidget = nullptr;
};
