#include "QtMainWindow.h"
#include "AppConfig.h"
#include <QApplication>
#include <QDebug>
#include <QtCore/QEventLoop>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QVBoxLayout>

class LoadingDialog : public QDialog
{
public:
    explicit LoadingDialog(QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowFlag(Qt::FramelessWindowHint);
        setWindowFlag(Qt::WindowStaysOnTopHint);
        setWindowFlag(Qt::CustomizeWindowHint);
        setWindowModality(Qt::ApplicationModal);
        setAttribute(Qt::WA_TranslucentBackground, false);
        setFixedSize(360, 140);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(16);

        titleLabel = new QLabel(tr("BetaPulseX startet..."), this);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setWordWrap(true);
        titleLabel->setStyleSheet("font-size: 16px; font-weight: 600;");

        statusLabel = new QLabel(tr("Vorbereitung läuft"), this);
        statusLabel->setAlignment(Qt::AlignCenter);
        statusLabel->setWordWrap(true);

        progressBar = new QProgressBar(this);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setTextVisible(false);
        progressBar->setFixedHeight(10);
        progressBar->setStyleSheet(
            "QProgressBar {"
            "    border: 1px solid rgba(255, 255, 255, 40);"
            "    border-radius: 5px;"
            "    background-color: rgba(255, 255, 255, 25);"
            "}"
            "QProgressBar::chunk {"
            "    border-radius: 5px;"
            "    background-color: #00C9A7;"
            "}"
        );

        layout->addWidget(titleLabel);
        layout->addWidget(statusLabel);
        layout->addWidget(progressBar);

        setStyleSheet(
            "QDialog {"
            "    background-color: rgba(20, 26, 31, 230);"
            "    color: white;"
            "    border-radius: 12px;"
            "}"
        );
    }

    void updateStatus(int value, const QString& text)
    {
        progressBar->setValue(value);
        statusLabel->setText(text);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

private:
    QLabel* titleLabel{nullptr};
    QLabel* statusLabel{nullptr};
    QProgressBar* progressBar{nullptr};
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    LoadingDialog splash;
    splash.show();
    splash.raise();
    splash.activateWindow();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    splash.updateStatus(5, QObject::tr("Initialisiere System..."));
    
    // BetaPulseX: Initialisiere App-Konfiguration und erstelle Verzeichnisse
    qDebug() << "=== BetaPulseX DJ Software Starting ===";
    qDebug() << "Build Type:" << (AppConfig::instance().isDebugBuild() ? "DEBUG/DEVELOPMENT" : "RELEASE");
    qDebug() << "Data Directory:" << AppConfig::instance().getAppDataDirectory();
    qDebug() << "Config Directory:" << AppConfig::instance().getConfigDirectory();
    qDebug() << "Library Database:" << AppConfig::instance().getLibraryDatabasePath();
    
    splash.updateStatus(25, QObject::tr("Prüfe Datenordner und Einstellungen..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // Erstelle alle notwendigen Verzeichnisse
    if (!AppConfig::instance().createDirectories()) {
        qWarning() << "Failed to create app directories - some features may not work!";
    }

    splash.updateStatus(55, QObject::tr("Initialisiere Benutzeroberfläche..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QtMainWindow w;
    // Make window wider by default and enforce a minimum size so loading tracks
    // can't slightly shift or expand the main window layout.
    const int defaultW = 1400;
    const int defaultH = 900;
    w.resize(defaultW, defaultH);
    w.setMinimumSize(defaultW, defaultH);
    splash.updateStatus(80, QObject::tr("Starte Audio- und UI-Komponenten..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    w.show();

    splash.updateStatus(100, QObject::tr("Bereit zum Durchstarten!"));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    splash.close();

    return app.exec();
}
