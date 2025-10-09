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

namespace
{
QString fromJuceString (const juce::String& value)
{
    return QString::fromUtf8 (value.toRawUTF8());
}

QString cleanupMetadataString (QString text)
{
    if (text.isEmpty())
        return text;

    text.replace (QRegularExpression (QStringLiteral ("[\\x00\\r\\n\\t]+")), QStringLiteral (" "));
    text = text.simplified();
    return text.trimmed();
}

QString normaliseKey (const QString& key)
{
    QString lowered = key.trimmed().toLower();
    QString result;
    result.reserve (lowered.size());
    for (const QChar ch : lowered)
    {
        if (ch.isLetterOrNumber())
            result.append (ch);
    }
    return result;
}

QHash<QString, QString> buildMetadataLookup (const juce::StringPairArray& metadata)
{
    QHash<QString, QString> map;
    const auto keys = metadata.getAllKeys();
    const auto values = metadata.getAllValues();

    for (int i = 0; i < metadata.size(); ++i)
    {
        QString key = cleanupMetadataString (fromJuceString (keys[i]));
        QString value = cleanupMetadataString (fromJuceString (values[i]));

        if (value.isEmpty())
            continue;

        const QString normalisedKey = normaliseKey (key);
        if (! normalisedKey.isEmpty() && ! map.contains (normalisedKey))
            map.insert (normalisedKey, value);

        if (key.contains (QLatin1Char (':')))
        {
            const auto parts = key.split (QLatin1Char (':'), Qt::SkipEmptyParts);
            for (const auto& part : parts)
            {
                const QString altKey = normaliseKey (part);
                if (! altKey.isEmpty() && ! map.contains (altKey))
                    map.insert (altKey, value);
            }
        }
    }

    return map;
}

QString metadataValueFor (const QHash<QString, QString>& map, std::initializer_list<const char*> candidateKeys)
{
    for (const char* key : candidateKeys)
    {
        const QString lookupKey = normaliseKey (QString::fromUtf8 (key));
        auto it = map.constFind (lookupKey);
        if (it != map.cend())
            return it.value();
    }
    return {};
}

double parseBpmString (const QString& raw)
{
    if (raw.isEmpty())
        return 0.0;

    QString candidate = raw.trimmed();
    candidate.replace (QLatin1Char (','), QLatin1Char ('.'));

    static const QRegularExpression numericPattern (QStringLiteral ("([0-9]+(?:\\.[0-9]+)?)"));
    const auto match = numericPattern.match (candidate);
    if (match.hasMatch())
    {
        bool ok = false;
        const double bpm = match.captured (1).toDouble (&ok);
        if (ok)
            return bpm;
    }

    bool ok = false;
    const double bpm = candidate.toDouble (&ok);
    return ok ? bpm : 0.0;
}

QString extractYearString (const QString& raw)
{
    if (raw.isEmpty())
        return {};

    static const QRegularExpression yearPattern (QStringLiteral ("((?:19|20)\\d{2})"));
    const auto match = yearPattern.match (raw);
    if (match.hasMatch())
        return match.captured (1);

    const QString trimmed = raw.trimmed();
    if (trimmed.size() >= 4)
    {
        bool ok = false;
        const QString prefix = trimmed.left (4);
        prefix.toInt (&ok);
        if (ok)
            return prefix;
    }
    return {};
}

QString id3v1GenreName (unsigned char index)
{
    static const QStringList genres = {
        QStringLiteral ("Blues"), QStringLiteral ("Classic Rock"), QStringLiteral ("Country"), QStringLiteral ("Dance"),
        QStringLiteral ("Disco"), QStringLiteral ("Funk"), QStringLiteral ("Grunge"), QStringLiteral ("Hip-Hop"),
        QStringLiteral ("Jazz"), QStringLiteral ("Metal"), QStringLiteral ("New Age"), QStringLiteral ("Oldies"),
        QStringLiteral ("Other"), QStringLiteral ("Pop"), QStringLiteral ("R&B"), QStringLiteral ("Rap"),
        QStringLiteral ("Reggae"), QStringLiteral ("Rock"), QStringLiteral ("Techno"), QStringLiteral ("Industrial"),
        QStringLiteral ("Alternative"), QStringLiteral ("Ska"), QStringLiteral ("Death Metal"), QStringLiteral ("Pranks"),
        QStringLiteral ("Soundtrack"), QStringLiteral ("Euro-Techno"), QStringLiteral ("Ambient"), QStringLiteral ("Trip-Hop"),
        QStringLiteral ("Vocal"), QStringLiteral ("Jazz+Funk"), QStringLiteral ("Fusion"), QStringLiteral ("Trance"),
        QStringLiteral ("Classical"), QStringLiteral ("Instrumental"), QStringLiteral ("Acid"), QStringLiteral ("House"),
        QStringLiteral ("Game"), QStringLiteral ("Sound Clip"), QStringLiteral ("Gospel"), QStringLiteral ("Noise"),
        QStringLiteral ("Alternative Rock"), QStringLiteral ("Bass"), QStringLiteral ("Soul"), QStringLiteral ("Punk"),
        QStringLiteral ("Space"), QStringLiteral ("Meditative"), QStringLiteral ("Instrumental Pop"), QStringLiteral ("Instrumental Rock"),
        QStringLiteral ("Ethnic"), QStringLiteral ("Gothic"), QStringLiteral ("Darkwave"), QStringLiteral ("Techno-Industrial"),
        QStringLiteral ("Electronic"), QStringLiteral ("Pop-Folk"), QStringLiteral ("Eurodance"), QStringLiteral ("Dream"),
        QStringLiteral ("Southern Rock"), QStringLiteral ("Comedy"), QStringLiteral ("Cult"), QStringLiteral ("Gangsta"),
        QStringLiteral ("Top 40"), QStringLiteral ("Christian Rap"), QStringLiteral ("Pop/Funk"), QStringLiteral ("Jungle"),
        QStringLiteral ("Native American"), QStringLiteral ("Cabaret"), QStringLiteral ("New Wave"), QStringLiteral ("Psychadelic"),
        QStringLiteral ("Rave"), QStringLiteral ("Showtunes"), QStringLiteral ("Trailer"), QStringLiteral ("Lo-Fi"),
        QStringLiteral ("Tribal"), QStringLiteral ("Acid Punk"), QStringLiteral ("Acid Jazz"), QStringLiteral ("Polka"),
        QStringLiteral ("Retro"), QStringLiteral ("Musical"), QStringLiteral ("Rock & Roll"), QStringLiteral ("Hard Rock"),
        QStringLiteral ("Folk"), QStringLiteral ("Folk-Rock"), QStringLiteral ("National Folk"), QStringLiteral ("Swing"),
        QStringLiteral ("Fast Fusion"), QStringLiteral ("Bebop"), QStringLiteral ("Latin"), QStringLiteral ("Revival"),
        QStringLiteral ("Celtic"), QStringLiteral ("Bluegrass"), QStringLiteral ("Avantgarde"), QStringLiteral ("Gothic Rock"),
        QStringLiteral ("Progressive Rock"), QStringLiteral ("Psychedelic Rock"), QStringLiteral ("Symphonic Rock"), QStringLiteral ("Slow Rock"),
        QStringLiteral ("Big Band"), QStringLiteral ("Chorus"), QStringLiteral ("Easy Listening"), QStringLiteral ("Acoustic"),
        QStringLiteral ("Humour"), QStringLiteral ("Speech"), QStringLiteral ("Chanson"), QStringLiteral ("Opera"),
        QStringLiteral ("Chamber Music"), QStringLiteral ("Sonata"), QStringLiteral ("Symphony"), QStringLiteral ("Booty Bass"),
        QStringLiteral ("Primus"), QStringLiteral ("Porn Groove"), QStringLiteral ("Satire"), QStringLiteral ("Slow Jam"),
        QStringLiteral ("Club"), QStringLiteral ("Tango"), QStringLiteral ("Samba"), QStringLiteral ("Folklore"),
        QStringLiteral ("Ballad"), QStringLiteral ("Power Ballad"), QStringLiteral ("Rhythmic Soul"), QStringLiteral ("Freestyle"),
        QStringLiteral ("Duet"), QStringLiteral ("Punk Rock"), QStringLiteral ("Drum Solo"), QStringLiteral ("Acapella"),
        QStringLiteral ("Euro-House"), QStringLiteral ("Dance Hall"), QStringLiteral ("Goa"), QStringLiteral ("Drum & Bass"),
        QStringLiteral ("Club-House"), QStringLiteral ("Hardcore"), QStringLiteral ("Terror"), QStringLiteral ("Indie"),
        QStringLiteral ("BritPop"), QStringLiteral ("Negerpunk"), QStringLiteral ("Polsk Punk"), QStringLiteral ("Beat"),
        QStringLiteral ("Christian Gangsta"), QStringLiteral ("Heavy Metal"), QStringLiteral ("Black Metal"), QStringLiteral ("Crossover"),
        QStringLiteral ("Contemporary Christian"), QStringLiteral ("Christian Rock"), QStringLiteral ("Merengue"), QStringLiteral ("Salsa"),
        QStringLiteral ("Thrash Metal"), QStringLiteral ("Anime"), QStringLiteral ("JPop"), QStringLiteral ("Synthpop"),
        QStringLiteral ("Abstract"), QStringLiteral ("Art Rock"), QStringLiteral ("Baroque"), QStringLiteral ("Bhangra"),
        QStringLiteral ("Big Beat"), QStringLiteral ("Breakbeat"), QStringLiteral ("Chillout"), QStringLiteral ("Downtempo"),
        QStringLiteral ("Dub"), QStringLiteral ("EBM"), QStringLiteral ("Eclectic"), QStringLiteral ("Electro"),
        QStringLiteral ("Electroclash"), QStringLiteral ("Emo"), QStringLiteral ("Experimental"), QStringLiteral ("Garage"),
        QStringLiteral ("Global"), QStringLiteral ("IDM"), QStringLiteral ("Illbient"), QStringLiteral ("Industro-Goth"),
        QStringLiteral ("Jam Band"), QStringLiteral ("Krautrock"), QStringLiteral ("Leftfield"), QStringLiteral ("Lounge"),
        QStringLiteral ("Math Rock"), QStringLiteral ("New Romantic"), QStringLiteral ("Nu-Breakz"), QStringLiteral ("Post-Punk"),
        QStringLiteral ("Post-Rock"), QStringLiteral ("Psytrance"), QStringLiteral ("Shoegaze"), QStringLiteral ("Space Rock"),
        QStringLiteral ("Trop Rock"), QStringLiteral ("World Music"), QStringLiteral ("Neoclassical"), QStringLiteral ("Audiobook"),
        QStringLiteral ("Audio Theatre"), QStringLiteral ("Neue Deutsche Welle"), QStringLiteral ("Podcast"), QStringLiteral ("Indie Rock"),
        QStringLiteral ("G-Funk"), QStringLiteral ("Dubstep"), QStringLiteral ("Garage Rock"), QStringLiteral ("Psybient")
    };

    if (index < genres.size())
        return genres.at (index);

    return {};
}

struct Id3v1TagData
{
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString year;
    QString comment;
    int trackNumber = -1;
};

std::optional<Id3v1TagData> readId3v1Tag (const QString& filePath)
{
    QFile file (filePath);
    if (! file.open (QIODevice::ReadOnly))
        return std::nullopt;

    if (file.size() < 128)
        return std::nullopt;

    if (! file.seek (file.size() - 128))
        return std::nullopt;

    const QByteArray data = file.read (128);
    if (data.size() != 128)
        return std::nullopt;

    if (std::memcmp (data.constData(), "TAG", 3) != 0)
        return std::nullopt;

    Id3v1TagData tag;
    tag.title = cleanupMetadataString (QString::fromLatin1 (data.mid (3, 30)));
    tag.artist = cleanupMetadataString (QString::fromLatin1 (data.mid (33, 30)));
    tag.album = cleanupMetadataString (QString::fromLatin1 (data.mid (63, 30)));
    tag.year = cleanupMetadataString (QString::fromLatin1 (data.mid (93, 4)));

    QByteArray comment = data.mid (97, 30);
    if (comment.size() == 30)
    {
        if (comment[28] == 0 && comment[29] != 0)
            tag.trackNumber = static_cast<unsigned char> (comment[29]);

        comment[28] = 0;
    }
    tag.comment = cleanupMetadataString (QString::fromLatin1 (comment));

    const unsigned char genreIndex = static_cast<unsigned char> (data[127]);
    tag.genre = cleanupMetadataString (id3v1GenreName (genreIndex));

    return tag;
}

void applyFilenameHeuristics (const QFileInfo& fileInfo, TrackInfo& track)
{
    const QString baseName = cleanupMetadataString (fileInfo.completeBaseName());

    if (track.title.trimmed().isEmpty())
        track.title = baseName;

    if (track.artist.trimmed().isEmpty())
    {
    static const QRegularExpression artistTitlePattern (QStringLiteral ("^\\s*(.+?)\\s*[-–]\\s*(.+)\\s*$"));
        const auto match = artistTitlePattern.match (fileInfo.completeBaseName());
        if (match.hasMatch())
        {
            const QString artist = cleanupMetadataString (match.captured (1));
            const QString title = cleanupMetadataString (match.captured (2));
            if (! artist.isEmpty())
                track.artist = artist;
            if (! title.isEmpty())
                track.title = title;
        }
    }
}

void applyId3v1Fallback (const std::optional<Id3v1TagData>& tag, TrackInfo& track)
{
    if (! tag)
        return;

    const auto assignIfEmpty = [] (QString& target, const QString& value)
    {
        if (target.trimmed().isEmpty() && ! value.trimmed().isEmpty())
            target = value.trimmed();
    };

    assignIfEmpty (track.title, tag->title);
    assignIfEmpty (track.artist, tag->artist);
    assignIfEmpty (track.album, tag->album);
    assignIfEmpty (track.genre, tag->genre);
    assignIfEmpty (track.year, tag->year);
    assignIfEmpty (track.comment, tag->comment);
}

void finaliseMetadata (const QFileInfo& fileInfo, TrackInfo& track)
{
    track.title = cleanupMetadataString (track.title);
    track.artist = cleanupMetadataString (track.artist);
    track.album = cleanupMetadataString (track.album);
    track.genre = cleanupMetadataString (track.genre);
    track.comment = cleanupMetadataString (track.comment);
    track.year = cleanupMetadataString (track.year);

    if (track.title.isEmpty())
        track.title = cleanupMetadataString (fileInfo.completeBaseName());

    if (! track.key.isEmpty())
    {
        track.key = cleanupMetadataString (track.key.toUpper());
        track.key.remove (QLatin1Char (' '));
    }
    else
    {
        track.key.clear();
    }
	
    if (track.trackLengthSeconds <= 0.0 && track.duration > 0.0)
        track.trackLengthSeconds = track.duration;
}
}

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
    TrackInfo track (filePath);
    const QFileInfo fileInfo (filePath);
    track.fileSize = fileInfo.size();
    track.lastModified = fileInfo.lastModified().toSecsSinceEpoch();
    if (track.addedAt == 0)
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        track.addedAt = now;
        track.updatedAt = now;
    }

    const auto id3v1Tag = readId3v1Tag (filePath);

    try
    {
        juce::File audioFile (filePath.toStdString());

        if (audioFile.exists())
        {
            std::unique_ptr<juce::AudioFormatReader> reader;
            {
                AudioFormatManagerGuard formatGuard;
                reader.reset(audioFormatManager->createReaderFor(audioFile));
            }

            if (reader)
            {
                if (reader->sampleRate > 0)
                    {
                        track.duration = reader->lengthInSamples / reader->sampleRate;
                        track.trackLengthSeconds = track.duration;
                    }

                const auto metadataMap = buildMetadataLookup (reader->metadataValues);

                const auto titleValue = metadataValueFor (metadataMap, { "title", "id3title", "tit2", "tt2", "tracktitle", "song", "name" });
                if (! titleValue.isEmpty())
                    track.title = titleValue;

                QString artistValue = metadataValueFor (metadataMap, { "artist", "id3artist", "tpe1", "albumartist", "tpe2", "band", "orchestra", "performer", "leadartist" });
                if (! artistValue.isEmpty())
                    track.artist = artistValue;

                const auto albumValue = metadataValueFor (metadataMap, { "album", "id3album", "talb", "record", "release", "albumtitle" });
                if (! albumValue.isEmpty())
                    track.album = albumValue;

                const auto genreValue = metadataValueFor (metadataMap, { "genre", "id3genre", "tcon", "style", "category" });
                if (! genreValue.isEmpty())
                    track.genre = genreValue;

                const auto yearValue = metadataValueFor (metadataMap, { "year", "tyer", "tdrc", "id3date", "date", "releasedate", "recordingtime" });
                const auto parsedYear = extractYearString (yearValue);
                if (! parsedYear.isEmpty())
                    track.year = parsedYear;

                const auto commentValue = metadataValueFor (metadataMap, { "comment", "id3comment", "comm", "description", "notes", "text" });
                if (! commentValue.isEmpty())
                    track.comment = commentValue;

                const auto keyValue = metadataValueFor (metadataMap, { "key", "tkey", "initialkey", "initial key", "musickey", "keysig", "keysignature" });
                if (! keyValue.isEmpty())
                    track.key = keyValue;

                const auto bpmValue = metadataValueFor (metadataMap, { "bpm", "tbpm", "tmpo", "tempo", "beatsperminute" });
                const double bpm = parseBpmString (bpmValue);
                if (bpm > 0.0)
                    track.bpm = bpm;
            }
        }
    }
    catch (const std::exception& e)
    {
        qWarning() << "Error loading metadata for" << filePath << ":" << e.what();
    }

    applyId3v1Fallback (id3v1Tag, track);
    applyFilenameHeuristics (fileInfo, track);
    finaliseMetadata (fileInfo, track);

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
        // Right-align numeric-ish columns for readability
        switch (index.column()) {
            case DurationColumn:
            case BpmColumn:
            case YearColumn:
            case FileSizeColumn:
                return int(Qt::AlignRight | Qt::AlignVCenter);
            default:
                return int(Qt::AlignVCenter); // default vertical centering
        }
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

void LibraryTableModel::sort(int column, Qt::SortOrder order)
{
    // Map view column to our SortMode
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
           track.genre.toLower().contains(filterText) ||
           track.year.toLower().contains(filterText) ||
           track.comment.toLower().contains(filterText) ||
           track.key.toLower().contains(filterText);
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
    setShowGrid(false);
    setWordWrap(false);
    setTextElideMode(Qt::ElideRight);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    // Configure headers
    auto* hh = horizontalHeader();
    hh->setSortIndicatorShown(true);
    hh->setSectionsClickable(true);
    hh->setHighlightSections(false);
    hh->setSectionResizeMode(QHeaderView::Interactive);
    // Prefer the title column to get more space when window grows
    hh->setStretchLastSection(false);
    hh->setSectionResizeMode(LibraryTableModel::TitleColumn, QHeaderView::Stretch);
    hh->resizeSection(LibraryTableModel::TitleColumn, 480); // allow artist column to claim some space
    // Other columns keep their initial width but remain resizable by user
    for (int col = LibraryTableModel::ArtistColumn; col < LibraryTableModel::ColumnCount; ++col) {
        hh->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    verticalHeader()->setVisible(false);
    verticalHeader()->setDefaultSectionSize(22); // row height
    
    // Set column widths
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
    setStyleSheet("QTableView { font-family: 'Lato', 'Arial', sans-serif; font-size: 13px; background: #181a1b; alternate-background-color: #222426; selection-background-color: #2d5aa0; border: none; gridline-color: #2a2d2e; } QHeaderView::section { font-weight: bold; font-size: 13px; background: #23272a; color: #e0e0e0; border: none; padding: 6px 4px; } QTableView::item { padding-left: 6px; padding-right: 6px; } QTableView::item:selected { color: #ffffff; } QTableView::item:hover { background: rgba(255,255,255,0.035); } ");
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
        "QWidget { background-color: #1a1a1a; color: #e0e0e0; }"
        "QTableView { font-family: 'Lato', 'Arial', sans-serif; font-size: 13px; background: #181a1b; alternate-background-color: #222426; selection-background-color: #2d5aa0; border: none; }"
        "QHeaderView::section { font-weight: bold; font-size: 13px; background: #23272a; color: #e0e0e0; border: none; padding: 6px 4px; }"
        "QTableView::item { padding-left: 6px; padding-right: 6px; }"
        "QPushButton { background-color: #23272a; border: 1px solid #444; padding: 4px 10px; border-radius: 3px; font-size: 12px; color: #e0e0e0; }"
        "QPushButton:hover { background-color: #3a3a3a; }"
        "QPushButton:pressed { background-color: #1a1a1a; }"
        "QComboBox { background-color: #23272a; border: 1px solid #444; padding: 2px 6px; font-size: 12px; color: #e0e0e0; min-width: 90px; }"
        "QLineEdit { background-color: #23272a; border: 1px solid #444; padding: 2px 6px; font-size: 12px; color: #e0e0e0; min-width: 180px; }"
        "QProgressBar { height: 12px; background: #23272a; border: 1px solid #444; border-radius: 3px; }"
        "QProgressBar::chunk { background: #4a9eff; }"
        "QTreeView { background-color: #1a1a1a; border: 1px solid #555; }"
        "QTreeView::item:selected { background-color: #2d5aa0; }"
        "QTreeView::item:hover { background-color: #2a2a2a; }"
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
    leftPanel->setMinimumWidth(160);
    leftPanel->setMaximumWidth(260);
    
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    // File browser header
    auto* browserHeader = new QLabel("Music Folders", leftPanel);
    browserHeader->setStyleSheet("font-weight: bold; font-size: 13px; padding: 6px 8px; background: #23272a; color: #e0e0e0; border-bottom: 1px solid #444;");
    
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
    
    // Compact top bar: icons + search (no big buttons)
    auto* topBarLayout = new QHBoxLayout();
    topBarLayout->setSpacing(6);
    topBarLayout->setContentsMargins(0,0,0,0);

    // Hidden sort combo (logic only) - we still use header click sorting; hide it.
    sortComboBox = new QComboBox(rightPanel);
    sortComboBox->addItem("Title", LibraryTableModel::SortByTitle);
    sortComboBox->addItem("Artist", LibraryTableModel::SortByArtist);
    sortComboBox->addItem("Album", LibraryTableModel::SortByAlbum);
    sortComboBox->addItem("Duration", LibraryTableModel::SortByDuration);
    sortComboBox->addItem("BPM", LibraryTableModel::SortByBpm);
    sortComboBox->addItem("Genre", LibraryTableModel::SortByGenre);
    sortComboBox->addItem("Year", LibraryTableModel::SortByYear);
    sortComboBox->addItem("File Size", LibraryTableModel::SortByFileSize);
    sortComboBox->setVisible(false);
    connect(sortComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LibraryManager::onSortModeChanged);

    // Actions
    actionAddFiles = new QAction(QIcon::fromTheme("list-add"), "Add Files", this);
    actionAddFolder = new QAction(QIcon::fromTheme("folder-open"), "Add Folder", this);
    actionRefresh = new QAction(QIcon::fromTheme("view-refresh"), "Refresh", this);
    actionClearLibrary = new QAction(QIcon::fromTheme("edit-delete"), "Clear Library", this);
    actionAnalyzeTrack = new QAction(QIcon::fromTheme("view-statistics"), tr("Analyze Track"), this);
    actionAnalyzeTrack->setEnabled(false);
    connect(actionAddFiles, &QAction::triggered, this, &LibraryManager::onAddFilesClicked);
    connect(actionAddFolder, &QAction::triggered, this, &LibraryManager::onAddFolderClicked);
    connect(actionRefresh, &QAction::triggered, this, &LibraryManager::onRefreshClicked);
    connect(actionClearLibrary, &QAction::triggered, this, &LibraryManager::onClearLibraryClicked);
    connect(actionAnalyzeTrack, &QAction::triggered, this, [this]() {
        const QStringList selected = getSelectedFiles();
        if (!selected.isEmpty())
            emit analyzeTracksRequested(selected);
    });

    auto addFilesBtn = new QToolButton(rightPanel); addFilesBtn->setDefaultAction(actionAddFiles); addFilesBtn->setToolTip("Add audio files");
    auto addFolderBtn = new QToolButton(rightPanel); addFolderBtn->setDefaultAction(actionAddFolder); addFolderBtn->setToolTip("Add folder");
    auto refreshBtn = new QToolButton(rightPanel); refreshBtn->setDefaultAction(actionRefresh); refreshBtn->setToolTip("Refresh view");
    auto clearBtn = new QToolButton(rightPanel); clearBtn->setDefaultAction(actionClearLibrary); clearBtn->setToolTip("Clear library");
    for (auto tb : {addFilesBtn, addFolderBtn, refreshBtn, clearBtn}) {
        tb->setAutoRaise(true);
        tb->setIconSize(QSize(16,16));
        tb->setCursor(Qt::PointingHandCursor);
    }

    filterLineEdit = new QLineEdit(rightPanel);
    filterLineEdit->setPlaceholderText("Search...");
    filterLineEdit->setClearButtonEnabled(true);
    filterLineEdit->setFixedHeight(24);
    filterLineEdit->setStyleSheet("QLineEdit { padding-left: 22px; background:#23272a; border:1px solid #444; border-radius:4px; font-size:12px; } QLineEdit:focus { border:1px solid #4a9eff; }");
    connect(filterLineEdit, &QLineEdit::textChanged, [this]() { filterUpdateTimer->start(); });

    // Magnifier icon overlay
    auto searchWrapper = new QHBoxLayout();
    searchWrapper->setContentsMargins(0,0,0,0);
    auto* searchContainer = new QWidget(rightPanel);
    searchContainer->setLayout(searchWrapper);
    auto* searchIcon = new QLabel(searchContainer);
    searchIcon->setPixmap(QIcon::fromTheme("edit-find").pixmap(14,14));
    searchIcon->setStyleSheet("QLabel { padding-left:4px; color:#aaa; }");
    searchWrapper->addWidget(searchIcon);
    searchWrapper->addWidget(filterLineEdit,1);

    topBarLayout->addWidget(addFilesBtn);
    topBarLayout->addWidget(addFolderBtn);
    topBarLayout->addWidget(refreshBtn);
    topBarLayout->addWidget(clearBtn);
    topBarLayout->addSpacing(4);
    topBarLayout->addWidget(searchContainer,1);
    topBarLayout->addWidget(sortComboBox); // hidden
    
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
    auto* statusLayout = new QHBoxLayout();
    statusLabel = new QLabel("Ready", rightPanel);
    progressBar = new QProgressBar(rightPanel);
    progressBar->setVisible(false);
    
    statusLayout->addWidget(statusLabel, 1);
    statusLayout->addWidget(progressBar);
    
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

    auto* analysisLayout = new QHBoxLayout();
    analysisLayout->setContentsMargins(0, 0, 0, 0);
    analysisLayout->setSpacing(8);
    analysisLayout->addWidget(analysisStatusLabel, 1);
    analysisLayout->addWidget(analysisProgressBar, 0);

    // Assemble right panel
    rightLayout->addLayout(topBarLayout);
    rightLayout->addWidget(tableView, 1);
    rightLayout->addLayout(statusLayout);
    rightLayout->addLayout(analysisLayout);
    
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

        actionAnalyzeTrack->setEnabled(true);
        menu.addAction(actionAnalyzeTrack);
        menu.addSeparator();
    }
    else
    {
        actionAnalyzeTrack->setEnabled(false);
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
