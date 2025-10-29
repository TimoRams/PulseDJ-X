#include "LibraryManager.h"

#include <QMimeData>
#include <QUrl>

#include <algorithm>

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
        QStringList tooltipLines;
        tooltipLines << QStringLiteral("Title: %1").arg(track->getDisplayTitle());
        tooltipLines << QStringLiteral("Artist: %1").arg(track->getDisplayArtist());
        if (!track->album.isEmpty())      tooltipLines << QStringLiteral("Album: %1").arg(track->album);
        if (!track->genre.isEmpty())      tooltipLines << QStringLiteral("Genre: %1").arg(track->genre);
        if (!track->year.isEmpty())       tooltipLines << QStringLiteral("Year: %1").arg(track->year);
        if (!track->key.isEmpty())        tooltipLines << QStringLiteral("Key: %1").arg(track->key);
        if (track->bpm > 0.0)             tooltipLines << QStringLiteral("BPM: %1").arg(track->getBpmString());
        if (!track->comment.isEmpty())    tooltipLines << QStringLiteral("Comment: %1").arg(track->comment);
        tooltipLines << QStringLiteral("Path: %1").arg(track->filePath);
        return tooltipLines.join(QLatin1Char('\n'));
    } else if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case DurationColumn:
            case BpmColumn:
            case YearColumn:
            case FileSizeColumn:
                return int(Qt::AlignRight | Qt::AlignVCenter);
            default:
                return int(Qt::AlignVCenter);
        }
    } else if (role == Qt::UserRole) {
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

void LibraryTableModel::sort(int column, Qt::SortOrder order)
{
    SortMode mode = SortByTitle;
    switch (column) {
        case TitleColumn:   mode = SortByTitle; break;
        case ArtistColumn:  mode = SortByArtist; break;
        case AlbumColumn:   mode = SortByAlbum; break;
        case DurationColumn:mode = SortByDuration; break;
        case BpmColumn:     mode = SortByBpm; break;
        case GenreColumn:   mode = SortByGenre; break;
        case YearColumn:    mode = SortByYear; break;
        case FileSizeColumn:mode = SortByFileSize; break;
        default:            mode = SortByTitle; break;
    }
    setSortMode(mode, order);
}

QStringList LibraryTableModel::mimeTypes() const
{
    return { QStringLiteral("text/uri-list") };
}

QMimeData* LibraryTableModel::mimeData(const QModelIndexList& indexes) const
{
    QMimeData* mimeData = new QMimeData();
    QList<QUrl> urls;
    
    QSet<int> rows;
    for (const QModelIndex& index : indexes) {
        if (index.isValid()) {
            rows.insert(index.row());
        }
    }

    urls.reserve(rows.size());

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

void LibraryTableModel::addOrUpdateTrack(const TrackInfo& track)
{
    auto it = std::find_if(allTracks.begin(), allTracks.end(), [&track](const TrackInfo& existing) {
        return existing.filePath.compare(track.filePath, Qt::CaseInsensitive) == 0;
    });

    if (it != allTracks.end())
        *it = track;
    else
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

std::optional<TrackInfo> LibraryTableModel::findTrackByPath(const QString& filePath) const
{
    auto it = std::find_if(allTracks.begin(), allTracks.end(), [&filePath](const TrackInfo& track) {
        return track.filePath.compare(filePath, Qt::CaseInsensitive) == 0;
    });

    if (it == allTracks.end())
        return std::nullopt;

    return *it;
}

void LibraryTableModel::setSortMode(SortMode mode, Qt::SortOrder order)
{
    if (currentSortMode == mode && currentSortOrder == order)
        return;
    emit layoutAboutToBeChanged();
    currentSortMode = mode;
    currentSortOrder = order;
    sortFilteredTracks();
    emit layoutChanged();
}

void LibraryTableModel::setFilterText(const QString& filter)
{
    filterText = filter;
    updateFilteredTracks();
}

void LibraryTableModel::setPlaylistFilter(const QSet<QString>& allowedFiles)
{
    playlistFilter = allowedFiles;
    playlistScopeCount = allowedFiles.size();
    playlistFilterActive = true;
    updateFilteredTracks();
}

void LibraryTableModel::clearPlaylistFilter()
{
    playlistFilter.clear();
    playlistScopeCount = 0;
    playlistFilterActive = false;
    updateFilteredTracks();
}

void LibraryTableModel::updateFilteredTracks()
{
    beginResetModel();

    filteredTracks.clear();
    filteredTracks.reserve(allTracks.size());
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
    if (playlistFilterActive && !playlistFilter.contains(track.filePath))
        return false;

    if (filterText.isEmpty()) return true;

    const auto& f = filterText;
    return track.getDisplayTitle().contains(f, Qt::CaseInsensitive) ||
           track.getDisplayArtist().contains(f, Qt::CaseInsensitive) ||
           track.album.contains(f, Qt::CaseInsensitive) ||
           track.genre.contains(f, Qt::CaseInsensitive) ||
           track.year.contains(f, Qt::CaseInsensitive) ||
           track.comment.contains(f, Qt::CaseInsensitive) ||
           track.key.contains(f, Qt::CaseInsensitive);
}

bool LibraryTableModel::isLessThan(const TrackInfo* a, const TrackInfo* b) const
{
    if (!a || !b) return false;

    switch (currentSortMode) {
        case SortByTitle:
            return QString::compare(a->getDisplayTitle(), b->getDisplayTitle(), Qt::CaseInsensitive) < 0;
        case SortByArtist:
            return QString::compare(a->getDisplayArtist(), b->getDisplayArtist(), Qt::CaseInsensitive) < 0;
        case SortByAlbum:
            return QString::compare(a->album, b->album, Qt::CaseInsensitive) < 0;
        case SortByDuration:
            return a->duration < b->duration;
        case SortByBpm:
            return a->bpm < b->bpm;
        case SortByGenre:
            return QString::compare(a->genre, b->genre, Qt::CaseInsensitive) < 0;
        case SortByYear:
            return a->year < b->year;
        case SortByFileSize:
            return a->fileSize < b->fileSize;
        default:
            return false;
    }
}
