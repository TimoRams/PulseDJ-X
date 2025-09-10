#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QAbstractTableModel>
#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
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
#include <vector>
#include <memory>

// Forward declarations
namespace juce {
    class AudioFormatManager;
    class File;
}

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
    
    TrackInfo() = default;
    TrackInfo(const QString& path) : filePath(path) {}
    
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
    
    // Drag and drop support
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    
    // Custom methods
    void addTrack(const TrackInfo& track);
    void clearTracks();
    const TrackInfo* getTrack(int row) const;
    void setSortMode(SortMode mode, Qt::SortOrder order = Qt::AscendingOrder);
    void setFilterText(const QString& filter);
    
    // Get filtered tracks count
    int getFilteredCount() const { return filteredTracks.size(); }
    int getTotalCount() const { return allTracks.size(); }
    
private:
    std::vector<TrackInfo> allTracks;
    std::vector<const TrackInfo*> filteredTracks;
    SortMode currentSortMode = SortByTitle;
    Qt::SortOrder currentSortOrder = Qt::AscendingOrder;
    QString filterText;
    
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
    
signals:
    void fileSelected(const QString& filePath);
    void filesDropped(const QStringList& files);
    
private slots:
    void onTrackLoaded(const TrackInfo& track);
    void onLoadingProgress(int current, int total);
    void onLoadingFinished();
    void onSortModeChanged();
    void onFilterTextChanged();
    void onAddFilesClicked();
    void onAddFolderClicked();
    void onRefreshClicked();
    void onClearLibraryClicked();
    void onTableDoubleClicked(const QModelIndex& index);
    void onSelectionChanged();
    void onFileSystemSelectionChanged();
    
private:
    // UI components
    QSplitter* mainSplitter;
    QTreeView* fileSystemTree;
    QFileSystemModel* fileSystemModel;
    LibraryTableView* tableView;
    LibraryTableModel* model;
    QComboBox* sortComboBox;
    QLineEdit* filterLineEdit;
    QPushButton* addFilesButton;
    QPushButton* addFolderButton;
    QPushButton* refreshButton;
    QPushButton* clearLibraryButton;
    QLabel* statusLabel;
    QProgressBar* progressBar;
    
    // Background loading
    ID3LoaderThread* loaderThread;
    juce::AudioFormatManager* audioFormatManager;
    
    // State
    bool isLoading = false;
    QTimer* filterUpdateTimer;
    
    void setupUI();
    void setupFileSystemModel();
    void updateStatusLabel();
    QStringList getSupportedAudioFiles(const QString& directory, bool recursive = true);
};
