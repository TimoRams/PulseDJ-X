#include "LibraryManager.h"
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QDirIterator>
#include <QStandardPaths>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QMouseEvent>
#include <QDrag>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSplitter>
#include <QTreeView>
#include <QFileSystemModel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QProgressBar>
#include <iostream>

// JUCE includes for audio format reading and ID3 tag extraction
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

// ID3LoaderThread Implementation
ID3LoaderThread::ID3LoaderThread(const QStringList& files, juce::AudioFormatManager* formatManager, QObject* parent)
    : QThread(parent), filesToProcess(files), audioFormatManager(formatManager)
{
}

void ID3LoaderThread::run()
{
    int current = 0;
    int total = filesToProcess.size();
    
    for (const QString& filePath : filesToProcess) {
        if (shouldStop) break;
        
        TrackInfo track = loadTrackInfo(filePath);
        emit trackLoaded(track);
        
        current++;
        emit progressUpdated(current, total);
        
        // Small delay to prevent UI blocking
        msleep(1);
    }
    
    emit finished();
}

TrackInfo ID3LoaderThread::loadTrackInfo(const QString& filePath)
{
    TrackInfo track(filePath);
    
    try {
        juce::File audioFile(filePath.toStdString());
        QFileInfo fileInfo(filePath);
        
        // Basic file info
        track.fileSize = fileInfo.size();
        
        if (!audioFile.exists()) {
            return track;
        }
        
        // Try to create a reader for the audio file
        std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager->createReaderFor(audioFile));
        
        if (reader) {
            // Get duration
            if (reader->sampleRate > 0) {
                track.duration = reader->lengthInSamples / reader->sampleRate;
            }
            
            // Try to get metadata from the reader
            juce::StringPairArray metadata = reader->metadataValues;
            
            // Extract ID3 tags
            track.title = QString::fromStdString(metadata.getValue("TITLE", "").toStdString());
            track.artist = QString::fromStdString(metadata.getValue("ARTIST", "").toStdString());
            track.album = QString::fromStdString(metadata.getValue("ALBUM", "").toStdString());
            track.genre = QString::fromStdString(metadata.getValue("GENRE", "").toStdString());
            track.year = QString::fromStdString(metadata.getValue("YEAR", "").toStdString());
            track.comment = QString::fromStdString(metadata.getValue("COMMENT", "").toStdString());
            
            // Try alternative tag names
            if (track.title.isEmpty()) {
                track.title = QString::fromStdString(metadata.getValue("TIT2", "").toStdString());
            }
            if (track.artist.isEmpty()) {
                track.artist = QString::fromStdString(metadata.getValue("TPE1", "").toStdString());
            }
            if (track.album.isEmpty()) {
                track.album = QString::fromStdString(metadata.getValue("TALB", "").toStdString());
            }
            if (track.genre.isEmpty()) {
                track.genre = QString::fromStdString(metadata.getValue("TCON", "").toStdString());
            }
            if (track.year.isEmpty()) {
                track.year = QString::fromStdString(metadata.getValue("TYER", "").toStdString());
                if (track.year.isEmpty()) {
                    track.year = QString::fromStdString(metadata.getValue("TDRC", "").toStdString());
                }
            }
            
            // Try to extract BPM
            QString bpmStr = QString::fromStdString(metadata.getValue("BPM", "").toStdString());
            if (bpmStr.isEmpty()) {
                bpmStr = QString::fromStdString(metadata.getValue("TBPM", "").toStdString());
            }
            if (!bpmStr.isEmpty()) {
                bool ok;
                double bpm = bpmStr.toDouble(&ok);
                if (ok && bpm > 0) {
                    track.bpm = bpm;
                }
            }
            
            // Try to extract key
            track.key = QString::fromStdString(metadata.getValue("KEY", "").toStdString());
            if (track.key.isEmpty()) {
                track.key = QString::fromStdString(metadata.getValue("TKEY", "").toStdString());
            }
        }
        
        // If we couldn't get title from metadata, use filename
        if (track.title.isEmpty()) {
            track.title = fileInfo.baseName();
        }
        
    } catch (const std::exception& e) {
        std::cout << "Error loading metadata for " << filePath.toStdString() << ": " << e.what() << std::endl;
        // Set basic info from filename if metadata loading fails
        QFileInfo fileInfo(filePath);
        track.title = fileInfo.baseName();
        track.fileSize = fileInfo.size();
    }
    
    return track;
}

// LibraryTableModel Implementation
LibraryTableModel::LibraryTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int LibraryTableModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return filteredTracks.size();
}

int LibraryTableModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return ColumnCount;
}

QVariant LibraryTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= filteredTracks.size()) {
        return QVariant();
    }
    
    const TrackInfo* track = filteredTracks[index.row()];
    if (!track) return QVariant();
    
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case TitleColumn: return track->getDisplayTitle();
            case ArtistColumn: return track->getDisplayArtist();
            case AlbumColumn: return track->album.isEmpty() ? "Unknown Album" : track->album;
            case DurationColumn: return track->getDurationString();
            case BpmColumn: return track->getBpmString();
            case GenreColumn: return track->genre.isEmpty() ? "Unknown" : track->genre;
            case YearColumn: return track->year.isEmpty() ? "--" : track->year;
            case FileSizeColumn: return track->getFileSizeString();
            default: return QVariant();
        }
    } else if (role == Qt::ToolTipRole) {
        return track->filePath;
    } else if (role == Qt::UserRole) {
        // Return the file path for drag operations
        return track->filePath;
    }
    
    return QVariant();
}

QVariant LibraryTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
            case TitleColumn: return tr("Title");
            case ArtistColumn: return tr("Artist");
            case AlbumColumn: return tr("Album");
            case DurationColumn: return tr("Duration");
            case BpmColumn: return tr("BPM");
            case GenreColumn: return tr("Genre");
            case YearColumn: return tr("Year");
            case FileSizeColumn: return tr("Size");
            default: return QVariant();
        }
    }
    return QVariant();
}

Qt::ItemFlags LibraryTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QStringList LibraryTableModel::mimeTypes() const
{
    return QStringList() << "text/uri-list";
}

QMimeData* LibraryTableModel::mimeData(const QModelIndexList& indexes) const
{
    QMimeData* mimeData = new QMimeData();
    QList<QUrl> urls;
    
    QSet<int> rows; // Avoid duplicates from multiple columns
    for (const QModelIndex& index : indexes) {
        if (index.isValid()) {
            rows.insert(index.row());
        }
    }
    
    for (int row : rows) {
        if (row < filteredTracks.size()) {
            const TrackInfo* track = filteredTracks[row];
            if (track) {
                urls.append(QUrl::fromLocalFile(track->filePath));
            }
        }
    }
    
    mimeData->setUrls(urls);
    return mimeData;
}

void LibraryTableModel::addTrack(const TrackInfo& track)
{
    allTracks.push_back(track);
    updateFilteredTracks();
}

void LibraryTableModel::clearTracks()
{
    beginResetModel();
    allTracks.clear();
    filteredTracks.clear();
    endResetModel();
}

const TrackInfo* LibraryTableModel::getTrack(int row) const
{
    if (row >= 0 && row < filteredTracks.size()) {
        return filteredTracks[row];
    }
    return nullptr;
}

void LibraryTableModel::setSortMode(SortMode mode, Qt::SortOrder order)
{
    currentSortMode = mode;
    currentSortOrder = order;
    sortFilteredTracks();
}

void LibraryTableModel::setFilterText(const QString& filter)
{
    filterText = filter.toLower();
    updateFilteredTracks();
}

void LibraryTableModel::updateFilteredTracks()
{
    beginResetModel();
    
    filteredTracks.clear();
    for (const auto& track : allTracks) {
        if (matchesFilter(track)) {
            filteredTracks.push_back(&track);
        }
    }
    
    sortFilteredTracks();
    endResetModel();
}

void LibraryTableModel::sortFilteredTracks()
{
    std::sort(filteredTracks.begin(), filteredTracks.end(), [this](const TrackInfo* a, const TrackInfo* b) {
        bool result = isLessThan(a, b);
        return currentSortOrder == Qt::AscendingOrder ? result : !result;
    });
}

bool LibraryTableModel::matchesFilter(const TrackInfo& track) const
{
    if (filterText.isEmpty()) return true;
    
    return track.getDisplayTitle().toLower().contains(filterText) ||
           track.getDisplayArtist().toLower().contains(filterText) ||
           track.album.toLower().contains(filterText) ||
           track.genre.toLower().contains(filterText);
}

bool LibraryTableModel::isLessThan(const TrackInfo* a, const TrackInfo* b) const
{
    if (!a || !b) return false;
    
    switch (currentSortMode) {
        case SortByTitle:
            return a->getDisplayTitle().toLower() < b->getDisplayTitle().toLower();
        case SortByArtist:
            return a->getDisplayArtist().toLower() < b->getDisplayArtist().toLower();
        case SortByAlbum:
            return a->album.toLower() < b->album.toLower();
        case SortByDuration:
            return a->duration < b->duration;
        case SortByBpm:
            return a->bpm < b->bpm;
        case SortByGenre:
            return a->genre.toLower() < b->genre.toLower();
        case SortByYear:
            return a->year < b->year;
        case SortByFileSize:
            return a->fileSize < b->fileSize;
        default:
            return false;
    }
}

// LibraryTableView Implementation
LibraryTableView::LibraryTableView(QWidget* parent)
    : QTableView(parent)
{
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    
    // Configure headers
    horizontalHeader()->setStretchLastSection(true);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    verticalHeader()->setVisible(false);
    
    // Set column widths
    setColumnWidth(LibraryTableModel::TitleColumn, 250);
    setColumnWidth(LibraryTableModel::ArtistColumn, 200);
    setColumnWidth(LibraryTableModel::AlbumColumn, 200);
    setColumnWidth(LibraryTableModel::DurationColumn, 80);
    setColumnWidth(LibraryTableModel::BpmColumn, 60);
    setColumnWidth(LibraryTableModel::GenreColumn, 100);
    setColumnWidth(LibraryTableModel::YearColumn, 60);
    setColumnWidth(LibraryTableModel::FileSizeColumn, 80);
}

void LibraryTableView::startDrag(Qt::DropActions supportedActions)
{
    QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) return;
    
    QMimeData* mimeData = model()->mimeData(indexes);
    if (!mimeData) return;
    
    QDrag* drag = new QDrag(this);
    drag->setMimeData(mimeData);
    
    // Create a simple drag pixmap
    QPixmap pixmap(100, 30);
    pixmap.fill(Qt::lightGray);
    drag->setPixmap(pixmap);
    
    drag->exec(supportedActions);
}

void LibraryTableView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragStartPosition = event->pos();
    }
    QTableView::mousePressEvent(event);
}

void LibraryTableView::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        QTableView::mouseMoveEvent(event);
        return;
    }
    
    if ((event->pos() - dragStartPosition).manhattanLength() < QApplication::startDragDistance()) {
        QTableView::mouseMoveEvent(event);
        return;
    }
    
    if (!dragInProgress) {
        dragInProgress = true;
        startDrag(Qt::CopyAction);
        dragInProgress = false;
    }
}

// LibraryManager Implementation
LibraryManager::LibraryManager(juce::AudioFormatManager* formatManager, QWidget* parent)
    : QWidget(parent), audioFormatManager(formatManager), loaderThread(nullptr)
{
    setupUI();
    setupFileSystemModel();
    
    // Setup filter update timer (debounce filtering)
    filterUpdateTimer = new QTimer(this);
    filterUpdateTimer->setSingleShot(true);
    filterUpdateTimer->setInterval(300); // 300ms delay
    connect(filterUpdateTimer, &QTimer::timeout, this, &LibraryManager::onFilterTextChanged);
}

LibraryManager::~LibraryManager()
{
    if (loaderThread && loaderThread->isRunning()) {
        loaderThread->stop();
        loaderThread->wait(3000);
        loaderThread->deleteLater();
    }
}

void LibraryManager::setupUI()
{
    setStyleSheet(
        "QWidget { background-color: #1a1a1a; color: #e0e0e0; }"
        "QTableView { gridline-color: #333; background-color: #0f0f0f; }"
        "QTableView::item:selected { background-color: #2d5aa0; }"
        "QTableView::item:alternate { background-color: #141414; }"
        "QHeaderView::section { background-color: #2a2a2a; border: 1px solid #555; padding: 4px; }"
        "QPushButton { background-color: #2a2a2a; border: 1px solid #555; padding: 5px; border-radius: 3px; }"
        "QPushButton:hover { background-color: #3a3a3a; }"
        "QPushButton:pressed { background-color: #1a1a1a; }"
        "QComboBox { background-color: #2a2a2a; border: 1px solid #555; padding: 3px; }"
        "QLineEdit { background-color: #2a2a2a; border: 1px solid #555; padding: 3px; }"
        "QProgressBar { background-color: #2a2a2a; border: 1px solid #555; }"
        "QProgressBar::chunk { background-color: #4a9eff; }"
        "QTreeView { background-color: #1a1a1a; border: 1px solid #555; }"
        "QTreeView::item:selected { background-color: #2d5aa0; }"
        "QTreeView::item:hover { background-color: #2a2a2a; }"
        "QTreeView::branch:has-children:!has-siblings:closed,"
        "QTreeView::branch:closed:has-children:has-siblings {"
        "    border-image: none;"
        "    image: url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAkAAAAJCAYAAADgkQYQAAAABHNCSVQICAgIfAhkiAAAAAlwSFlzAAAAdgAAAHYBTnsmCAAAABl0RVh0U29mdHdhcmUAd3d3Lmlua3NjYXBlLm9yZ5vuPBoAAAFHSURBVBiVY/j//z8DJQAggBhwgQACiAHbgCCAGLANCAKIAdeBIIAYsA0IAogB24AggBiwDQgCiAHXgSCAGLANCAKIAdeBIIAYsA0IAogB24AggBhwHQgCiAHbgCCAGHAdCAKIAduAIIAYcB0IAogB24AggBiwDQgCiAHXgSCAGLANCAKIAdeBIIAYsA0IAogB14EggBiwDQgCiAHbgCCAGHAdCAKIAduAIIAYcB0IAogB24AggBhwHQgCiAHbgCCAGHAdCAKIAduAIIAYsA0IAogB14EggBiwDQgCiAHbgCCAGHAdCAKIAduAIIAYcB0IAogB24AggBhwHQgCiAHbgCCAGLANCAKIAdeBIIAYsA0IAogB24AggBhwHQgCiAHbgCCAGHAdCAKIAduAIIAYsA0IAogB14EggBiwDf//AwQQAwMVAAgAAP//sEhb+gAAAABJRU5ErkJggg==);"
        "}"
        "QTreeView::branch:open:has-children:!has-siblings,"
        "QTreeView::branch:open:has-children:has-siblings {"
        "    border-image: none;"
        "    image: url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAkAAAAJCAYAAADgkQYQAAAABHNCSVQICAgIfAhkiAAAAAlwSFlzAAAAdgAAAHYBTnsmCAAAABl0RVh0U29mdHdhcmUAd3d3Lmlua3NjYXBlLm9yZ5vuPBoAAAFHSURBVBiVY/j//z8DJQAggBhwgQACiAHbgCCAGLANCAKIAdeBIIAYsA0IAogB24AggBiwDQgCiAHXgSCAGLANCAKIAdeBIIAYsA0IAogB24AggBhwHQgCiAHbgCCAGHAdCAKIAduAIIAYcB0IAogB24AggBiwDQgCiAHXgSCAGLANCAKIAdeBIIAYsA0IAogB14EggBiwDQgCiAHbgCCAGHAdCAKIAduAIIAYcB0IAogB24AggBhwHQgCiAHbgCCAGHAdCAKIAduAIIAYsA0IAogB14EggBiwDQgCiAHbgCCAGHAdCAKIAduAIIAYcB0IAogB24AggBhwHQgCiAHbgCCAGLANCAKIAdeBIIAYsA0IAogB24AggBhwHQgCiAHbgCCAGHAdCAKIAduAIIAYsA0IAogB14EggBiwDf//AwQQAwMVAAgAAP//sEhb+gAAAABJRU5ErkJggg==);"
        "}"
        "QSplitter::handle { background-color: #555; }"
        "QSplitter::handle:horizontal { width: 2px; }"
    );
    
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(5);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    
    // Create main splitter (horizontal)
    mainSplitter = new QSplitter(Qt::Horizontal, this);
    
    // === LEFT PANEL: File System Browser ===
    auto* leftPanel = new QWidget();
    leftPanel->setMinimumWidth(250);
    leftPanel->setMaximumWidth(400);
    
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    
    // File browser header
    auto* browserHeader = new QLabel("Music Folders", leftPanel);
    browserHeader->setStyleSheet("font-weight: bold; padding: 5px; background-color: #2a2a2a; border-bottom: 1px solid #555;");
    
    // File system tree view
    fileSystemTree = new QTreeView(leftPanel);
    fileSystemTree->setHeaderHidden(true);
    fileSystemTree->setRootIsDecorated(true);
    fileSystemTree->setDragEnabled(true);
    fileSystemTree->setDragDropMode(QAbstractItemView::DragOnly);
    
    leftLayout->addWidget(browserHeader);
    leftLayout->addWidget(fileSystemTree);
    
    // === RIGHT PANEL: Track Table ===
    auto* rightPanel = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    
    // Top controls for right panel
    auto* controlsLayout = new QHBoxLayout();
    
    // Sort controls
    auto* sortLabel = new QLabel("Sort by:", rightPanel);
    sortComboBox = new QComboBox(rightPanel);
    sortComboBox->addItem("Title", LibraryTableModel::SortByTitle);
    sortComboBox->addItem("Artist", LibraryTableModel::SortByArtist);
    sortComboBox->addItem("Album", LibraryTableModel::SortByAlbum);
    sortComboBox->addItem("Duration", LibraryTableModel::SortByDuration);
    sortComboBox->addItem("BPM", LibraryTableModel::SortByBpm);
    sortComboBox->addItem("Genre", LibraryTableModel::SortByGenre);
    sortComboBox->addItem("Year", LibraryTableModel::SortByYear);
    sortComboBox->addItem("File Size", LibraryTableModel::SortByFileSize);
    connect(sortComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LibraryManager::onSortModeChanged);
    
    // Filter controls
    auto* filterLabel = new QLabel("Filter:", rightPanel);
    filterLineEdit = new QLineEdit(rightPanel);
    filterLineEdit->setPlaceholderText("Search title, artist, album, genre...");
    connect(filterLineEdit, &QLineEdit::textChanged, [this]() {
        filterUpdateTimer->start(); // Restart timer on each keystroke
    });
    
    controlsLayout->addWidget(sortLabel);
    controlsLayout->addWidget(sortComboBox);
    controlsLayout->addStretch();
    controlsLayout->addWidget(filterLabel);
    controlsLayout->addWidget(filterLineEdit, 1);
    
    // Action buttons
    auto* buttonsLayout = new QHBoxLayout();
    
    addFilesButton = new QPushButton("Add Files...", rightPanel);
    addFolderButton = new QPushButton("Add Folder...", rightPanel);
    refreshButton = new QPushButton("Refresh", rightPanel);
    clearLibraryButton = new QPushButton("Clear Library", rightPanel);
    
    connect(addFilesButton, &QPushButton::clicked, this, &LibraryManager::onAddFilesClicked);
    connect(addFolderButton, &QPushButton::clicked, this, &LibraryManager::onAddFolderClicked);
    connect(refreshButton, &QPushButton::clicked, this, &LibraryManager::onRefreshClicked);
    connect(clearLibraryButton, &QPushButton::clicked, this, &LibraryManager::onClearLibraryClicked);
    
    buttonsLayout->addWidget(addFilesButton);
    buttonsLayout->addWidget(addFolderButton);
    buttonsLayout->addWidget(refreshButton);
    buttonsLayout->addStretch();
    buttonsLayout->addWidget(clearLibraryButton);
    
    // Table view
    model = new LibraryTableModel(rightPanel);
    tableView = new LibraryTableView(rightPanel);
    tableView->setModel(model);
    
    connect(tableView, &QTableView::doubleClicked, this, &LibraryManager::onTableDoubleClicked);
    connect(tableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LibraryManager::onSelectionChanged);
    
    // Status and progress for right panel
    auto* statusLayout = new QHBoxLayout();
    statusLabel = new QLabel("Ready", rightPanel);
    progressBar = new QProgressBar(rightPanel);
    progressBar->setVisible(false);
    
    statusLayout->addWidget(statusLabel, 1);
    statusLayout->addWidget(progressBar);
    
    // Assemble right panel
    rightLayout->addLayout(controlsLayout);
    rightLayout->addLayout(buttonsLayout);
    rightLayout->addWidget(tableView, 1);
    rightLayout->addLayout(statusLayout);
    
    // Add panels to splitter
    mainSplitter->addWidget(leftPanel);
    mainSplitter->addWidget(rightPanel);
    
    // Set splitter proportions (30% left, 70% right)
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({250, 600});
    
    // Add splitter to main layout
    mainLayout->addWidget(mainSplitter);
    
    updateStatusLabel();
}

void LibraryManager::setupFileSystemModel()
{
    // Create file system model for browsing
    fileSystemModel = new QFileSystemModel(this);
    fileSystemModel->setRootPath(QDir::rootPath());
    
    // Set name filters for audio files
    QStringList nameFilters;
    nameFilters << "*.mp3" << "*.wav" << "*.flac" << "*.aac" << "*.ogg" << "*.m4a";
    fileSystemModel->setNameFilters(nameFilters);
    fileSystemModel->setNameFilterDisables(false);
    
    // Set the model to the tree view
    fileSystemTree->setModel(fileSystemModel);
    
    // Hide size, type, and date columns - only show name
    fileSystemTree->hideColumn(1); // Size
    fileSystemTree->hideColumn(2); // Type
    fileSystemTree->hideColumn(3); // Date Modified
    
    // Set root to Music directory by default
    QString musicPath = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (QDir(musicPath).exists()) {
        QModelIndex musicIndex = fileSystemModel->index(musicPath);
        fileSystemTree->setRootIndex(musicIndex);
        fileSystemTree->expand(musicIndex);
    }
    
    // Connect file system tree selection
    connect(fileSystemTree->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &LibraryManager::onFileSystemSelectionChanged);
    
    // Enable drag from file system tree
    fileSystemTree->setDragEnabled(true);
    fileSystemTree->setDragDropMode(QAbstractItemView::DragOnly);
}

void LibraryManager::addFiles(const QStringList& files)
{
    if (files.isEmpty() || isLoading) return;
    
    // Filter for supported audio files
    QStringList audioFiles;
    QStringList supportedExtensions = {"mp3", "wav", "flac", "aac", "ogg", "m4a"};
    
    for (const QString& file : files) {
        QFileInfo info(file);
        if (info.exists() && info.isFile() && 
            supportedExtensions.contains(info.suffix().toLower())) {
            audioFiles.append(file);
        }
    }
    
    if (audioFiles.isEmpty()) {
        QMessageBox::information(this, "No Audio Files", "No supported audio files found.");
        return;
    }
    
    // Start background loading
    isLoading = true;
    progressBar->setVisible(true);
    progressBar->setRange(0, audioFiles.size());
    progressBar->setValue(0);
    
    loaderThread = new ID3LoaderThread(audioFiles, audioFormatManager, this);
    connect(loaderThread, &ID3LoaderThread::trackLoaded, this, &LibraryManager::onTrackLoaded);
    connect(loaderThread, &ID3LoaderThread::progressUpdated, this, &LibraryManager::onLoadingProgress);
    connect(loaderThread, &ID3LoaderThread::finished, this, &LibraryManager::onLoadingFinished);
    
    loaderThread->start();
    
    statusLabel->setText(QString("Loading %1 files...").arg(audioFiles.size()));
}

void LibraryManager::addDirectory(const QString& directory, bool recursive)
{
    if (directory.isEmpty() || isLoading) return;
    
    QStringList audioFiles = getSupportedAudioFiles(directory, recursive);
    addFiles(audioFiles);
}

QStringList LibraryManager::getSupportedAudioFiles(const QString& directory, bool recursive)
{
    QStringList files;
    QStringList nameFilters = {"*.mp3", "*.wav", "*.flac", "*.aac", "*.ogg", "*.m4a"};
    
    QDirIterator::IteratorFlag flags = recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator it(directory, nameFilters, QDir::Files, flags);
    
    while (it.hasNext()) {
        files.append(it.next());
    }
    
    return files;
}

QStringList LibraryManager::getSelectedFiles() const
{
    QStringList files;
    QModelIndexList indexes = tableView->selectionModel()->selectedRows();
    
    for (const QModelIndex& index : indexes) {
        const TrackInfo* track = model->getTrack(index.row());
        if (track) {
            files.append(track->filePath);
        }
    }
    
    return files;
}

QString LibraryManager::getCurrentFile() const
{
    QModelIndex current = tableView->currentIndex();
    if (current.isValid()) {
        const TrackInfo* track = model->getTrack(current.row());
        if (track) {
            return track->filePath;
        }
    }
    return QString();
}

void LibraryManager::clearLibrary()
{
    if (isLoading && loaderThread) {
        loaderThread->stop();
        loaderThread->wait(1000);
    }
    
    model->clearTracks();
    updateStatusLabel();
}

void LibraryManager::onTrackLoaded(const TrackInfo& track)
{
    model->addTrack(track);
    updateStatusLabel();
}

void LibraryManager::onLoadingProgress(int current, int total)
{
    progressBar->setValue(current);
    statusLabel->setText(QString("Loading files... %1/%2").arg(current).arg(total));
}

void LibraryManager::onLoadingFinished()
{
    isLoading = false;
    progressBar->setVisible(false);
    updateStatusLabel();
    
    if (loaderThread) {
        loaderThread->deleteLater();
        loaderThread = nullptr;
    }
}

void LibraryManager::onSortModeChanged()
{
    LibraryTableModel::SortMode mode = static_cast<LibraryTableModel::SortMode>(
        sortComboBox->currentData().toInt());
    model->setSortMode(mode);
}

void LibraryManager::onFilterTextChanged()
{
    model->setFilterText(filterLineEdit->text());
    updateStatusLabel();
}

void LibraryManager::onAddFilesClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        "Add Audio Files",
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
        "Audio Files (*.mp3 *.wav *.flac *.aac *.ogg *.m4a);;All Files (*)"
    );
    
    if (!files.isEmpty()) {
        addFiles(files);
    }
}

void LibraryManager::onAddFolderClicked()
{
    QString directory = QFileDialog::getExistingDirectory(
        this,
        "Add Audio Folder",
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation)
    );
    
    if (!directory.isEmpty()) {
        addDirectory(directory, true);
    }
}

void LibraryManager::onRefreshClicked()
{
    // Refresh the current folder view
    if (fileSystemModel) {
        QString currentPath = fileSystemModel->rootPath();
        fileSystemModel->setRootPath("");
        fileSystemModel->setRootPath(currentPath);
    }
}

void LibraryManager::onFileSystemSelectionChanged()
{
    QModelIndexList selected = fileSystemTree->selectionModel()->selectedIndexes();
    if (selected.isEmpty()) return;
    
    QModelIndex index = selected.first();
    QString path = fileSystemModel->filePath(index);
    QFileInfo info(path);
    
    if (info.isDir()) {
        // If it's a directory, load all audio files from it
        QStringList audioFiles = getSupportedAudioFiles(path, false); // Don't recurse
        if (!audioFiles.isEmpty()) {
            // Clear current library and load this folder
            model->clearTracks();
            addFiles(audioFiles);
        }
    } else if (info.isFile()) {
        // If it's a file, load just this file
        QStringList singleFile;
        singleFile << path;
        model->clearTracks();
        addFiles(singleFile);
    }
}

void LibraryManager::onClearLibraryClicked()
{
    int result = QMessageBox::question(
        this,
        "Clear Library",
        "Are you sure you want to clear the entire library?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    
    if (result == QMessageBox::Yes) {
        clearLibrary();
    }
}

void LibraryManager::onTableDoubleClicked(const QModelIndex& index)
{
    const TrackInfo* track = model->getTrack(index.row());
    if (track) {
        emit fileSelected(track->filePath);
    }
}

void LibraryManager::onSelectionChanged()
{
    // Optional: emit signal when selection changes
    QString currentFile = getCurrentFile();
    if (!currentFile.isEmpty()) {
        // Could emit a preview signal here
    }
}

void LibraryManager::updateStatusLabel()
{
    if (isLoading) return;
    
    int filtered = model->getFilteredCount();
    int total = model->getTotalCount();
    
    if (total == 0) {
        statusLabel->setText("Library is empty. Add some music files!");
    } else if (filtered == total) {
        statusLabel->setText(QString("%1 tracks").arg(total));
    } else {
        statusLabel->setText(QString("%1 of %2 tracks").arg(filtered).arg(total));
    }
}
