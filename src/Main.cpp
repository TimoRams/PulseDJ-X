#include "MainWindow.h"
#include "AppConfig.h"
#include "FrameTiming.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QtCore/QEventLoop>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QVBoxLayout>
#include <QSurfaceFormat>

#include <memory>
#include <expected>
#include <print>

namespace
{
constexpr QSize kDefaultWindowSize{1400, 900};
constexpr bool kEnableGpuAcceleration = true;

void configureSurfaceDefaults() noexcept
{
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL, kEnableGpuAcceleration);
    QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL, !kEnableGpuAcceleration);
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES, false);

    auto fmt = QSurfaceFormat::defaultFormat();
    if (kEnableGpuAcceleration) {
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        fmt.setVersion(4, 1);
    } else {
        fmt.setRenderableType(QSurfaceFormat::DefaultRenderableType);
        fmt.setProfile(QSurfaceFormat::NoProfile);
    }
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(FrameTiming::kVSyncSwapInterval);
    QSurfaceFormat::setDefaultFormat(fmt);
}

inline void processUiEvents() noexcept
{
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}
}

class LoadingDialog final : public QDialog
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

    void updateStatus(int value, QString text) noexcept
    {
        progressBar->setValue(value);
        statusLabel->setText(std::move(text));
    }

private:
    QLabel* titleLabel{};
    QLabel* statusLabel{};
    QProgressBar* progressBar{};
};

int main(int argc, char** argv)
{
    configureSurfaceDefaults();
    QApplication app(argc, argv);

    auto splash = std::make_unique<LoadingDialog>();
    splash->show();
    splash->raise();
    splash->activateWindow();
    processUiEvents();

    const auto updateSplash = [&splash](int value, QString status) noexcept {
        splash->updateStatus(value, std::move(status));
        processUiEvents();
    };

    updateSplash(5, QObject::tr("Initialisiere System..."));

    const auto& config = AppConfig::instance();
    qDebug() << "=== BetaPulseX DJ Software Starting ===";
    qDebug() << "Build Type:" << (config.isDebugBuild() ? "DEBUG/DEVELOPMENT" : "RELEASE");
    qDebug() << "Data Directory:" << config.getAppDataDirectory();
    qDebug() << "Config Directory:" << config.getConfigDirectory();
    qDebug() << "Library Database:" << config.getLibraryDatabasePath();
    qDebug() << "GPU Acceleration:" << (kEnableGpuAcceleration ? "ENABLED" : "DISABLED");

    updateSplash(25, QObject::tr("Prüfe Datenordner und Einstellungen..."));

    if (!config.createDirectories()) [[unlikely]] {
        qWarning() << "Failed to create app directories - some features may not work!";
    }

    updateSplash(55, QObject::tr("Initialisiere Benutzeroberfläche..."));

    QtMainWindow mainWindow;
    mainWindow.resize(kDefaultWindowSize);
    mainWindow.setMinimumSize(kDefaultWindowSize);

    updateSplash(80, QObject::tr("Starte Audio- und UI-Komponenten..."));

    mainWindow.show();

    updateSplash(100, QObject::tr("Bereit zum Durchstarten!"));
    splash->close();

    return app.exec();
}
