#pragma once

#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QString>
#include <QDebug>

/**
 * Zentrale Konfigurationsklasse für BetaPulseX/DJDavid
 * 
 * - Debug/Development Build: Verwendet "BetaPulseX" Ordner im Projektverzeichnis
 * - Release Build: Verwendet Standard User-Verzeichnisse (AppData/Documents etc.)
 */
class AppConfig {
public:
    static AppConfig& instance() {
        static AppConfig instance;
        return instance;
    }
    
    // Hauptverzeichnis für alle App-Daten
    QString getAppDataDirectory() const {
        return appDataDir;
    }
    
    // Spezifische Unterverzeichnisse
    QString getConfigDirectory() const {
        return appDataDir + "/config";
    }
    
    QString getLibraryDirectory() const {
        return appDataDir + "/library";
    }
    
    QString getCacheDirectory() const {
        return appDataDir + "/cache";
    }
    
    QString getWaveformCacheDirectory() const {
        return appDataDir + "/waveforms";
    }
    
    QString getBpmCacheDirectory() const {
        return appDataDir + "/bpm_cache";
    }
    
    QString getPresetsDirectory() const {
        return appDataDir + "/presets";
    }
    
    QString getLogsDirectory() const {
        return appDataDir + "/logs";
    }
    
    // Spezifische Dateipfade
    QString getLibraryDatabasePath() const {
        return getLibraryDirectory() + "/libraryItems.xml";
    }
    
    QString getSettingsPath() const {
        return getConfigDirectory() + "/settings.ini";
    }
    
    // Prüft ob Debug/Development Build
    bool isDebugBuild() const {
        return debugBuild;
    }
    
    // Erstellt alle notwendigen Verzeichnisse
    bool createDirectories() const {
        QStringList dirs = {
            getAppDataDirectory(),
            getConfigDirectory(),
            getLibraryDirectory(),
            getCacheDirectory(),
            getWaveformCacheDirectory(),
            getBpmCacheDirectory(),
            getPresetsDirectory(),
            getLogsDirectory()
        };
        
        for (const QString& dir : dirs) {
            QDir qdir;
            if (!qdir.mkpath(dir)) {
                qDebug() << "Failed to create directory:" << dir;
                return false;
            }
        }
        
        qDebug() << "App directories created successfully in:" << getAppDataDirectory();
        return true;
    }
    
private:
    AppConfig() {
        initializeDirectories();
    }
    
    void initializeDirectories() {
        // Debug-Build erkennen (verschiedene Methoden)
        debugBuild = false;
        
#ifdef _DEBUG
        debugBuild = true;
#endif

#ifdef DEBUG
        debugBuild = true;
#endif

#ifndef NDEBUG
        debugBuild = true;
#endif
        
        // Zusätzlich: Prüfe ob CMAKE_BUILD_TYPE=Debug gesetzt war
        QString buildType = qgetenv("CMAKE_BUILD_TYPE");
        if (buildType.toLower() == "debug") {
            debugBuild = true;
        }
        
        // Prüfe ob Executable im "build" Verzeichnis liegt (Development Indikator)
        QString execPath = QCoreApplication::applicationDirPath();
        if (execPath.contains("/build") || execPath.endsWith("/build")) {
            debugBuild = true;
        }
        
        if (debugBuild) {
            // DEVELOPMENT/DEBUG: BetaPulseX im Projektverzeichnis
            QString projectRoot = QCoreApplication::applicationDirPath();
            
            // Wenn wir im build/ Ordner sind, gehe eine Ebene hoch
            if (projectRoot.endsWith("/build")) {
                projectRoot = QDir(projectRoot).absoluteFilePath("..");
            }
            
            appDataDir = QDir(projectRoot).absoluteFilePath("BetaPulseX");
            qDebug() << "DEBUG BUILD detected - Using project directory:" << appDataDir;
            
        } else {
            // RELEASE: Standard User-Verzeichnisse
            QString userDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            if (userDataDir.isEmpty()) {
                // Fallback für Linux/macOS
                userDataDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.DJDavid";
            }
            
            appDataDir = userDataDir;
            qDebug() << "RELEASE BUILD detected - Using user directory:" << appDataDir;
        }
        
        // Normalisiere Pfad
        appDataDir = QDir::cleanPath(appDataDir);
    }
    
    QString appDataDir;
    bool debugBuild = false;
};

// Convenience Makros für häufig verwendete Pfade
#define APP_CONFIG AppConfig::instance()
#define APP_DATA_DIR APP_CONFIG.getAppDataDirectory()
#define APP_CONFIG_DIR APP_CONFIG.getConfigDirectory()
#define APP_LIBRARY_DIR APP_CONFIG.getLibraryDirectory()
#define APP_CACHE_DIR APP_CONFIG.getCacheDirectory()
