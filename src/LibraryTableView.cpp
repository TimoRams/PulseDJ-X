#include "LibraryManager.h"

#include <QApplication>
#include <QDrag>
#include <QHeaderView>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>

LibraryTableView::LibraryTableView(QWidget* parent)
    : QTableView(parent)
{
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    setTextElideMode(Qt::ElideRight);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    auto* hh = horizontalHeader();
    hh->setSortIndicatorShown(true);
    hh->setSectionsClickable(true);
    hh->setHighlightSections(false);
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->setStretchLastSection(false);
    hh->setSectionResizeMode(LibraryTableModel::TitleColumn, QHeaderView::Stretch);
    hh->resizeSection(LibraryTableModel::TitleColumn, 480);
    for (int col = LibraryTableModel::ArtistColumn; col < LibraryTableModel::ColumnCount; ++col) {
        hh->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    verticalHeader()->setVisible(false);
    verticalHeader()->setDefaultSectionSize(20);

    setColumnWidth(LibraryTableModel::TitleColumn, 560);
    setColumnWidth(LibraryTableModel::ArtistColumn, 180);
    setColumnWidth(LibraryTableModel::AlbumColumn, 160);
    setColumnWidth(LibraryTableModel::DurationColumn, 70);
    setColumnWidth(LibraryTableModel::BpmColumn, 60);
    setColumnWidth(LibraryTableModel::GenreColumn, 110);
    setColumnWidth(LibraryTableModel::YearColumn, 60);
    setColumnWidth(LibraryTableModel::FileSizeColumn, 70);
    setSortingEnabled(true);
    sortByColumn(LibraryTableModel::TitleColumn, Qt::AscendingOrder);
    setStyleSheet(
        "QTableView { "
        "    background: #181a1b; "
        "    color: #e0e0e0; "
        "    alternate-background-color: #202224; "
        "    selection-background-color: #2d5aa0; "
        "    border: none; "
        "    gridline-color: transparent; "
        "} "
        "QHeaderView::section { "
        "    background: #23272a; "
        "    color: #e0e0e0; "
        "    border: none; "
        "    padding: 4px; "
        "    font-weight: 600; "
        "} "
        "QTableView::item { "
        "    padding-left: 6px; "
        "    padding-right: 6px; "
        "    border: none; "
        "}"
    );
}

void LibraryTableView::startDrag(Qt::DropActions supportedActions)
{
    QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) return;
    
    QMimeData* mimeData = model()->mimeData(indexes);
    if (!mimeData) return;
    
    QDrag* drag = new QDrag(this);
    drag->setMimeData(mimeData);
    
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
