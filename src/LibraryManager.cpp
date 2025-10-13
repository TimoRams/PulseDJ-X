#include "LibraryManager.h"
#include "AppConfig.h"
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
#include <QAbstractButton>
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
#include <QStyle>
#include <optional>
#include <cstring>
#include <cmath>
#include <iostream>
#include <QDebug>
#include <algorithm>

LibraryManager::LibraryManager(juce::AudioFormatManager* formatManager, QWidget* parent)
    : QWidget(parent),
      mainSplitter(nullptr),
      fileSystemTree(nullptr),
      fileSystemModel(nullptr),
      tableView(nullptr),
      model(nullptr),
      sortComboBox(nullptr),
      filterLineEdit(nullptr),
      actionAddFiles(nullptr),
      actionAddFolder(nullptr),
      actionRefresh(nullptr),
      actionClearLibrary(nullptr),
      statusLabel(nullptr),
      progressBar(nullptr),
      loaderThread(nullptr),
      audioFormatManager(formatManager),
      filterUpdateTimer(new QTimer(this))
{
    setObjectName("LibraryManager");
    setAcceptDrops(true);

    initializeStoragePaths();

    libraryDatabase = std::make_unique<LibraryDatabase>();
    if (!libraryDatabase->open(libraryDatabasePath))
    {
        qWarning() << "Failed to open library database at" << libraryDatabasePath;
        libraryDatabase.reset();
    }

    setupUI();

    filterUpdateTimer->setInterval(180);
    filterUpdateTimer->setSingleShot(true);
    connect(filterUpdateTimer, &QTimer::timeout, this, &LibraryManager::onFilterTextChanged);

    setupFileSystemModel();

    if (libraryDatabase)
    {
        loadExistingTracks();
        loadPlaylists();
    }
    else
    {
        loadPlaylists();
    }

    updateStatusLabel();
}

LibraryManager::~LibraryManager()
{
    saveColumnState();

    if (filterUpdateTimer)
        filterUpdateTimer->stop();

    if (loaderThread)
    {
        loaderThread->stop();
        loaderThread->wait(2000);
        loaderThread->deleteLater();
        loaderThread = nullptr;
    }
}
#include <QDateTime>

void LibraryManager::setupUI()
{
    setStyleSheet(
        "QWidget { background-color: #101114; color: #f0f0f0; font-family: 'Lato', 'Arial', sans-serif; }"
        "QSplitter::handle { background-color: #23262e; }"
        "QSplitter::handle:horizontal { width: 2px; }"
        "#libraryIconSidebar { background: #0c0d10; border-right: 1px solid #23262e; }"
        "QToolButton[sidebarButton=\"true\"] { background: transparent; border: none; padding: 8px; margin: 0; icon-size: 22px; }"
        "QToolButton[sidebarButton=\"true\"]:hover { background: rgba(66,133,244,0.18); border-radius: 8px; }"
        "QToolButton[sidebarButton=\"true\"]:checked { background: rgba(66,133,244,0.28); border-radius: 8px; }"
        "#libraryMiddlePanel { background: #14161a; border-right: 1px solid #262931; }"
        "#navigationHeaderLabel { font-weight: 600; font-size: 10px; letter-spacing: 0.8px; text-transform: uppercase; color: #8b92a3; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar::tab { background: transparent; color: #a8acb3; padding: 7px 6px; margin: 1px; border-radius: 7px; min-width: 70px; font-size: 9px; font-weight: 600; }"
        "QTabBar::tab:selected { background: #2f6ae0; color: #ffffff; }"
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
        "QTreeWidget { background-color: transparent; border: none; }"
        "QTreeWidget::item { height: 20px; color: #d8dce8; }"
        "QTreeWidget::item:selected { background: #2f6ae0; border-radius: 5px; color: #ffffff; }"
        "QTreeWidget::item:hover { background: rgba(47,106,224,0.16); border-radius: 5px; }"
        "QTableView { font-size: 10px; background: #0f1014; alternate-background-color: #15171d; selection-background-color: #2f6ae0; border: none; gridline-color: #1f232c; }"
        "QHeaderView::section { font-weight: 600; font-size: 9px; background: #181b22; color: #d8dce8; border: none; padding: 4px 3px; }"
        "QTableView::item { padding-left: 5px; padding-right: 5px; }"
        "QProgressBar { height: 9px; background: #1f232c; border: 1px solid #323845; border-radius: 4px; }"
        "QProgressBar::chunk { background: #4188ff; border-radius: 4px; }"
    );

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(5);
    mainLayout->setContentsMargins(6, 6, 6, 6);

    mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->setHandleWidth(2);

    // === LEFT ICON SIDEBAR ===
    iconSidebar = new QWidget(mainSplitter);
    iconSidebar->setObjectName("libraryIconSidebar");
    iconSidebar->setFixedWidth(60);
    auto* iconLayout = new QVBoxLayout(iconSidebar);
    iconLayout->setContentsMargins(10, 14, 10, 14);
    iconLayout->setSpacing(10);

    sidebarButtonGroup = new QButtonGroup(iconSidebar);
    sidebarButtonGroup->setExclusive(true);

    auto addSidebarButton = [&](SidebarSection section, const QIcon& icon, const QString& tooltip) {
        QToolButton* button = createSidebarButton(section, icon, tooltip);
        iconLayout->addWidget(button, 0, Qt::AlignHCenter);
        return button;
    };

    addSidebarButton(SidebarSection::Collection, style()->standardIcon(QStyle::SP_DirHomeIcon), tr("Collection"));
    addSidebarButton(SidebarSection::Playlists, style()->standardIcon(QStyle::SP_FileDialogListView), tr("Playlists"));
    addSidebarButton(SidebarSection::Explorer, style()->standardIcon(QStyle::SP_DirIcon), tr("Explorer"));
    addSidebarButton(SidebarSection::Streaming, style()->standardIcon(QStyle::SP_BrowserReload), tr("Streaming"));
    addSidebarButton(SidebarSection::Devices, style()->standardIcon(QStyle::SP_DriveHDIcon), tr("Devices"));
    addSidebarButton(SidebarSection::Settings, style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Settings"));
    iconLayout->addStretch(1);

    connect(sidebarButtonGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, &LibraryManager::onSidebarSectionChanged);

    // === MIDDLE PANEL: CONTEXTUAL NAVIGATION ===
    auto* middlePanel = new QWidget(mainSplitter);
    middlePanel->setObjectName("libraryMiddlePanel");
    middlePanel->setMinimumWidth(240);
    middlePanel->setMaximumWidth(360);
    auto* middleLayout = new QVBoxLayout(middlePanel);
    middleLayout->setContentsMargins(6, 6, 6, 6);
    middleLayout->setSpacing(6);

    navigationHeader = new QWidget(middlePanel);
    auto* navHeaderLayout = new QHBoxLayout(navigationHeader);
    navHeaderLayout->setContentsMargins(0, 0, 0, 0);
    navHeaderLayout->setSpacing(6);
    navigationTitleLabel = new QLabel(tr("Collection"), navigationHeader);
    navigationTitleLabel->setObjectName("navigationHeaderLabel");
    addPlaylistButton = new QToolButton(navigationHeader);
    addPlaylistButton->setIcon(QIcon::fromTheme("list-add", style()->standardIcon(QStyle::SP_FileDialogNewFolder)));
    addPlaylistButton->setIconSize(QSize(14, 14));
    addPlaylistButton->setAutoRaise(true);
    addPlaylistButton->setCursor(Qt::PointingHandCursor);
    addPlaylistButton->setToolTip(tr("Create new playlist"));
    navHeaderLayout->addWidget(navigationTitleLabel, 1);
    navHeaderLayout->addWidget(addPlaylistButton, 0, Qt::AlignRight);
    navigationHeader->setLayout(navHeaderLayout);
    middleLayout->addWidget(navigationHeader, 0);

    navigationStack = new QStackedWidget(middlePanel);
    navigationStack->setObjectName("libraryNavigationStack");

    collectionTree = new QTreeWidget(navigationStack);
    collectionTree->setHeaderHidden(true);
    collectionTree->setIndentation(18);
    collectionTree->setUniformRowHeights(true);
    collectionTree->setAnimated(true);
    collectionTree->setSelectionMode(QAbstractItemView::SingleSelection);

    playlistPanel = new QWidget(navigationStack);
    auto* playlistLayout = new QVBoxLayout(playlistPanel);
    playlistLayout->setContentsMargins(0, 0, 0, 0);
    playlistLayout->setSpacing(0);
    navigationTree = new QTreeWidget(playlistPanel);
    navigationTree->setObjectName("playlistNavigationTree");
    navigationTree->setHeaderHidden(true);
    navigationTree->setIndentation(18);
    navigationTree->setUniformRowHeights(true);
    navigationTree->setAnimated(true);
    navigationTree->setSelectionMode(QAbstractItemView::SingleSelection);
    navigationTree->setContextMenuPolicy(Qt::CustomContextMenu);
    navigationTree->setFocusPolicy(Qt::StrongFocus);
    playlistLayout->addWidget(navigationTree);
    playlistPanel->setLayout(playlistLayout);

    explorationPanel = new QWidget(navigationStack);
    auto* explorerLayout = new QVBoxLayout(explorationPanel);
    explorerLayout->setContentsMargins(0, 0, 0, 0);
    explorerLayout->setSpacing(4);
    auto* explorerLabel = new QLabel(tr("Folders"), explorationPanel);
    explorerLabel->setStyleSheet("font-weight: 600; font-size: 9px; color: #8b92a3; padding-left: 4px;");
    explorerContainer = new QWidget(explorationPanel);
    auto* explorerContainerLayout = new QVBoxLayout(explorerContainer);
    explorerContainerLayout->setContentsMargins(0, 0, 0, 0);
    explorerContainerLayout->setSpacing(0);
    fileSystemTree = new QTreeView(explorerContainer);
    fileSystemTree->setHeaderHidden(true);
    fileSystemTree->setRootIsDecorated(true);
    fileSystemTree->setDragEnabled(true);
    fileSystemTree->setDragDropMode(QAbstractItemView::DragOnly);
    explorerContainerLayout->addWidget(fileSystemTree);
    explorerContainer->setLayout(explorerContainerLayout);
    explorerLayout->addWidget(explorerLabel);
    explorerLayout->addWidget(explorerContainer, 1);
    explorationPanel->setLayout(explorerLayout);

    streamingPanel = createPlaceholderPanel(tr("Streaming"), tr("Connect a streaming service to browse playlists."));
    devicesPanel = createPlaceholderPanel(tr("Devices"), tr("No devices connected."));
    settingsPanel = createPlaceholderPanel(tr("Settings"), tr("Library settings coming soon."));

    navigationStack->insertWidget(static_cast<int>(SidebarSection::Collection), collectionTree);
    navigationStack->insertWidget(static_cast<int>(SidebarSection::Playlists), playlistPanel);
    navigationStack->insertWidget(static_cast<int>(SidebarSection::Explorer), explorationPanel);
    navigationStack->insertWidget(static_cast<int>(SidebarSection::Streaming), streamingPanel);
    navigationStack->insertWidget(static_cast<int>(SidebarSection::Devices), devicesPanel);
    navigationStack->insertWidget(static_cast<int>(SidebarSection::Settings), settingsPanel);
    navigationStack->setCurrentIndex(static_cast<int>(SidebarSection::Collection));

    middleLayout->addWidget(navigationStack, 1);

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

    // Add sections to splitter
    mainSplitter->addWidget(iconSidebar);
    mainSplitter->addWidget(middlePanel);
    mainSplitter->addWidget(rightPanel);

    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 0);
    mainSplitter->setStretchFactor(2, 1);
    mainSplitter->setSizes({60, 280, 720});

    // Add splitter to main layout
    mainLayout->addWidget(mainSplitter);

    connect(addPlaylistButton, &QToolButton::clicked, this, &LibraryManager::onAddPlaylistClicked);
    connect(collectionTree, &QTreeWidget::currentItemChanged, this, &LibraryManager::onNavigationItemChanged);
    connect(navigationTree, &QTreeWidget::currentItemChanged, this, &LibraryManager::onNavigationItemChanged);
    connect(navigationTree, &QWidget::customContextMenuRequested, this, &LibraryManager::onNavigationContextMenu);

    if (sidebarButtonGroup)
    {
        if (QAbstractButton* collectionButton = sidebarButtonGroup->button(static_cast<int>(SidebarSection::Collection)))
            collectionButton->setChecked(true);
    }

    buildCollectionTree();
    setActiveSidebarSection(SidebarSection::Collection);

    initializeNavigationState();
    updateStatusLabel();
}

QToolButton* LibraryManager::createSidebarButton(SidebarSection section, const QIcon& icon, const QString& tooltip)
{
    auto* button = new QToolButton(iconSidebar);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setIcon(icon);
    button->setIconSize(QSize(20, 20));
    button->setToolTip(tooltip);
    button->setProperty("sidebarButton", true);

    if (sidebarButtonGroup)
        sidebarButtonGroup->addButton(button, static_cast<int>(section));

    return button;
}

QWidget* LibraryManager::createPlaceholderPanel(const QString& title, const QString& message)
{
    auto* panel = new QWidget(navigationStack);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 18, 12, 18);
    layout->setSpacing(8);

    auto* titleLabel = new QLabel(title, panel);
    titleLabel->setStyleSheet("font-weight: 600; font-size: 10px; letter-spacing: 0.8px; text-transform: uppercase; color: #8b92a3;");
    auto* messageLabel = new QLabel(message, panel);
    messageLabel->setWordWrap(true);
    messageLabel->setStyleSheet("font-size: 9px; color: #b0b6c2;");

    layout->addWidget(titleLabel, 0, Qt::AlignTop);
    layout->addWidget(messageLabel, 0, Qt::AlignTop);
    layout->addStretch(1);

    return panel;
}

void LibraryManager::buildCollectionTree()
{
    if (!collectionTree)
        return;

    collectionTree->clear();

    collectionRoot = new QTreeWidgetItem(collectionTree);
    collectionRoot->setText(0, tr("Collection"));
    collectionRoot->setData(0, NavigationRole, static_cast<int>(NavigationType::CollectionAll));
    collectionRoot->setFlags(collectionRoot->flags() & ~Qt::ItemIsDragEnabled);

    allTracksItem = new QTreeWidgetItem(collectionRoot);
    allTracksItem->setText(0, tr("All Tracks"));
    allTracksItem->setData(0, NavigationRole, static_cast<int>(NavigationType::CollectionAll));
    allTracksItem->setFlags(allTracksItem->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    collectionTree->expandAll();
}

void LibraryManager::setActiveSidebarSection(SidebarSection section)
{
    activeSidebarSection = section;

    if (sidebarButtonGroup)
    {
        QSignalBlocker blocker(*sidebarButtonGroup);
        if (QAbstractButton* button = sidebarButtonGroup->button(static_cast<int>(section)))
            button->setChecked(true);
    }

    if (navigationStack)
        navigationStack->setCurrentIndex(static_cast<int>(section));

    updateNavigationHeader(section);

    const bool playlistToolsVisible = (section == SidebarSection::Playlists);
    if (addPlaylistButton)
        addPlaylistButton->setVisible(playlistToolsVisible);
}

void LibraryManager::updateNavigationHeader(SidebarSection section)
{
    if (!navigationTitleLabel)
        return;

    switch (section)
    {
        case SidebarSection::Collection:
            navigationTitleLabel->setText(tr("Collection"));
            break;
        case SidebarSection::Playlists:
            navigationTitleLabel->setText(tr("Playlists"));
            break;
        case SidebarSection::Explorer:
            navigationTitleLabel->setText(tr("Explorer"));
            break;
        case SidebarSection::Streaming:
            navigationTitleLabel->setText(tr("Streaming"));
            break;
        case SidebarSection::Devices:
            navigationTitleLabel->setText(tr("Devices"));
            break;
        case SidebarSection::Settings:
            navigationTitleLabel->setText(tr("Settings"));
            break;
    }
}

void LibraryManager::onSidebarSectionChanged(int sectionId)
{
    const auto section = static_cast<SidebarSection>(sectionId);
    setActiveSidebarSection(section);

    switch (section)
    {
        case SidebarSection::Collection:
            if (collectionTree && allTracksItem)
                collectionTree->setCurrentItem(allTracksItem);
            currentPlaylistId = -1;
            currentViewMode = LibraryViewMode::Collection;
            clearPlaylistFilter();
            updateStatusLabel();
            break;
        case SidebarSection::Playlists:
            currentViewMode = LibraryViewMode::Playlists;
            if (navigationTree)
            {
                if (QTreeWidgetItem* current = navigationTree->currentItem())
                    onNavigationItemChanged(current, nullptr);
                else if (playlistsRoot)
                    navigationTree->setCurrentItem(playlistsRoot);
            }
            break;
        case SidebarSection::Explorer:
            currentViewMode = LibraryViewMode::Explorer;
            currentPlaylistId = -1;
            clearPlaylistFilter();
            updateStatusLabel();
            break;
        case SidebarSection::Streaming:
            currentViewMode = LibraryViewMode::Streaming;
            currentPlaylistId = -1;
            clearPlaylistFilter();
            if (statusLabel)
                statusLabel->setText(tr("Connect a streaming service to browse tracks"));
            break;
        case SidebarSection::Devices:
            currentViewMode = LibraryViewMode::Devices;
            currentPlaylistId = -1;
            clearPlaylistFilter();
            if (statusLabel)
                statusLabel->setText(tr("No devices connected"));
            break;
        case SidebarSection::Settings:
            currentViewMode = LibraryViewMode::Collection;
            currentPlaylistId = -1;
            clearPlaylistFilter();
            if (statusLabel)
                statusLabel->setText(tr("Library settings coming soon"));
            break;
    }
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
    QSignalBlocker blockButtons(sidebarButtonGroup);
    if (sidebarButtonGroup)
    {
        if (QAbstractButton* button = sidebarButtonGroup->button(static_cast<int>(SidebarSection::Collection)))
            button->setChecked(true);
    }

    setActiveSidebarSection(SidebarSection::Collection);

    if (collectionTree && allTracksItem)
        collectionTree->setCurrentItem(allTracksItem);

    currentViewMode = LibraryViewMode::Collection;
    currentPlaylistId = -1;
}

void LibraryManager::onNavigationItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous)
{
    Q_UNUSED(previous);

    if (!current)
        return;

    auto* tree = qobject_cast<QTreeWidget*>(sender());
    const auto type = static_cast<NavigationType>(current->data(0, NavigationRole).toInt());

    if (tree == collectionTree && activeSidebarSection != SidebarSection::Collection)
        setActiveSidebarSection(SidebarSection::Collection);
    else if (tree == navigationTree && activeSidebarSection != SidebarSection::Playlists)
        setActiveSidebarSection(SidebarSection::Playlists);

    switch (type)
    {
        case NavigationType::CollectionAll:
            currentViewMode = LibraryViewMode::Collection;
            currentPlaylistId = -1;
            clearPlaylistFilter();
            updateStatusLabel();
            break;
        case NavigationType::Playlist: {
            currentViewMode = LibraryViewMode::Playlists;
            const int playlistId = current->data(0, PlaylistIdRole).toInt();
            currentPlaylistId = playlistId;
            applyPlaylistFilter(playlistId);
            break;
        }
        case NavigationType::PlaylistRoot:
            currentViewMode = LibraryViewMode::Playlists;
            currentPlaylistId = -1;
            clearPlaylistFilter();
            updateStatusLabel();
            break;
        default:
            break;
    }
}

void LibraryManager::onNavigationContextMenu(const QPoint& pos)
{
    if (!navigationTree)
        return;

    QTreeWidgetItem* item = navigationTree->itemAt(pos);
    if (item)
        navigationTree->setCurrentItem(item);

    QMenu menu(this);
    menu.addAction(tr("New Playlist"), this, &LibraryManager::onAddPlaylistClicked);

    if (item)
    {
        const auto type = static_cast<NavigationType>(item->data(0, NavigationRole).toInt());
        if (type == NavigationType::Playlist)
        {
            menu.addAction(tr("Rename"), this, &LibraryManager::onRenamePlaylistRequested);
            menu.addAction(tr("Delete"), this, &LibraryManager::onDeletePlaylistRequested);
        }
    }

    menu.exec(navigationTree->viewport()->mapToGlobal(pos));
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
    rebuildPlaylistBranch();
    selectPlaylistById(record.id);
}

void LibraryManager::onRenamePlaylistRequested()
{
    if (!libraryDatabase)
        return;

    const int playlistId = currentNavigationPlaylistId();
    if (playlistId <= 0)
        return;

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

    rebuildPlaylistBranch();
    selectPlaylistById(playlistId);
    updatePlaylistStatusUi();
}

void LibraryManager::onDeletePlaylistRequested()
{
    if (!libraryDatabase)
        return;

    const int playlistId = currentNavigationPlaylistId();
    if (playlistId <= 0)
        return;

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
    rebuildPlaylistBranch();
    selectCollection();
    updateStatusLabel();
}

void LibraryManager::loadPlaylists()
{
    playlistRecords.clear();
    playlistItemCache.clear();

    if (!libraryDatabase)
    {
        rebuildPlaylistBranch();
        return;
    }

    playlistRecords = libraryDatabase->loadAllPlaylists();
    rebuildPlaylistBranch();
    if (currentPlaylistId > 0)
        selectPlaylistById(currentPlaylistId);
}

void LibraryManager::rebuildPlaylistBranch()
{
    if (!navigationTree)
        return;

    if (!playlistsRoot)
    {
        playlistsRoot = new QTreeWidgetItem(navigationTree);
        playlistsRoot->setText(0, tr("Playlists"));
        playlistsRoot->setData(0, NavigationRole, static_cast<int>(NavigationType::PlaylistRoot));
        playlistsRoot->setFlags(playlistsRoot->flags() & ~Qt::ItemIsDragEnabled);
    }

    playlistsRoot->takeChildren();

    for (const auto& record : playlistRecords)
    {
        auto* item = new QTreeWidgetItem(playlistsRoot);
        item->setData(0, NavigationRole, static_cast<int>(NavigationType::Playlist));
        item->setData(0, PlaylistIdRole, record.id);
        updatePlaylistNodeLabel(record.id, item);
    }

    playlistsRoot->setExpanded(true);
    navigationTree->expandItem(playlistsRoot);
}

void LibraryManager::selectCollection()
{
    setActiveSidebarSection(SidebarSection::Collection);

    if (collectionTree && allTracksItem)
        collectionTree->setCurrentItem(allTracksItem);

    currentViewMode = LibraryViewMode::Collection;
    currentPlaylistId = -1;
    clearPlaylistFilter();
    updateStatusLabel();
}

void LibraryManager::selectPlaylistById(int playlistId)
{
    if (playlistId <= 0)
        return;

    setActiveSidebarSection(SidebarSection::Playlists);

    if (!navigationTree)
        return;

    if (QTreeWidgetItem* target = findPlaylistTreeItem(playlistId))
        navigationTree->setCurrentItem(target);
}

int LibraryManager::currentNavigationPlaylistId() const
{
    if (!navigationTree)
        return -1;

    if (QTreeWidgetItem* current = navigationTree->currentItem())
    {
        if (static_cast<NavigationType>(current->data(0, NavigationRole).toInt()) == NavigationType::Playlist)
            return current->data(0, PlaylistIdRole).toInt();
    }
    return -1;
}

QTreeWidgetItem* LibraryManager::findPlaylistTreeItem(int playlistId) const
{
    if (!playlistsRoot)
        return nullptr;

    const int childCount = playlistsRoot->childCount();
    for (int i = 0; i < childCount; ++i)
    {
        QTreeWidgetItem* child = playlistsRoot->child(i);
        if (child->data(0, PlaylistIdRole).toInt() == playlistId)
            return child;
    }
    return nullptr;
}

void LibraryManager::updatePlaylistNodeLabel(int playlistId, QTreeWidgetItem* item)
{
    if (!item)
        return;

    QString name = tr("Playlist");
    int trackCount = 0;
    const int index = findPlaylistIndexById(playlistId);
    if (index >= 0)
    {
        name = playlistRecords.at(index).name;
        trackCount = playlistRecords.at(index).trackCount;
    }

    item->setText(0, QStringLiteral("%1 (%2)").arg(name).arg(trackCount));
    item->setToolTip(0, tr("%1 tracks").arg(trackCount));
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

    if (QTreeWidgetItem* item = findPlaylistTreeItem(playlistId))
        updatePlaylistNodeLabel(playlistId, item);
    else
        rebuildPlaylistBranch();

    selectPlaylistById(playlistId);

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

    if (QTreeWidgetItem* item = findPlaylistTreeItem(currentPlaylistId))
        updatePlaylistNodeLabel(currentPlaylistId, item);
    else
        rebuildPlaylistBranch();

    selectPlaylistById(currentPlaylistId);

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
    Q_UNUSED(index);
    // Double-click loading is intentionally disabled.
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
