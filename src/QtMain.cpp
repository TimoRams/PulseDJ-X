#include "QtMainWindow.h"
#include "AppConfig.h"
#include <QApplication>
#include <QDebug>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    
    // BetaPulseX: Initialisiere App-Konfiguration und erstelle Verzeichnisse
    qDebug() << "=== BetaPulseX DJ Software Starting ===";
    qDebug() << "Build Type:" << (AppConfig::instance().isDebugBuild() ? "DEBUG/DEVELOPMENT" : "RELEASE");
    qDebug() << "Data Directory:" << AppConfig::instance().getAppDataDirectory();
    
    // Erstelle alle notwendigen Verzeichnisse
    if (!AppConfig::instance().createDirectories()) {
        qWarning() << "Failed to create app directories - some features may not work!";
    }

    QtMainWindow w;
    // Make window wider by default and enforce a minimum size so loading tracks
    // can't slightly shift or expand the main window layout.
    const int defaultW = 1400;
    const int defaultH = 900;
    w.resize(defaultW, defaultH);
    w.setMinimumSize(defaultW, defaultH);
    w.show();

    return app.exec();
}
