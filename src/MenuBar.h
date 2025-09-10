#pragma once

#include <QMenuBar>
#include <QMenu>
#include <QAction>
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

    // Parent window reference
    QtMainWindow* mainWindow;

    // Menus
    QMenu* fileMenu;
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

    // System monitoring timer
    QTimer* systemTimer;

    // Preferences dialog
    PreferencesDialog* preferencesDialog;
};
