#include "LibraryManager.h"
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QDirIterator>
#include <QStandardPaths>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSplitter>
#include <QFrame>
#include <QTreeView>
#include <QFileSystemModel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton> // (legacy, can be removed if no longer used)
#include <QComboBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QToolButton>
#include <QMenu>
#include <QFile>
#include <QHash>
#include <QItemSelectionModel>
#include <QRegularExpression>
#include <QLocale>
#include <QInputDialog>
#include <QSignalBlocker>
#include <QTabBar>
#include <optional>
#include <cstring>
#include <cmath>
#include <iostream>
#include <QDebug>
#include <algorithm>
#include <QDateTime>
#include "AppConfig.h"
#include "LibraryDatabase.h"
#include "AudioFormatGuard.h"

// JUCE includes for audio format reading and ID3 tag extraction
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

// LibraryManager Implementation
LibraryManager::LibraryManager(juce::AudioFormatManager* formatManager, QWidget* parent)
    : QWidget(parent), audioFormatManager(formatManager), loaderThread(nullptr)
{
    initializeStoragePaths();

    libraryDatabase = std::make_unique<LibraryDatabase>(this);
    if (!libraryDatabase->open(libraryDatabasePath))
    {
        qWarning() << "Failed to open library database at" << libraryDatabasePath;
        libraryDatabase.reset();
    }

    setupUI();
    setupFileSystemModel();
    
    // Setup filter update timer (debounce filtering)
    filterUpdateTimer = new QTimer(this);
    filterUpdateTimer->setSingleShot(true);
    filterUpdateTimer->setInterval(300); // 300ms delay
    connect(filterUpdateTimer, &QTimer::timeout, this, &LibraryManager::onFilterTextChanged);

    loadExistingTracks();
    loadPlaylists();
}

LibraryManager::~LibraryManager()
{
    // Persist column sizes/order for next launch
    saveColumnState();
    if (loaderThread && loaderThread->isRunning()) {
        loaderThread->stop();
        loaderThread->wait(3000);
        loaderThread->deleteLater();
    }
}

void LibraryManager::setupUI()
{
    setStyleSheet(
        "QWidget { background-color: #101114; color: #f0f0f0; font-family: 'Lato', 'Arial', sans-serif; }"
        "QSplitter::handle { background-color: #23262e; }"
        "QSplitter::handle:horizontal { width: 2px; }"
        "#libraryNavPanel { background: #14161a; border-right: 1px solid #262931; }"
        "QTabWidget::pane { border: none; }"
    "QTabBar::tab { background: transparent; color: #a8acb3; padding: 7px 6px; margin: 1px; border-radius: 7px; min-width: 70px; font-size: 9px; font-weight: 600; }"
        "QTabBar::tab:selected { background: #2f6ae0; color: #ffffff; }"
    "QListWidget { background-color: transparent; border: none; font-size: 9px; }"
    "QListWidget::item { padding: 3px 6px; border-radius: 5px; margin: 1px 2px; }"
        "QListWidget::item:selected { background-color: #2f6ae0; color: #ffffff; }"
        "QListWidget::item:hover { background-color: rgba(47,106,224,0.16); }"
        "#libraryToolbar { background: #14171d; border: 1px solid #242832; border-radius: 8px; }"
        "#librarySearchContainer { background: #1a1d24; border: 1px solid #2b303c; border-radius: 6px; }"
    "#librarySearchContainer QLabel { color: #8b92a3; }"
        "#libraryFooterBar { background: #14171d; border: 1px solid #242832; border-radius: 8px; }"
    "QToolButton[class=\"primaryAction\"] { background: #1b1f27; border: 1px solid #2b303c; border-radius: 6px; padding: 3px 6px; font-size: 9px; min-width: 0px; min-height: 24px; color: #d8dce8; }"
        "QToolButton[class=\"primaryAction\"]:hover { border-color: #3c7cff; color: #ffffff; }"
        "QToolButton[class=\"primaryAction\"]:pressed { background: #161a21; }"
    "#filterCaption { color: #9aa3b5; font-size: 9px; font-weight: 600; padding-right: 2px; }"
        "#searchIcon { color: #7f8899; padding-right: 4px; }"
    "QLineEdit { background-color: #1f232c; border: 1px solid #323845; padding: 3px 6px; font-size: 9px; color: #e5e9f0; border-radius: 5px; }"
        "QLineEdit:focus { border-color: #4188ff; }"
    "#librarySearchContainer QLineEdit { background: transparent; border: none; padding: 0; font-size: 9px; }"
        "#librarySearchContainer QLineEdit:focus { border: none; }"
    "QComboBox { background-color: #1f232c; border: 1px solid #323845; padding: 2px 6px; font-size: 9px; color: #e5e9f0; border-radius: 5px; min-width: 96px; }"
        "QComboBox::drop-down { border: none; }"
    "QTableView { font-size: 10px; background: #0f1014; alternate-background-color: #15171d; selection-background-color: #2f6ae0; border: none; gridline-color: #1f232c; }"
    "QHeaderView::section { font-weight: 600; font-size: 9px; background: #181b22; color: #d8dce8; border: none; padding: 4px 3px; }"
        "QTableView::item { padding-left: 5px; padding-right: 5px; }"
        "QProgressBar { height: 9px; background: #1f232c; border: 1px solid #323845; border-radius: 4px; }"
        "QProgressBar::chunk { background: #4188ff; border-radius: 4px; }"
        "QTreeView { background-color: transparent; border: none; }"
        "QTreeView::item:selected { background-color: #2f6ae0; }"
        "QTreeView::item:hover { background-color: rgba(47,106,224,0.16); }"
    );

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(5);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    
    // Create main splitter (horizontal)
    mainSplitter = new QSplitter(Qt::Horizontal, this);
    
    // === LEFT PANEL: Navigation Tabs ===
    auto* leftPanel = new QWidget();
    leftPanel->setObjectName("libraryNavPanel");
    leftPanel->setMinimumWidth(120);
    leftPanel->setMaximumWidth(180);

    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(2, 4, 2, 4);
    leftLayout->setSpacing(5);

    navigationTabs = new QTabWidget(leftPanel);
    navigationTabs->setTabPosition(QTabWidget::West);
    navigationTabs->setDocumentMode(true);
    navigationTabs->setMovable(false);
    navigationTabs->setIconSize(QSize(16, 16));
    if (auto* bar = navigationTabs->tabBar())
    {
        bar->setIconSize(QSize(16, 16));
        bar->setFocusPolicy(Qt::NoFocus);
        bar->setExpanding(false);
        bar->setMinimumWidth(64);
    }

    // Collection tab
    auto* collectionPage = new QWidget(navigationTabs);
    auto* collectionLayout = new QVBoxLayout(collectionPage);
    collectionLayout->setContentsMargins(0, 0, 0, 0);
    collectionLayout->setSpacing(4);
    auto* collectionHeader = new QLabel(tr("Collection"), collectionPage);
    collectionHeader->setObjectName("collectionHeader");
    collectionHeader->setStyleSheet("font-weight: 600; font-size: 10px; padding: 4px 8px; color: #f0f0f0;");
    collectionList = new QListWidget(collectionPage);
    collectionList->setSelectionMode(QAbstractItemView::SingleSelection);
    collectionList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    collectionList->setSpacing(2);
    collectionList->addItem(tr("All Tracks"));
    collectionLayout->addWidget(collectionHeader);
    collectionLayout->addWidget(collectionList, 1);
    navigationTabs->addTab(collectionPage, QIcon::fromTheme("media-playlist-shuffle"), tr("Collection"));

    // Playlists tab
    auto* playlistPage = new QWidget(navigationTabs);
    auto* playlistLayout = new QVBoxLayout(playlistPage);
    playlistLayout->setContentsMargins(0, 0, 0, 0);
    playlistLayout->setSpacing(4);

    auto* playlistHeader = new QWidget(playlistPage);
    auto* playlistHeaderLayout = new QHBoxLayout(playlistHeader);
    playlistHeaderLayout->setContentsMargins(8, 4, 8, 4);
    playlistHeaderLayout->setSpacing(4);
    auto* playlistLabel = new QLabel(tr("Playlists"), playlistHeader);
    playlistLabel->setStyleSheet("font-weight: 600; font-size: 9px; color: #f0f0f0;");
    auto* addPlaylistButton = new QToolButton(playlistHeader);
    addPlaylistButton->setIcon(QIcon::fromTheme("list-add"));
    addPlaylistButton->setIconSize(QSize(12, 12));
    addPlaylistButton->setAutoRaise(true);
    addPlaylistButton->setCursor(Qt::PointingHandCursor);
    addPlaylistButton->setToolTip(tr("Create new playlist"));
    playlistHeaderLayout->addWidget(playlistLabel, 1);
    playlistHeaderLayout->addWidget(addPlaylistButton, 0, Qt::AlignRight);

    playlistList = new QListWidget(playlistPage);
    playlistList->setSelectionMode(QAbstractItemView::SingleSelection);
    playlistList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    playlistList->setSpacing(2);
    playlistList->setContextMenuPolicy(Qt::CustomContextMenu);

    playlistLayout->addWidget(playlistHeader);
    playlistLayout->addWidget(playlistList, 1);

    navigationTabs->addTab(playlistPage, QIcon::fromTheme("view-media-playlist"), tr("Playlists"));

    // Explorer tab
    auto* explorerPage = new QWidget(navigationTabs);
    auto* explorerLayout = new QVBoxLayout(explorerPage);
    explorerLayout->setContentsMargins(0, 0, 0, 0);
    explorerLayout->setSpacing(4);
    auto* explorerHeader = new QLabel(tr("Folders"), explorerPage);
    explorerHeader->setStyleSheet("font-weight: 600; font-size: 10px; padding: 5px 9px; color: #f0f0f0;");
    fileSystemTree = new QTreeView(explorerPage);
    fileSystemTree->setHeaderHidden(true);
    fileSystemTree->setRootIsDecorated(true);
    fileSystemTree->setDragEnabled(true);
    fileSystemTree->setDragDropMode(QAbstractItemView::DragOnly);
    explorerLayout->addWidget(explorerHeader);
    explorerLayout->addWidget(fileSystemTree, 1);
    navigationTabs->addTab(explorerPage, QIcon::fromTheme("folder"), tr("Explorer"));

    leftLayout->addWidget(navigationTabs);
    
    // === RIGHT PANEL: Track Table ===
    auto* rightPanel = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    // Sort combo is now exposed in filter row
    sortComboBox = new QComboBox(rightPanel);
    sortComboBox->addItem("Title", LibraryTableModel::SortByTitle);
    sortComboBox->addItem("Artist", LibraryTableModel::SortByArtist);
    sortComboBox->addItem("Album", LibraryTableModel::SortByAlbum);
    sortComboBox->addItem("Duration", LibraryTableModel::SortByDuration);
    sortComboBox->addItem("BPM", LibraryTableModel::SortByBpm);
    sortComboBox->addItem("Genre", LibraryTableModel::SortByGenre);
    sortComboBox->addItem("Year", LibraryTableModel::SortByYear);
    sortComboBox->addItem("File Size", LibraryTableModel::SortByFileSize);
    sortComboBox->setMinimumWidth(108);
    sortComboBox->setFixedHeight(22);
    connect(sortComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LibraryManager::onSortModeChanged);

    // Actions
    actionAddFiles = new QAction(QIcon::fromTheme("list-add"), "Add Files", this);
    actionAddFolder = new QAction(QIcon::fromTheme("folder-open"), "Add Folder", this);
    actionRefresh = new QAction(QIcon::fromTheme("view-refresh"), "Refresh", this);
    actionClearLibrary = new QAction(QIcon::fromTheme("edit-delete"), "Clear Library", this);
    actionAnalyzeTrack = new QAction(QIcon::fromTheme("view-statistics"), tr("Analyze Track"), this);
    actionAnalyzeTrack->setEnabled(false);
    actionAnalyzeAdvanced = new QAction(QIcon::fromTheme("tools-wizard"), tr("Analyze (adv)"), this);
    actionAnalyzeAdvanced->setEnabled(false);
    connect(actionAddFiles, &QAction::triggered, this, &LibraryManager::onAddFilesClicked);
    connect(actionAddFolder, &QAction::triggered, this, &LibraryManager::onAddFolderClicked);
    connect(actionRefresh, &QAction::triggered, this, &LibraryManager::onRefreshClicked);
    connect(actionClearLibrary, &QAction::triggered, this, &LibraryManager::onClearLibraryClicked);
    connect(actionAnalyzeTrack, &QAction::triggered, this, [this]() {
        const QStringList selected = getSelectedFiles();
        if (!selected.isEmpty())
            emit analyzeTracksRequested(selected);
    });
    connect(actionAnalyzeAdvanced, &QAction::triggered, this, [this]() {
        const QStringList selected = getSelectedFiles();
        if (selected.isEmpty())
            return;

        struct Preset { QString label; double minBpm; double maxBpm; };
        const QVector<Preset> presets = {
            {tr("78–155 BPM"), 78.0, 155.0},
            {tr("70–140 BPM"), 70.0, 140.0},
            {tr("90–180 BPM"), 90.0, 180.0},
            {tr("100–200 BPM"), 100.0, 200.0},
            {tr("120–240 BPM"), 120.0, 240.0}
        };

        QDialog dialog(this);
        dialog.setWindowTitle(tr("Advanced Analysis"));
        dialog.setModal(true);

        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(12);

        auto* label = new QLabel(tr("Select a BPM range preset:"), &dialog);
        label->setWordWrap(true);
        layout->addWidget(label);

        auto* presetCombo = new QComboBox(&dialog);
        for (const auto& preset : presets) {
            QVariantList range;
            range << preset.minBpm << preset.maxBpm;
            presetCombo->addItem(preset.label, range);
        }
        presetCombo->setCurrentIndex(0);
        layout->addWidget(presetCombo);

        auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttonBox);

        connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted)
            return;

        const QVariant data = presetCombo->currentData();
        double minBpm = 78.0;
        double maxBpm = 155.0;
        if (data.canConvert<QVariantList>()) {
            const auto list = data.toList();
            if (list.size() == 2) {
                minBpm = list.at(0).toDouble();
                maxBpm = list.at(1).toDouble();
            }
        }

        emit analyzeTracksAdvancedRequested(selected, minBpm, maxBpm);
    });

    // Compact toolbar with search, sort and actions
    auto* toolbar = new QFrame(rightPanel);
    toolbar->setObjectName("libraryToolbar");
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(10, 6, 10, 6);
    toolbarLayout->setSpacing(6);

    auto makeActionButton = [toolbar](QAction* action, const QString& tooltip) {
        auto* button = new QToolButton(toolbar);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(12, 12));
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoRaise(false);
        button->setProperty("class", "primaryAction");
        button->setMinimumHeight(24);
        button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        if (!tooltip.isEmpty())
            button->setToolTip(tooltip);
        return button;
    };

    auto* searchContainer = new QFrame(toolbar);
    searchContainer->setObjectName("librarySearchContainer");
    auto* searchLayout = new QHBoxLayout(searchContainer);
    searchLayout->setContentsMargins(8, 0, 8, 0);
    searchLayout->setSpacing(4);
    auto* searchIcon = new QLabel(searchContainer);
    searchIcon->setObjectName("searchIcon");
    searchIcon->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    const auto searchPixmap = QIcon::fromTheme("edit-find").pixmap(11, 11);
    if (!searchPixmap.isNull())
    {
        searchIcon->setPixmap(searchPixmap);
    }
    else
    {
        searchIcon->setText(QString::fromUtf8("\xE2\x8C\x95"));
    }

    filterLineEdit = new QLineEdit(searchContainer);
    filterLineEdit->setPlaceholderText(tr("Search library"));
    filterLineEdit->setClearButtonEnabled(true);
    filterLineEdit->setFixedHeight(22);
    filterLineEdit->setMinimumWidth(180);
    connect(filterLineEdit, &QLineEdit::textChanged, [this]() { filterUpdateTimer->start(); });

    searchLayout->addWidget(searchIcon, 0, Qt::AlignVCenter);
    searchLayout->addWidget(filterLineEdit, 1);
    searchContainer->setLayout(searchLayout);

    auto* sortContainer = new QWidget(toolbar);
    auto* sortLayout = new QHBoxLayout(sortContainer);
    sortLayout->setContentsMargins(0, 0, 0, 0);
    sortLayout->setSpacing(4);
    auto* sortLabel = new QLabel(tr("Sort"), sortContainer);
    sortLabel->setObjectName("filterCaption");
    sortLayout->addWidget(sortLabel, 0, Qt::AlignVCenter);
    sortLayout->addWidget(sortComboBox, 0, Qt::AlignVCenter);
    sortContainer->setLayout(sortLayout);

    auto* actionContainer = new QWidget(toolbar);
    auto* actionLayout = new QHBoxLayout(actionContainer);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(4);
    auto* addFilesBtn = makeActionButton(actionAddFiles, tr("Import individual tracks"));
    auto* addFolderBtn = makeActionButton(actionAddFolder, tr("Scan an entire folder"));
    auto* refreshBtn = makeActionButton(actionRefresh, tr("Reload metadata and playlists"));
    auto* clearBtn = makeActionButton(actionClearLibrary, tr("Remove everything from the library"));
    auto* analyzeColumn = new QWidget(actionContainer);
    auto* analyzeLayout = new QVBoxLayout(analyzeColumn);
    analyzeLayout->setContentsMargins(0, 0, 0, 0);
    analyzeLayout->setSpacing(2);
    auto* analyzeBtn = makeActionButton(actionAnalyzeTrack, tr("Analyze BPM and beatgrid for selected tracks"));
    auto* analyzeAdvBtn = makeActionButton(actionAnalyzeAdvanced, tr("Analyze with BPM range presets"));
    analyzeAdvBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    analyzeLayout->addWidget(analyzeBtn);
    analyzeLayout->addWidget(analyzeAdvBtn);
    analyzeColumn->setLayout(analyzeLayout);

    actionLayout->addWidget(addFilesBtn);
    actionLayout->addWidget(addFolderBtn);
    actionLayout->addWidget(analyzeColumn);
    actionLayout->addWidget(refreshBtn);
    actionLayout->addWidget(clearBtn);
    actionContainer->setLayout(actionLayout);

    toolbarLayout->addWidget(searchContainer, 1);
    toolbarLayout->addWidget(sortContainer, 0);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(actionContainer, 0, Qt::AlignRight);

    rightLayout->addWidget(toolbar);
    
    // Table view
    model = new LibraryTableModel(rightPanel);
    tableView = new LibraryTableView(rightPanel);
    tableView->setModel(model);
    tableView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tableView, &QWidget::customContextMenuRequested, this, &LibraryManager::onContextMenuRequested);
    // Restore previously saved column sizes/order if available
    restoreColumnState();
    
    connect(tableView, &QTableView::doubleClicked, this, &LibraryManager::onTableDoubleClicked);
    connect(tableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LibraryManager::onSelectionChanged);
    
    // Status and progress for right panel
    analysisStatusLabel = new QLabel(tr("Analysis idle"), rightPanel);
    analysisStatusLabel->setStyleSheet("QLabel { color: #a0a0a0; }");
    analysisStatusLabel->setVisible(true);
    analysisProgressBar = new QProgressBar(rightPanel);
    analysisProgressBar->setRange(0, 100);
    analysisProgressBar->setValue(0);
    analysisProgressBar->setFormat(QStringLiteral("%p%"));
    analysisProgressBar->setTextVisible(true);
    analysisProgressBar->setEnabled(false);
    analysisProgressBar->setVisible(true);
    analysisProgressBar->setFixedHeight(12);

    auto* footerBar = new QFrame(rightPanel);
    footerBar->setObjectName("libraryFooterBar");
    auto* footerLayout = new QHBoxLayout(footerBar);
    footerLayout->setContentsMargins(8, 4, 8, 4);
    footerLayout->setSpacing(6);
    statusLabel = new QLabel("Ready", footerBar);
    progressBar = new QProgressBar(footerBar);
    progressBar->setVisible(false);
    footerLayout->addWidget(statusLabel, 1);
    footerLayout->addWidget(progressBar, 0);
    footerLayout->addSpacing(6);
    footerLayout->addWidget(analysisStatusLabel, 0);
    footerLayout->addWidget(analysisProgressBar, 0);

    // Assemble right panel
    rightLayout->addWidget(tableView, 1);
    rightLayout->addWidget(footerBar);
    
    // Add panels to splitter
    mainSplitter->addWidget(leftPanel);
    mainSplitter->addWidget(rightPanel);
    
    // Set splitter proportions (30% left, 70% right)
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({250, 600});
    
    // Add splitter to main layout
    mainLayout->addWidget(mainSplitter);
    
    connect(navigationTabs, &QTabWidget::currentChanged, this, &LibraryManager::onNavigationTabChanged);
    connect(collectionList, &QListWidget::currentRowChanged, this, &LibraryManager::onCollectionSelectionChanged);
    connect(playlistList, &QListWidget::itemSelectionChanged, this, &LibraryManager::onPlaylistSelectionChanged);
    connect(playlistList, &QWidget::customContextMenuRequested, this, &LibraryManager::onPlaylistContextMenu);
    connect(playlistList, &QListWidget::itemDoubleClicked, this, &LibraryManager::onPlaylistItemDoubleClicked);
    connect(addPlaylistButton, &QToolButton::clicked, this, &LibraryManager::onAddPlaylistClicked);

    initializeNavigationState();
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

void LibraryManager::initializeNavigationState()
{
    if (navigationTabs)
        navigationTabs->setCurrentIndex(0);

    if (collectionList && collectionList->count() > 0)
        collectionList->setCurrentRow(0);

    currentViewMode = LibraryViewMode::Collection;
}

void LibraryManager::onNavigationTabChanged(int index)
{
    if (!model)
        return;

    switch (index)
    {
    case 0:
        currentViewMode = LibraryViewMode::Collection;
        clearPlaylistFilter();
        updateStatusLabel();
        break;
    case 1:
        currentViewMode = LibraryViewMode::Playlists;
        ensurePlaylistSelection();
        if (currentPlaylistId > 0)
            applyPlaylistFilter(currentPlaylistId);
        else
            updateStatusLabel();
        break;
    case 2:
    default:
        currentViewMode = LibraryViewMode::Explorer;
        clearPlaylistFilter();
        updateStatusLabel();
        break;
    }
}

void LibraryManager::onCollectionSelectionChanged(int row)
{
    Q_UNUSED(row);
    if (currentViewMode != LibraryViewMode::Collection)
        return;

    clearPlaylistFilter();
    updateStatusLabel();
}

void LibraryManager::onPlaylistSelectionChanged()
{
    if (!playlistList)
        return;

    QListWidgetItem* item = playlistList->currentItem();
    if (!item)
    {
        if (currentViewMode == LibraryViewMode::Playlists)
        {
            model->clearPlaylistFilter();
            updateStatusLabel();
        }
        return;
    }

    const int playlistId = item->data(Qt::UserRole).toInt();
    currentPlaylistId = playlistId;

    if (currentViewMode == LibraryViewMode::Playlists)
        applyPlaylistFilter(playlistId);
}

void LibraryManager::onPlaylistContextMenu(const QPoint& pos)
{
    if (!playlistList)
        return;

    QListWidgetItem* item = playlistList->itemAt(pos);
    if (item && playlistList->currentItem() != item)
        playlistList->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);

    QMenu menu(this);
    menu.addAction(tr("New Playlist"), this, &LibraryManager::onAddPlaylistClicked);

    if (playlistList->currentItem())
    {
        menu.addAction(tr("Rename"), this, &LibraryManager::onRenamePlaylistRequested);
        menu.addAction(tr("Delete"), this, &LibraryManager::onDeletePlaylistRequested);
    }

    menu.exec(playlistList->mapToGlobal(pos));
}

void LibraryManager::onAddPlaylistClicked()
{
    if (!libraryDatabase)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Create Playlist"), tr("Playlist name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    PlaylistRecord record;
    if (!libraryDatabase->createPlaylist(name, &record))
    {
        QMessageBox::warning(this, tr("Playlist"), tr("Could not create playlist."));
        return;
    }

    playlistRecords.append(record);
    playlistItemCache.remove(record.id);
    currentPlaylistId = record.id;
    refreshPlaylistList();
    ensurePlaylistSelection();

    if (currentViewMode == LibraryViewMode::Playlists)
        applyPlaylistFilter(record.id);
}

void LibraryManager::onRenamePlaylistRequested()
{
    if (!libraryDatabase || !playlistList)
        return;

    QListWidgetItem* item = playlistList->currentItem();
    if (!item)
        return;

    const int playlistId = item->data(Qt::UserRole).toInt();
    const QString currentName = playlistNameById(playlistId);

    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename Playlist"), tr("New name:"), QLineEdit::Normal, currentName, &ok).trimmed();
    if (!ok || newName.isEmpty() || newName == currentName)
        return;

    if (!libraryDatabase->renamePlaylist(playlistId, newName))
    {
        QMessageBox::warning(this, tr("Playlist"), tr("Could not rename playlist."));
        return;
    }

    const int index = findPlaylistIndexById(playlistId);
    if (index >= 0)
        playlistRecords[index].name = newName;

    refreshPlaylistList();
    ensurePlaylistSelection();
    updatePlaylistStatusUi();
}

void LibraryManager::onDeletePlaylistRequested()
{
    if (!libraryDatabase || !playlistList)
        return;

    QListWidgetItem* item = playlistList->currentItem();
    if (!item)
        return;

    const int playlistId = item->data(Qt::UserRole).toInt();
    const QString name = playlistNameById(playlistId);

    const auto answer = QMessageBox::question(this,
                                              tr("Delete Playlist"),
                                              tr("Delete playlist \"%1\"?" ).arg(name));
    if (answer != QMessageBox::Yes)
        return;

    if (!libraryDatabase->deletePlaylist(playlistId))
    {
        QMessageBox::warning(this, tr("Playlist"), tr("Could not delete playlist."));
        return;
    }

    playlistItemCache.remove(playlistId);
    const int index = findPlaylistIndexById(playlistId);
    if (index >= 0)
        playlistRecords.removeAt(index);

    if (currentPlaylistId == playlistId)
        model->clearPlaylistFilter();

    currentPlaylistId = -1;
    refreshPlaylistList();
    ensurePlaylistSelection();
    updateStatusLabel();
}

void LibraryManager::onPlaylistItemDoubleClicked(QListWidgetItem* item)
{
    if (!item)
        return;

    playlistList->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
    onRenamePlaylistRequested();
}

void LibraryManager::loadPlaylists()
{
    playlistRecords.clear();
    playlistItemCache.clear();

    if (!libraryDatabase)
    {
        refreshPlaylistList();
        return;
    }

    playlistRecords = libraryDatabase->loadAllPlaylists();
    refreshPlaylistList();
    ensurePlaylistSelection();
}

void LibraryManager::refreshPlaylistList()
{
    if (!playlistList)
        return;

    const int previousId = currentPlaylistId;
    QSignalBlocker blocker(playlistList);
    playlistList->clear();

    for (const auto& record : playlistRecords)
    {
        auto* item = new QListWidgetItem(QStringLiteral("%1 (%2)").arg(record.name).arg(record.trackCount), playlistList);
        item->setData(Qt::UserRole, record.id);
        item->setToolTip(tr("%1 tracks").arg(record.trackCount));
    }

    currentPlaylistId = previousId;
}

void LibraryManager::ensurePlaylistSelection()
{
    if (!playlistList)
        return;

    if (playlistRecords.isEmpty())
    {
        playlistList->clearSelection();
        updateStatusLabel();
        return;
    }

    int index = findPlaylistIndexById(currentPlaylistId);
    if (index < 0)
        index = 0;

    if (index >= 0 && index < playlistList->count())
        playlistList->setCurrentRow(index);

    currentPlaylistId = playlistList->currentItem() ? playlistList->currentItem()->data(Qt::UserRole).toInt() : -1;

    if (currentViewMode == LibraryViewMode::Playlists && currentPlaylistId > 0)
        applyPlaylistFilter(currentPlaylistId);
}

void LibraryManager::applyPlaylistFilter(int playlistId)
{
    if (!model)
        return;

    if (playlistId <= 0)
    {
        model->clearPlaylistFilter();
        updateStatusLabel();
        return;
    }

    const QVector<PlaylistItemRecord> items = getPlaylistItems(playlistId);
    QSet<QString> allowed;
    allowed.reserve(items.size());
    for (const auto& item : items)
        allowed.insert(item.filePath);

    model->setPlaylistFilter(allowed);
    updatePlaylistStatusUi();
}

void LibraryManager::clearPlaylistFilter()
{
    if (model && model->isPlaylistFilterActive())
        model->clearPlaylistFilter();
}

void LibraryManager::addTracksToPlaylist(int playlistId, const QStringList& filePaths)
{
    if (!libraryDatabase || playlistId <= 0 || filePaths.isEmpty())
        return;

    bool changed = false;
    for (const QString& path : filePaths)
    {
        PlaylistItemRecord newItem;
        if (libraryDatabase->addTrackToPlaylist(playlistId, path, -1, &newItem))
            changed = true;
    }

    if (!changed)
        return;

    QVector<PlaylistItemRecord> refreshedItems;
    if (libraryDatabase)
        refreshedItems = libraryDatabase->loadPlaylistItems(playlistId);

    playlistItemCache.insert(playlistId, refreshedItems);

    const int index = findPlaylistIndexById(playlistId);
    if (index >= 0)
        playlistRecords[index].trackCount = refreshedItems.size();

    refreshPlaylistList();
    ensurePlaylistSelection();

    if (currentViewMode == LibraryViewMode::Playlists && currentPlaylistId == playlistId)
        applyPlaylistFilter(playlistId);
    else
        updateStatusLabel();
}

void LibraryManager::removeTracksFromCurrentPlaylist(const QStringList& filePaths)
{
    if (!libraryDatabase || currentPlaylistId <= 0 || filePaths.isEmpty())
        return;

    QVector<PlaylistItemRecord> items = getPlaylistItems(currentPlaylistId);
    bool changed = false;

    for (const QString& path : filePaths)
    {
        auto it = std::find_if(items.begin(), items.end(), [&path](const PlaylistItemRecord& record) {
            return record.filePath.compare(path, Qt::CaseInsensitive) == 0;
        });

        if (it == items.end())
            continue;

        if (libraryDatabase->removePlaylistItem(it->id))
        {
            changed = true;
            items.erase(it);
        }
    }

    if (!changed)
        return;

    QVector<PlaylistItemRecord> refreshedItems;
    if (libraryDatabase)
        refreshedItems = libraryDatabase->loadPlaylistItems(currentPlaylistId);

    playlistItemCache.insert(currentPlaylistId, refreshedItems);

    const int index = findPlaylistIndexById(currentPlaylistId);
    if (index >= 0)
        playlistRecords[index].trackCount = refreshedItems.size();

    refreshPlaylistList();
    ensurePlaylistSelection();

    if (currentViewMode == LibraryViewMode::Playlists)
        applyPlaylistFilter(currentPlaylistId);
    else
        updateStatusLabel();
}

QVector<PlaylistItemRecord> LibraryManager::getPlaylistItems(int playlistId)
{
    if (playlistItemCache.contains(playlistId))
        return playlistItemCache.value(playlistId);

    QVector<PlaylistItemRecord> items;
    if (libraryDatabase)
        items = libraryDatabase->loadPlaylistItems(playlistId);

    playlistItemCache.insert(playlistId, items);
    return items;
}

int LibraryManager::findPlaylistIndexById(int playlistId) const
{
    for (int i = 0; i < playlistRecords.size(); ++i)
    {
        if (playlistRecords.at(i).id == playlistId)
            return i;
    }
    return -1;
}

QString LibraryManager::playlistNameById(int playlistId) const
{
    const int index = findPlaylistIndexById(playlistId);
    if (index >= 0)
        return playlistRecords.at(index).name;
    return QString();
}

void LibraryManager::updatePlaylistStatusUi()
{
    if (!statusLabel || !model || currentPlaylistId <= 0 || !model->isPlaylistFilterActive())
        return;

    const QString name = playlistNameById(currentPlaylistId);
    int totalTracks = 0;
    const int index = findPlaylistIndexById(currentPlaylistId);
    if (index >= 0)
        totalTracks = playlistRecords.at(index).trackCount;
    else
        totalTracks = getPlaylistItems(currentPlaylistId).size();

    const int filtered = model->getFilteredCount();
    if (filtered == totalTracks)
        statusLabel->setText(tr("Playlist \"%1\" · %2 tracks").arg(name).arg(totalTracks));
    else
        statusLabel->setText(tr("Playlist \"%1\": %2 of %3 tracks matched").arg(name).arg(filtered).arg(totalTracks));
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
    if (libraryDatabase)
        libraryDatabase->removeAllTracks();
    updateStatusLabel();
}

void LibraryManager::onTrackLoaded(const TrackInfo& track)
{
    TrackInfo merged = track;

    auto mergePersistedFields = [&merged](const TrackInfo& persisted) {
        if (persisted.duration > 0.0 && merged.duration <= 0.0)
            merged.duration = persisted.duration;
        if (persisted.fileSize > 0 && merged.fileSize <= 0)
            merged.fileSize = persisted.fileSize;
        if (persisted.lastModified > 0)
            merged.lastModified = persisted.lastModified;
        if (persisted.addedAt > 0)
            merged.addedAt = persisted.addedAt;

        merged.analysisFailed = persisted.analysisFailed;
        bool hasAnalysis = persisted.analyzedAt > 0 || persisted.bpm > 0.0 || !persisted.beatPositions.isEmpty();
        if (hasAnalysis)
        {
            merged.analyzedAt = persisted.analyzedAt;
            merged.analysisAlgorithm = persisted.analysisAlgorithm;
            merged.bpm = persisted.bpm;
            merged.firstBeatOffset = persisted.firstBeatOffset;
            if (persisted.trackLengthSeconds > 0.0 && merged.trackLengthSeconds <= 0.0)
                merged.trackLengthSeconds = persisted.trackLengthSeconds;
            merged.beatPositions = persisted.beatPositions;
        }

        if (persisted.updatedAt > 0)
            merged.updatedAt = persisted.updatedAt;
    };

    if (auto existing = model->findTrackByPath(track.filePath))
    {
        mergePersistedFields(*existing);
    }
    else if (libraryDatabase)
    {
        if (auto persisted = libraryDatabase->loadTrackByPath(track.filePath))
            mergePersistedFields(*persisted);
    }

    model->addOrUpdateTrack(merged);
    persistTrack(merged);
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
    // After first full load, auto-size the non-title columns to contents once
    autoSizeColumnsInitial();
    
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
    // Map SortMode to column index for header sorting and indicator
    int column = 0;
    switch (mode) {
        case LibraryTableModel::SortByTitle:    column = LibraryTableModel::TitleColumn; break;
        case LibraryTableModel::SortByArtist:   column = LibraryTableModel::ArtistColumn; break;
        case LibraryTableModel::SortByAlbum:    column = LibraryTableModel::AlbumColumn; break;
        case LibraryTableModel::SortByDuration: column = LibraryTableModel::DurationColumn; break;
        case LibraryTableModel::SortByBpm:      column = LibraryTableModel::BpmColumn; break;
        case LibraryTableModel::SortByGenre:    column = LibraryTableModel::GenreColumn; break;
        case LibraryTableModel::SortByYear:     column = LibraryTableModel::YearColumn; break;
        case LibraryTableModel::SortByFileSize: column = LibraryTableModel::FileSizeColumn; break;
    }
    tableView->horizontalHeader()->setSortIndicatorShown(true);
    tableView->sortByColumn(column, Qt::AscendingOrder);
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

    const bool hasSelection = !getSelectedFiles().isEmpty();
    if (actionAnalyzeTrack)
        actionAnalyzeTrack->setEnabled(hasSelection);
    if (actionAnalyzeAdvanced)
        actionAnalyzeAdvanced->setEnabled(hasSelection);
}

void LibraryManager::updateStatusLabel()
{
    if (isLoading || !model || !statusLabel) return;

    if (model->isPlaylistFilterActive() && currentPlaylistId > 0)
    {
        updatePlaylistStatusUi();
        return;
    }
    
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

void LibraryManager::onContextMenuRequested(const QPoint& pos)
{
    QMenu menu(this);
    // Basic actions
    menu.addAction(actionAddFiles);
    menu.addAction(actionAddFolder);
    menu.addSeparator();

    // Track-specific section only if a valid row under cursor
    QModelIndex index = tableView->indexAt(pos);
    if (index.isValid() && tableView->selectionModel() && !tableView->selectionModel()->isSelected(index))
    {
        tableView->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }

    const TrackInfo* track = nullptr;
    if (index.isValid()) {
        track = model->getTrack(index.row());
    }

    const QStringList selectedFiles = getSelectedFiles();

    if (track) {
        QMenu* loadToMenu = menu.addMenu("Load To");
        actionLoadDeck1 = new QAction("Deck 1", loadToMenu);
        actionLoadDeck2 = new QAction("Deck 2", loadToMenu);
        loadToMenu->addAction(actionLoadDeck1);
        loadToMenu->addAction(actionLoadDeck2);

        QString filePath = track->filePath;
        connect(actionLoadDeck1, &QAction::triggered, this, [this, filePath]() {
            emit loadToDeck(1, filePath);
        });
        connect(actionLoadDeck2, &QAction::triggered, this, [this, filePath]() {
            emit loadToDeck(2, filePath);
        });

        if (!playlistRecords.isEmpty())
        {
            QMenu* addToMenu = menu.addMenu(tr("Add to Playlist"));
            for (const auto& record : playlistRecords)
            {
                QAction* action = addToMenu->addAction(record.name);
                connect(action, &QAction::triggered, this, [this, record, selectedFiles]() {
                    addTracksToPlaylist(record.id, selectedFiles);
                });
            }
        }

        if (model->isPlaylistFilterActive() && currentPlaylistId > 0)
        {
            QAction* removeAction = menu.addAction(tr("Remove from \"%1\"").arg(playlistNameById(currentPlaylistId)));
            connect(removeAction, &QAction::triggered, this, [this, selectedFiles]() {
                removeTracksFromCurrentPlaylist(selectedFiles);
            });
        }

        actionAnalyzeTrack->setEnabled(true);
        if (actionAnalyzeAdvanced)
            actionAnalyzeAdvanced->setEnabled(true);
        menu.addAction(actionAnalyzeTrack);
        if (actionAnalyzeAdvanced)
            menu.addAction(actionAnalyzeAdvanced);
        menu.addSeparator();
    }
    else
    {
        actionAnalyzeTrack->setEnabled(false);
        if (actionAnalyzeAdvanced)
            actionAnalyzeAdvanced->setEnabled(false);
    }

    menu.addAction(actionRefresh);
    menu.addSeparator();
    menu.addAction(actionClearLibrary);
    menu.exec(tableView->viewport()->mapToGlobal(pos));
}

void LibraryManager::autoSizeColumnsInitial()
{
    if (columnsSizedOnce || !tableView || !model) return;
    if (model->rowCount() == 0) return;
    columnsSizedOnce = true;
    auto* hh = tableView->horizontalHeader();
    if (!hh) return;

    // Temporarily set other columns to ResizeToContents to measure
    for (int col = LibraryTableModel::ArtistColumn; col < LibraryTableModel::ColumnCount; ++col) {
        hh->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    }
    tableView->resizeColumnsToContents();

    // Clamp overly wide columns to keep Title dominant
    auto clamp = [&](int col, int maxWidth) {
        int w = hh->sectionSize(col);
        if (w > maxWidth) hh->resizeSection(col, maxWidth);
    };
    clamp(LibraryTableModel::ArtistColumn,   260);
    clamp(LibraryTableModel::AlbumColumn,    240);
    clamp(LibraryTableModel::DurationColumn, 80);
    clamp(LibraryTableModel::BpmColumn,      80);
    clamp(LibraryTableModel::GenreColumn,    180);
    clamp(LibraryTableModel::YearColumn,     80);
    clamp(LibraryTableModel::FileSizeColumn, 110);

    // Restore resize modes: Title stretches, others interactive
    hh->setSectionResizeMode(LibraryTableModel::TitleColumn, QHeaderView::Stretch);
    for (int col = LibraryTableModel::ArtistColumn; col < LibraryTableModel::ColumnCount; ++col) {
        hh->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    // Ensure Title starts notably wide
    hh->resizeSection(LibraryTableModel::TitleColumn, qMax(hh->sectionSize(LibraryTableModel::TitleColumn), 560));
}

void LibraryManager::restoreColumnState()
{
    if (libraryUiStatePath.isEmpty()) {
        return;
    }
    QSettings s(libraryUiStatePath, QSettings::IniFormat);
    QByteArray headerState = s.value("library/headerState").toByteArray();
    if (!headerState.isEmpty()) {
        tableView->horizontalHeader()->restoreState(headerState);
    }
}

void LibraryManager::saveColumnState()
{
    if (!tableView || libraryUiStatePath.isEmpty()) return;
    QSettings s(libraryUiStatePath, QSettings::IniFormat);
    s.setValue("library/headerState", tableView->horizontalHeader()->saveState());
}

void LibraryManager::initializeStoragePaths()
{
    auto& config = AppConfig::instance();
    config.createDirectories();
    libraryDatabasePath = config.getLibraryDatabasePath();
    libraryXmlBackupPath = config.getLibraryXmlBackupPath();
    libraryUiStatePath = config.getUiStatePath("library_ui.ini");

    qDebug() << "LibraryManager storage initialized:";
    qDebug() << "  App data root:" << config.getAppDataDirectory();
    qDebug() << "  Library database:" << libraryDatabasePath;
    qDebug() << "  Legacy XML backup:" << libraryXmlBackupPath;
    qDebug() << "  UI state file:" << libraryUiStatePath;
}

void LibraryManager::loadExistingTracks()
{
    if (!libraryDatabase || !model)
        return;

    const QVector<TrackInfo> tracks = libraryDatabase->loadAllTracks();
    if (tracks.isEmpty())
        return;

    for (const auto& track : tracks)
        model->addOrUpdateTrack(track);

    updateStatusLabel();
    autoSizeColumnsInitial();
}

void LibraryManager::persistTrack(const TrackInfo& track)
{
    if (!libraryDatabase)
        return;

    if (!libraryDatabase->upsertTrack(track))
        qWarning() << "Failed to persist track metadata for" << track.filePath;
}

void LibraryManager::applyAnalysisResult(const QString& filePath,
                                         double bpm,
                                         double firstBeatOffset,
                                         double trackLengthSeconds,
                                         const QVector<double>& beatPositions,
                                         const QString& algorithm,
                                         bool analysisFailed)
{
    if (!model)
        return;

    TrackInfo updated(filePath);

    if (auto existing = model->findTrackByPath(filePath))
    {
        updated = *existing;
    }
    else
    {
        QFileInfo info(filePath);
        updated.title = info.baseName();
        updated.fileSize = info.exists() ? info.size() : 0;
        updated.lastModified = info.exists() ? info.lastModified().toSecsSinceEpoch() : 0;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        updated.addedAt = now;
        updated.updatedAt = now;
        if (updated.duration <= 0.0 && trackLengthSeconds > 0.0)
            updated.duration = trackLengthSeconds;
        if (updated.trackLengthSeconds <= 0.0)
            updated.trackLengthSeconds = updated.duration;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    updated.bpm = bpm > 0.0 ? bpm : 0.0;
    updated.firstBeatOffset = firstBeatOffset;
    if (trackLengthSeconds > 0.0)
    {
        updated.trackLengthSeconds = trackLengthSeconds;
        if (updated.duration <= 0.0)
            updated.duration = trackLengthSeconds;
    }
    updated.beatPositions = beatPositions;
    updated.analysisAlgorithm = algorithm;
    updated.analysisFailed = analysisFailed || bpm <= 0.0;
    updated.analyzedAt = now;
    updated.updatedAt = now;

    model->addOrUpdateTrack(updated);
    persistTrack(updated);
    updateStatusLabel();
}

void LibraryManager::notifyAnalysisStarted(const QString& filePath)
{
    if (filePath.isEmpty())
        return;

    activeAnalyses.insert(filePath, 0.0);
    updateAnalysisUi();
}

void LibraryManager::notifyAnalysisProgress(const QString& filePath, double progress)
{
    if (!activeAnalyses.contains(filePath))
        return;

    const double clamped = std::clamp(progress, 0.0, 1.0);
    activeAnalyses[filePath] = clamped;
    updateAnalysisUi();
}

void LibraryManager::notifyAnalysisFinished(const QString& filePath, bool success)
{
    if (activeAnalyses.remove(filePath) > 0)
        ++analysesCompleted;

    if (analysisStatusLabel && activeAnalyses.isEmpty())
    {
        const QString fileName = QFileInfo(filePath).fileName();
        if (success)
            analysisStatusLabel->setText(QStringLiteral("Analysis finished for %1").arg(fileName));
        else
            analysisStatusLabel->setText(QStringLiteral("Analysis failed for %1").arg(fileName));
    }

    updateAnalysisUi();
}

std::optional<std::array<double, 8>> LibraryManager::getCuePointsForTrack(const QString& filePath) const
{
    if (filePath.isEmpty())
        return std::nullopt;

    if (model)
    {
        if (auto existing = model->findTrackByPath(filePath))
        {
            if (existing->hasCuePoints)
                return existing->cuePoints;
        }
    }

    if (libraryDatabase)
    {
        if (auto persisted = libraryDatabase->loadTrackByPath(filePath))
        {
            if (persisted->hasCuePoints)
                return persisted->cuePoints;
        }
    }

    return std::nullopt;
}

void LibraryManager::saveCuePointsForTrack(const QString& filePath, const std::array<double, 8>& cuePoints)
{
    if (filePath.isEmpty())
        return;

    auto hasValidCuePoints = [](const std::array<double, 8>& cues) {
        for (double c : cues)
        {
            if (c >= 0.0)
                return true;
        }
        return false;
    };

    auto cuesEqual = [](const std::array<double, 8>& a, const std::array<double, 8>& b) {
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::abs(a[i] - b[i]) > 1e-6)
                return false;
        }
        return true;
    };

    TrackInfo track(filePath);
    bool hadExisting = false;

    if (model)
    {
        if (auto existing = model->findTrackByPath(filePath))
        {
            track = *existing;
            hadExisting = true;
            if (existing->hasCuePoints && cuesEqual(existing->cuePoints, cuePoints))
                return;
        }
    }

    if (!hadExisting && libraryDatabase)
    {
        if (auto persisted = libraryDatabase->loadTrackByPath(filePath))
        {
            track = *persisted;
            hadExisting = true;
            if (persisted->hasCuePoints && cuesEqual(persisted->cuePoints, cuePoints))
                return;
        }
    }

    track.cuePoints = cuePoints;
    track.hasCuePoints = hasValidCuePoints(cuePoints);

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (track.addedAt == 0)
        track.addedAt = now;
    track.updatedAt = now;

    if (model)
        model->addOrUpdateTrack(track);

    persistTrack(track);
}

std::optional<TrackInfo> LibraryManager::getTrackInfo(const QString& filePath) const
{
    if (filePath.isEmpty())
        return std::nullopt;

    if (model)
    {
        if (auto existing = model->findTrackByPath(filePath))
            return *existing;
    }

    if (libraryDatabase)
    {
        if (auto persisted = libraryDatabase->loadTrackByPath(filePath))
            return persisted;
    }

    return std::nullopt;
}

void LibraryManager::updateAnalysisUi()
{
    if (!analysisProgressBar || !analysisStatusLabel)
        return;

    analysisProgressBar->setVisible(true);
    analysisStatusLabel->setVisible(true);

    if (activeAnalyses.isEmpty())
    {
        analysisProgressBar->setEnabled(false);
        analysisProgressBar->setRange(0, 100);
        analysisProgressBar->setValue(0);

        const QString currentText = analysisStatusLabel->text();
        if (currentText.trimmed().isEmpty() || currentText.startsWith(QStringLiteral("Analyzing")))
            analysisStatusLabel->setText(QStringLiteral("Analysis idle"));

        analysesCompleted = 0;
        return;
    }

    analysisProgressBar->setEnabled(true);

    double sum = 0.0;
    for (auto it = activeAnalyses.cbegin(); it != activeAnalyses.cend(); ++it)
        sum += it.value();

    const int activeCount = activeAnalyses.size();
    const double average = activeCount > 0 ? sum / static_cast<double>(activeCount) : 0.0;
    analysisProgressBar->setRange(0, 100);
    analysisProgressBar->setValue(static_cast<int>(std::round(average * 100.0)));

    const int totalTasks = analysesCompleted + activeCount;
    const QString labelText = QStringLiteral("Analyzing %1 track%2 (%3/%4 done)")
                                  .arg(activeCount)
                                  .arg(activeCount == 1 ? QString() : QStringLiteral("s"))
                                  .arg(analysesCompleted)
                                  .arg(std::max(totalTasks, 1));

    analysisStatusLabel->setText(labelText);
}
