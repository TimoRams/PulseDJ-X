#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QAbstractTableModel>
#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QMenu>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QTabWidget>
#include <QListWidget>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QProgressBar>
#include <QThread>
#include <QMutex>
#include <QMimeData>
#include <QApplication>
#include <QDrag>
#include <QSplitter>
#include <QTreeView>
#include <QFileSystemModel>
#include <QVector>
#include <QSet>
#include <vector>
#include <array>
#include <memory>
#include <optional>

// Forward declarations
namespace juce {
    class AudioFormatManager;
    class File;
}

class QMouseEvent;

class LibraryDatabase;
struct PlaylistRecord;
struct PlaylistItemRecord;

// Structure to hold track metadata
struct TrackInfo {
    QString filePath;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString year;
    double duration = 0.0; // in seconds
    double bpm = 0.0;
    QString key;
    qint64 fileSize = 0;
    QString comment;
    qint64 lastModified = 0;
    qint64 addedAt = 0;
    qint64 updatedAt = 0;
    qint64 analyzedAt = 0;
    QString analysisAlgorithm;
    double firstBeatOffset = 0.0;
    double trackLengthSeconds = 0.0;
    QVector<double> beatPositions;
    bool analysisFailed = false;
    std::array<double, 8> cuePoints;
    bool hasCuePoints = false;
    
    TrackInfo() { cuePoints.fill(-1.0); }
    TrackInfo(const QString& path) : filePath(path) { cuePoints.fill(-1.0); }
    
    // Get display name (title if available, otherwise filename)
    QString getDisplayTitle() const {
        if (!title.isEmpty()) return title;
        QFileInfo info(filePath);
        return info.baseName();
    }
    
    // Get display artist (artist if available, otherwise "Unknown Artist")
    QString getDisplayArtist() const {
        return artist.isEmpty() ? "Unknown Artist" : artist;
    }
    
    // Get formatted duration string
    QString getDurationString() const {
        if (duration <= 0.0) return "--:--";
        int totalSeconds = static_cast<int>(duration);
        int minutes = totalSeconds / 60;
        int seconds = totalSeconds % 60;
        return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    }
    
    // Get BPM string
    QString getBpmString() const {
        return bpm > 0.0 ? QString::number(static_cast<int>(bpm)) : "--";
    }
    
    // Get file size string
    QString getFileSizeString() const {
        if (fileSize <= 0) return "--";
        double size = fileSize;
        QStringList units = {"B", "KB", "MB", "GB"};
        int unitIndex = 0;
        while (size >= 1024.0 && unitIndex < units.size() - 1) {
            size /= 1024.0;
            unitIndex++;
        }
        return QString("%1 %2").arg(QString::number(size, 'f', 1)).arg(units[unitIndex]);
    }

    bool hasBeatGrid() const {
        return analyzedAt > 0 && !beatPositions.isEmpty();
    }
};

// Background thread for loading ID3 tags
class ID3LoaderThread : public QThread {
    Q_OBJECT
    
public:
    explicit ID3LoaderThread(const QStringList& files, juce::AudioFormatManager* formatManager, QObject* parent = nullptr);
    
protected:
    void run() override;
    
signals:
    void trackLoaded(const TrackInfo& track);
    void progressUpdated(int current, int total);
    void finished();
    
private:
    QStringList filesToProcess;
    juce::AudioFormatManager* audioFormatManager;
    bool shouldStop = false;
    
    TrackInfo loadTrackInfo(const QString& filePath);
    
public slots:
    void stop() { shouldStop = true; }
};

// Custom table model for the library
class LibraryTableModel : public QAbstractTableModel {
    Q_OBJECT
    
public:
    enum Column {
        TitleColumn = 0,
        ArtistColumn,
        AlbumColumn,
        DurationColumn,
        BpmColumn,
        GenreColumn,
        YearColumn,
        FileSizeColumn,
        ColumnCount
    };
    
    enum SortMode {
        SortByTitle = 0,
        SortByArtist,
        SortByAlbum,
        SortByDuration,
        SortByBpm,
        SortByGenre,
        SortByYear,
        SortByFileSize
    };
    
    explicit LibraryTableModel(QObject* parent = nullptr);
    
    // QAbstractTableModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override; // Enable header-click sorting
    
    // Drag and drop support
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    
    // Custom methods
    void addOrUpdateTrack(const TrackInfo& track);
    void clearTracks();
    const TrackInfo* getTrack(int row) const;
    void setSortMode(SortMode mode, Qt::SortOrder order = Qt::AscendingOrder);
    void setFilterText(const QString& filter);
    void setPlaylistFilter(const QSet<QString>& allowedFiles);
    void clearPlaylistFilter();
    bool isPlaylistFilterActive() const { return playlistFilterActive; }
    int getPlaylistScopeCount() const { return playlistScopeCount; }
    std::optional<TrackInfo> findTrackByPath(const QString& filePath) const;
    
    // Get filtered tracks count
    int getFilteredCount() const { return filteredTracks.size(); }
    int getTotalCount() const { return allTracks.size(); }
    
private:
    std::vector<TrackInfo> allTracks;
    std::vector<const TrackInfo*> filteredTracks;
    SortMode currentSortMode = SortByTitle;
    Qt::SortOrder currentSortOrder = Qt::AscendingOrder;
    QString filterText;
    QSet<QString> playlistFilter;
    bool playlistFilterActive = false;
    int playlistScopeCount = 0;
    
    void updateFilteredTracks();
    void sortFilteredTracks();
    bool matchesFilter(const TrackInfo& track) const;
    bool isLessThan(const TrackInfo* a, const TrackInfo* b) const;
};

// Custom table view with drag support
class LibraryTableView : public QTableView {
    Q_OBJECT
    
public:
    explicit LibraryTableView(QWidget* parent = nullptr);
    
protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    
private:
    QPoint dragStartPosition;
    bool dragInProgress = false;
};

// Main library manager widget
class LibraryManager : public QWidget {
    Q_OBJECT
    
public:
    explicit LibraryManager(juce::AudioFormatManager* formatManager, QWidget* parent = nullptr);
    ~LibraryManager();
    
    // Add files to library
    void addFiles(const QStringList& files);
    void addDirectory(const QString& directory, bool recursive = true);
    
    // Get current selection
    QStringList getSelectedFiles() const;
    QString getCurrentFile() const;
    
    // Library management
    void clearLibrary();
    void saveLibrary(const QString& filePath);
    void loadLibrary(const QString& filePath);
    void applyAnalysisResult(const QString& filePath,
                             double bpm,
                             double firstBeatOffset,
                             double trackLengthSeconds,
                             const QVector<double>& beatPositions,
                             const QString& algorithm,
                             bool analysisFailed);
    void notifyAnalysisStarted(const QString& filePath);
    void notifyAnalysisProgress(const QString& filePath, double progress);
    void notifyAnalysisFinished(const QString& filePath, bool success);
    std::optional<std::array<double, 8>> getCuePointsForTrack(const QString& filePath) const;
    void saveCuePointsForTrack(const QString& filePath, const std::array<double, 8>& cuePoints);
    std::optional<TrackInfo> getTrackInfo(const QString& filePath) const;
    
signals:
    void fileSelected(const QString& filePath);
    void filesDropped(const QStringList& files);
    void loadToDeck(int deckIndex, const QString& filePath); // New signal for explicit deck loading
    void analyzeTracksRequested(const QStringList& filePaths);
    void analyzeTracksAdvancedRequested(const QStringList& filePaths, double minBpm, double maxBpm);
    
private slots:
    void onTrackLoaded(const TrackInfo& track);
    void onLoadingProgress(int current, int total);
    void onLoadingFinished();
    void onSortModeChanged();
    void onFilterTextChanged();
         void onAddFilesClicked(); // Replaced QPushButton with QAction
         void onAddFolderClicked(); // Replaced QPushButton with QAction
         void onRefreshClicked(); // Replaced QPushButton with QAction
         void onClearLibraryClicked(); // Replaced QPushButton with QAction
         void onContextMenuRequested(const QPoint& pos); // New slot for context menu
    void onTableDoubleClicked(const QModelIndex& index);
    void onSelectionChanged();
    void onFileSystemSelectionChanged();
    void onNavigationTabChanged(int index);
    void onCollectionSelectionChanged(int row);
    void onPlaylistSelectionChanged();
    void onPlaylistContextMenu(const QPoint& pos);
    void onAddPlaylistClicked();
    void onRenamePlaylistRequested();
    void onDeletePlaylistRequested();
    void onPlaylistItemDoubleClicked(QListWidgetItem* item);
    
private:
    // UI components
    QSplitter* mainSplitter;
    QTabWidget* navigationTabs;
    QListWidget* collectionList;
    QListWidget* playlistList;
    QTreeView* fileSystemTree;
    QFileSystemModel* fileSystemModel;
    LibraryTableView* tableView;
    LibraryTableModel* model;
    QComboBox* sortComboBox;
    QLineEdit* filterLineEdit;
        QAction* actionAddFiles; // Replaced QPushButton with QAction
        QAction* actionAddFolder; // Replaced QPushButton with QAction
        QAction* actionRefresh; // Replaced QPushButton with QAction
        QAction* actionClearLibrary; // Replaced QPushButton with QAction
        QAction* actionLoadDeck1 = nullptr; // Context-menu created; keep pointers if we later need enable/disable
        QAction* actionLoadDeck2 = nullptr;
    QAction* actionAnalyzeTrack = nullptr;
    QAction* actionAnalyzeAdvanced = nullptr;
    QLabel* statusLabel;
    QProgressBar* progressBar;
    QLabel* analysisStatusLabel = nullptr;
    QProgressBar* analysisProgressBar = nullptr;
    
    // Background loading
    ID3LoaderThread* loaderThread;
    juce::AudioFormatManager* audioFormatManager;
    
    // State
    bool isLoading = false;
    QTimer* filterUpdateTimer;
    bool columnsSizedOnce = false; // run auto-size only after first load

    enum class LibraryViewMode { Collection, Playlists, Explorer };
    LibraryViewMode currentViewMode = LibraryViewMode::Collection;
    QVector<PlaylistRecord> playlistRecords;
    QHash<int, QVector<PlaylistItemRecord>> playlistItemCache;
    int currentPlaylistId = -1;
    
    void initializeStoragePaths();
    void setupUI();
    void setupFileSystemModel();
    void initializeNavigationState();
    void updateStatusLabel();
    QStringList getSupportedAudioFiles(const QString& directory, bool recursive = true);
    void autoSizeColumnsInitial();
    void restoreColumnState();
    void saveColumnState();
    void loadExistingTracks();
    void loadPlaylists();
    void refreshPlaylistList();
    void ensurePlaylistSelection();
    void applyPlaylistFilter(int playlistId);
    void clearPlaylistFilter();
    void addTracksToPlaylist(int playlistId, const QStringList& filePaths);
    void removeTracksFromCurrentPlaylist(const QStringList& filePaths);
    QVector<PlaylistItemRecord> getPlaylistItems(int playlistId);
    int findPlaylistIndexById(int playlistId) const;
    QString playlistNameById(int playlistId) const;
    void updatePlaylistStatusUi();
    void persistTrack(const TrackInfo& track);
    void updateAnalysisUi();

    QString libraryDatabasePath;
    QString libraryXmlBackupPath;
    QString libraryUiStatePath;
    std::unique_ptr<LibraryDatabase> libraryDatabase;

    QHash<QString, double> activeAnalyses;
    int analysesCompleted = 0;
};
