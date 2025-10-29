#include "LibraryDatabase.h"

#include "LibraryManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QVariant>
#include <iostream>
#include <optional>
#include <array>
#include <algorithm>

namespace
{
constexpr auto kConnectionPrefix = "LibraryConnection";
constexpr int kSchemaVersion = 5; // Bumped to 5 for waveform caching

bool execOrLog(QSqlQuery& query, const char* context)
{
    if (!query.exec())
    {
        std::cerr << "LibraryDatabase " << context << " failed: "
                  << query.lastError().text().toStdString() << std::endl;
        return false;
    }
    return true;
}

QString serializeBeats(const QVector<double>& beats)
{
    if (beats.isEmpty())
        return {};

    QString result;
    result.reserve(beats.size() * 12);

    for (int i = 0; i < beats.size(); ++i)
    {
        if (i > 0)
            result.append(QLatin1Char(','));
        result.append(QString::number(beats[i], 'f', 6));
    }

    return result;
}

QVector<double> deserializeBeats(const QString& json)
{
    QVector<double> beats;
    if (json.isEmpty())
        return beats;

    QString current;
    current.reserve(16);

    const auto flush = [&beats, &current]
    {
        bool ok = false;
        const double value = current.toDouble(&ok);
        if (ok)
            beats.append(value);
        current.clear();
    };

    for (int i = 0; i < json.size(); ++i)
    {
        const QChar ch = json.at(i);
        if (ch == QLatin1Char(','))
        {
            flush();
        }
        else
        {
            current.append(ch);
        }
    }

    if (!current.isEmpty())
        flush();

    return beats;
}

QString serializeCuePoints(const std::array<double, 8>& cues)
{
    bool hasValid = false;
    for (double c : cues)
    {
        if (c >= 0.0)
        {
            hasValid = true;
            break;
        }
    }

    if (!hasValid)
        return {};

    QString result;
    result.reserve(static_cast<int>(cues.size()) * 12);

    for (size_t i = 0; i < cues.size(); ++i)
    {
        if (i > 0)
            result.append(QLatin1Char(','));
        result.append(QString::number(cues[i], 'f', 6));
    }

    return result;
}

std::array<double, 8> deserializeCuePoints(const QString& text)
{
    std::array<double, 8> cues;
    cues.fill(-1.0);

    if (text.isEmpty())
        return cues;

    const QStringList parts = text.split(QLatin1Char(','), Qt::KeepEmptyParts);
    const int limit = std::min<int>(static_cast<int>(cues.size()), static_cast<int>(parts.size()));

    for (int i = 0; i < limit; ++i)
    {
        bool ok = false;
        const double value = parts.at(i).toDouble(&ok);
        cues[i] = ok ? value : -1.0;
    }

    return cues;
}

// WAVEFORM CACHING: Serialize/deserialize waveform bins to/from BLOB
QByteArray serializeWaveformBins(const std::vector<float>& bins)
{
    if (bins.empty())
        return QByteArray();
    
    QByteArray data(reinterpret_cast<const char*>(bins.data()), 
                    static_cast<int>(bins.size() * sizeof(float)));
    return data;
}

std::vector<float> deserializeWaveformBins(const QByteArray& data)
{
    std::vector<float> bins;
    if (data.isEmpty() || (data.size() % sizeof(float)) != 0)
        return bins;
    
    bins.resize(data.size() / sizeof(float));
    std::memcpy(bins.data(), data.constData(), data.size());
    return bins;
}

bool ensurePlaylistTables(QSqlDatabase& db)
{
    const char* statements[] = {
        "CREATE TABLE IF NOT EXISTS playlists ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL UNIQUE,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "sort_index INTEGER NOT NULL DEFAULT 0"
        ")",
        "CREATE TABLE IF NOT EXISTS playlist_tracks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
        "track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
        "file_path TEXT NOT NULL,"
        "position INTEGER NOT NULL,"
        "added_at INTEGER NOT NULL,"
        "UNIQUE(playlist_id, position)"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_playlists_sort ON playlists(sort_index, name COLLATE NOCASE)",
        "CREATE INDEX IF NOT EXISTS idx_playlist_tracks_playlist ON playlist_tracks(playlist_id, position)",
        "CREATE INDEX IF NOT EXISTS idx_playlist_tracks_track ON playlist_tracks(track_id)",
        "CREATE INDEX IF NOT EXISTS idx_playlist_tracks_path ON playlist_tracks(file_path)"
    };

    for (const char* sql : statements)
    {
        QSqlQuery query(db);
        if (!query.exec(QString::fromUtf8(sql)))
        {
            std::cerr << "LibraryDatabase: failed to initialize playlist schema: "
                      << query.lastError().text().toStdString() << std::endl;
            return false;
        }
    }

    return true;
}
}

LibraryDatabase::LibraryDatabase(QObject* parent)
    : QObject(parent)
    , connectionName(QStringLiteral("%1_%2").arg(QLatin1String(kConnectionPrefix)).arg(reinterpret_cast<quintptr>(this)))
{
}

LibraryDatabase::~LibraryDatabase()
{
    if (QSqlDatabase::contains(connectionName))
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        if (db.isValid() && db.isOpen())
            db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

bool LibraryDatabase::open(const QString& databasePath)
{
    dbPath = databasePath;

    if (dbPath.isEmpty())
    {
    std::cerr << "LibraryDatabase: provided database path is empty" << std::endl;
        return false;
    }

    if (QSqlDatabase::contains(connectionName))
    {
        QSqlDatabase existing = QSqlDatabase::database(connectionName, false);
        if (existing.isValid() && existing.isOpen())
            existing.close();
        QSqlDatabase::removeDatabase(connectionName);
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(dbPath);

    if (!db.open())
    {
    std::cerr << "LibraryDatabase: failed to open " << dbPath.toStdString()
          << ": " << db.lastError().text().toStdString() << std::endl;
        return false;
    }

    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));

    schemaReady = ensureSchema();
    return schemaReady;
}

QVector<TrackInfo> LibraryDatabase::loadAllTracks() const
{
    QVector<TrackInfo> tracks;
    if (!schemaReady)
        return tracks;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return tracks;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
        "SELECT file_path, title, artist, album, genre, year, duration, bpm, file_size, comment, track_key, file_modified, added_at, updated_at, "
        "analyzed_at, analysis_algorithm, first_beat_offset, track_length, beat_grid, analysis_failed, cue_points, "
        "waveform_max_bins, waveform_min_bins, waveform_audio_start_offset, waveform_analyzed_at "
        "FROM tracks ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE, title COLLATE NOCASE")))
    {
    std::cerr << "LibraryDatabase loadAllTracks failed: "
          << query.lastError().text().toStdString() << std::endl;
        return tracks;
    }

    tracks.reserve(query.size() > 0 ? query.size() : 128);

    const QSqlRecord record = query.record();
    const int idxFilePath = record.indexOf("file_path");
    const int idxTitle = record.indexOf("title");
    const int idxArtist = record.indexOf("artist");
    const int idxAlbum = record.indexOf("album");
    const int idxGenre = record.indexOf("genre");
    const int idxYear = record.indexOf("year");
    const int idxDuration = record.indexOf("duration");
    const int idxBpm = record.indexOf("bpm");
    const int idxFileSize = record.indexOf("file_size");
    const int idxComment = record.indexOf("comment");
    const int idxTrackKey = record.indexOf("track_key");
    const int idxFileModified = record.indexOf("file_modified");
    const int idxAddedAt = record.indexOf("added_at");
    const int idxUpdatedAt = record.indexOf("updated_at");
    const int idxAnalyzedAt = record.indexOf("analyzed_at");
    const int idxAlgorithm = record.indexOf("analysis_algorithm");
    const int idxFirstBeat = record.indexOf("first_beat_offset");
    const int idxTrackLength = record.indexOf("track_length");
    const int idxBeatGrid = record.indexOf("beat_grid");
    const int idxAnalysisFailed = record.indexOf("analysis_failed");
    const int idxCuePoints = record.indexOf("cue_points");
    const int idxWaveformMaxBins = record.indexOf("waveform_max_bins");
    const int idxWaveformMinBins = record.indexOf("waveform_min_bins");
    const int idxWaveformAudioStartOffset = record.indexOf("waveform_audio_start_offset");
    const int idxWaveformAnalyzedAt = record.indexOf("waveform_analyzed_at");

    while (query.next())
    {
        TrackInfo track(query.value(idxFilePath).toString());
        track.title = query.value(idxTitle).toString();
        track.artist = query.value(idxArtist).toString();
        track.album = query.value(idxAlbum).toString();
        track.genre = query.value(idxGenre).toString();
        track.year = query.value(idxYear).toString();
        track.duration = query.value(idxDuration).toDouble();
        track.bpm = query.value(idxBpm).toDouble();
        track.fileSize = query.value(idxFileSize).toLongLong();
        track.comment = query.value(idxComment).toString();
        track.key = query.value(idxTrackKey).toString();
        track.lastModified = query.value(idxFileModified).toLongLong();
        track.addedAt = query.value(idxAddedAt).toLongLong();
        track.updatedAt = query.value(idxUpdatedAt).toLongLong();
        track.analyzedAt = query.value(idxAnalyzedAt).toLongLong();
        track.analysisAlgorithm = query.value(idxAlgorithm).toString();
        track.firstBeatOffset = query.value(idxFirstBeat).toDouble();
        track.trackLengthSeconds = query.value(idxTrackLength).toDouble();
        track.beatPositions = deserializeBeats(query.value(idxBeatGrid).toString());
        track.analysisFailed = query.value(idxAnalysisFailed).toInt() != 0;
        const QString cueText = query.value(idxCuePoints).toString();
        track.cuePoints = deserializeCuePoints(cueText);
        track.hasCuePoints = false;
        for (double c : track.cuePoints)
        {
            if (c >= 0.0)
            {
                track.hasCuePoints = true;
                break;
            }
        }
        // WAVEFORM CACHING: Load waveform bins if available
        track.waveformMaxBins = deserializeWaveformBins(query.value(idxWaveformMaxBins).toByteArray());
        track.waveformMinBins = deserializeWaveformBins(query.value(idxWaveformMinBins).toByteArray());
        track.waveformAudioStartOffset = query.value(idxWaveformAudioStartOffset).toDouble();
        track.waveformAnalyzedAt = query.value(idxWaveformAnalyzedAt).toLongLong();
        
        if (track.trackLengthSeconds <= 0.0)
            track.trackLengthSeconds = track.duration;
        if (track.duration <= 0.0 && track.trackLengthSeconds > 0.0)
            track.duration = track.trackLengthSeconds;
        tracks.push_back(track);
    }

    return tracks;
}

std::optional<TrackInfo> LibraryDatabase::loadTrackByPath(const QString& filePath) const
{
    if (!schemaReady)
        return std::nullopt;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return std::nullopt;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT file_path, title, artist, album, genre, year, duration, bpm, file_size, comment, track_key, file_modified, added_at, updated_at, "
        "analyzed_at, analysis_algorithm, first_beat_offset, track_length, beat_grid, analysis_failed, cue_points "
        "FROM tracks WHERE file_path = :file_path"));
    query.bindValue(QStringLiteral(":file_path"), filePath);

    if (!execOrLog(query, "loadTrackByPath"))
        return std::nullopt;

    const QSqlRecord record = query.record();
    const int idxFilePath = record.indexOf("file_path");
    const int idxTitle = record.indexOf("title");
    const int idxArtist = record.indexOf("artist");
    const int idxAlbum = record.indexOf("album");
    const int idxGenre = record.indexOf("genre");
    const int idxYear = record.indexOf("year");
    const int idxDuration = record.indexOf("duration");
    const int idxBpm = record.indexOf("bpm");
    const int idxFileSize = record.indexOf("file_size");
    const int idxComment = record.indexOf("comment");
    const int idxTrackKey = record.indexOf("track_key");
    const int idxFileModified = record.indexOf("file_modified");
    const int idxAddedAt = record.indexOf("added_at");
    const int idxUpdatedAt = record.indexOf("updated_at");
    const int idxAnalyzedAt = record.indexOf("analyzed_at");
    const int idxAlgorithm = record.indexOf("analysis_algorithm");
    const int idxFirstBeat = record.indexOf("first_beat_offset");
    const int idxTrackLength = record.indexOf("track_length");
    const int idxBeatGrid = record.indexOf("beat_grid");
    const int idxAnalysisFailed = record.indexOf("analysis_failed");
    const int idxCuePoints = record.indexOf("cue_points");

    if (!query.next())
        return std::nullopt;

    TrackInfo track(query.value(idxFilePath).toString());
    track.title = query.value(idxTitle).toString();
    track.artist = query.value(idxArtist).toString();
    track.album = query.value(idxAlbum).toString();
    track.genre = query.value(idxGenre).toString();
    track.year = query.value(idxYear).toString();
    track.duration = query.value(idxDuration).toDouble();
    track.bpm = query.value(idxBpm).toDouble();
    track.fileSize = query.value(idxFileSize).toLongLong();
    track.comment = query.value(idxComment).toString();
    track.key = query.value(idxTrackKey).toString();
    track.lastModified = query.value(idxFileModified).toLongLong();
    track.addedAt = query.value(idxAddedAt).toLongLong();
    track.updatedAt = query.value(idxUpdatedAt).toLongLong();
    track.analyzedAt = query.value(idxAnalyzedAt).toLongLong();
    track.analysisAlgorithm = query.value(idxAlgorithm).toString();
    track.firstBeatOffset = query.value(idxFirstBeat).toDouble();
    track.trackLengthSeconds = query.value(idxTrackLength).toDouble();
    track.beatPositions = deserializeBeats(query.value(idxBeatGrid).toString());
    track.analysisFailed = query.value(idxAnalysisFailed).toInt() != 0;
    const QString cueText = query.value(idxCuePoints).toString();
    track.cuePoints = deserializeCuePoints(cueText);
    track.hasCuePoints = false;
    for (double c : track.cuePoints)
    {
        if (c >= 0.0)
        {
            track.hasCuePoints = true;
            break;
        }
    }

    if (track.trackLengthSeconds <= 0.0)
        track.trackLengthSeconds = track.duration;
    if (track.duration <= 0.0 && track.trackLengthSeconds > 0.0)
        track.duration = track.trackLengthSeconds;

    return track;
}

bool LibraryDatabase::upsertTrack(const TrackInfo& track)
{
    if (!schemaReady)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QFileInfo fileInfo(track.filePath);
    const qint64 modified = fileInfo.exists() ? fileInfo.lastModified().toSecsSinceEpoch() : track.lastModified;

    QSqlQuery update(db);
    update.prepare(QStringLiteral(
        "UPDATE tracks SET title=:title, artist=:artist, album=:album, genre=:genre, year=:year, duration=:duration, bpm=:bpm, "
        "file_size=:file_size, comment=:comment, track_key=:track_key, file_modified=:file_modified, updated_at=:updated_at, "
        "analyzed_at=:analyzed_at, analysis_algorithm=:analysis_algorithm, first_beat_offset=:first_beat_offset, "
        "track_length=:track_length, beat_grid=:beat_grid, analysis_failed=:analysis_failed, cue_points=:cue_points "
        "WHERE file_path=:file_path"));
    update.bindValue(QStringLiteral(":title"), track.title);
    update.bindValue(QStringLiteral(":artist"), track.artist);
    update.bindValue(QStringLiteral(":album"), track.album);
    update.bindValue(QStringLiteral(":genre"), track.genre);
    update.bindValue(QStringLiteral(":year"), track.year);
    update.bindValue(QStringLiteral(":duration"), track.duration);
    update.bindValue(QStringLiteral(":bpm"), track.bpm);
    update.bindValue(QStringLiteral(":file_size"), track.fileSize);
    update.bindValue(QStringLiteral(":comment"), track.comment);
    update.bindValue(QStringLiteral(":track_key"), track.key);
    update.bindValue(QStringLiteral(":file_modified"), modified);
    update.bindValue(QStringLiteral(":updated_at"), now);
    update.bindValue(QStringLiteral(":analyzed_at"), track.analyzedAt);
    update.bindValue(QStringLiteral(":analysis_algorithm"), track.analysisAlgorithm);
    update.bindValue(QStringLiteral(":first_beat_offset"), track.firstBeatOffset);
    update.bindValue(QStringLiteral(":track_length"), track.trackLengthSeconds > 0.0 ? track.trackLengthSeconds : track.duration);
    update.bindValue(QStringLiteral(":beat_grid"), serializeBeats(track.beatPositions));
    update.bindValue(QStringLiteral(":analysis_failed"), track.analysisFailed ? 1 : 0);
    update.bindValue(QStringLiteral(":cue_points"), serializeCuePoints(track.cuePoints));
    update.bindValue(QStringLiteral(":file_path"), track.filePath);

    if (!execOrLog(update, "update track"))
        return false;

    if (update.numRowsAffected() > 0)
        return true;

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO tracks (file_path, file_name, title, artist, album, genre, year, duration, bpm, file_size, comment, track_key, "
        "file_modified, added_at, updated_at, analyzed_at, analysis_algorithm, first_beat_offset, track_length, beat_grid, analysis_failed, cue_points) "
        "VALUES (:file_path, :file_name, :title, :artist, :album, :genre, :year, :duration, :bpm, :file_size, :comment, :track_key, "
        ":file_modified, :added_at, :updated_at, :analyzed_at, :analysis_algorithm, :first_beat_offset, :track_length, :beat_grid, :analysis_failed, :cue_points)"));
    insert.bindValue(QStringLiteral(":file_path"), track.filePath);
    insert.bindValue(QStringLiteral(":file_name"), QFileInfo(track.filePath).fileName());
    insert.bindValue(QStringLiteral(":title"), track.title);
    insert.bindValue(QStringLiteral(":artist"), track.artist);
    insert.bindValue(QStringLiteral(":album"), track.album);
    insert.bindValue(QStringLiteral(":genre"), track.genre);
    insert.bindValue(QStringLiteral(":year"), track.year);
    insert.bindValue(QStringLiteral(":duration"), track.duration);
    insert.bindValue(QStringLiteral(":bpm"), track.bpm);
    insert.bindValue(QStringLiteral(":file_size"), track.fileSize);
    insert.bindValue(QStringLiteral(":comment"), track.comment);
    insert.bindValue(QStringLiteral(":track_key"), track.key);
    insert.bindValue(QStringLiteral(":file_modified"), modified);
    insert.bindValue(QStringLiteral(":added_at"), now);
    insert.bindValue(QStringLiteral(":updated_at"), now);
    insert.bindValue(QStringLiteral(":analyzed_at"), track.analyzedAt);
    insert.bindValue(QStringLiteral(":analysis_algorithm"), track.analysisAlgorithm);
    insert.bindValue(QStringLiteral(":first_beat_offset"), track.firstBeatOffset);
    insert.bindValue(QStringLiteral(":track_length"), track.trackLengthSeconds > 0.0 ? track.trackLengthSeconds : track.duration);
    insert.bindValue(QStringLiteral(":beat_grid"), serializeBeats(track.beatPositions));
    insert.bindValue(QStringLiteral(":analysis_failed"), track.analysisFailed ? 1 : 0);
    insert.bindValue(QStringLiteral(":cue_points"), serializeCuePoints(track.cuePoints));

    return execOrLog(insert, "insert track");
}

bool LibraryDatabase::removeTrack(const QString& filePath)
{
    if (!schemaReady)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM tracks WHERE file_path = :file_path"));
    query.bindValue(QStringLiteral(":file_path"), filePath);
    return execOrLog(query, "delete track");
}

bool LibraryDatabase::removeAllTracks()
{
    if (!schemaReady)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("DELETE FROM tracks")))
    {
    std::cerr << "LibraryDatabase removeAllTracks failed: "
          << query.lastError().text().toStdString() << std::endl;
        return false;
    }
    return true;
}

bool LibraryDatabase::vacuum()
{
    if (!schemaReady)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery query(db);
    return execOrLog(query, "VACUUM");
}

QVector<PlaylistRecord> LibraryDatabase::loadAllPlaylists() const
{
    QVector<PlaylistRecord> playlists;
    if (!schemaReady)
        return playlists;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return playlists;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT p.id, p.name, p.created_at, p.updated_at, p.sort_index, "
            "(SELECT COUNT(*) FROM playlist_tracks pt WHERE pt.playlist_id = p.id) AS track_count "
            "FROM playlists p ORDER BY p.sort_index ASC, p.name COLLATE NOCASE ASC")))
    {
        std::cerr << "LibraryDatabase loadAllPlaylists failed: "
                  << query.lastError().text().toStdString() << std::endl;
        return playlists;
    }

    while (query.next())
    {
        PlaylistRecord record;
        record.id = query.value(0).toInt();
        record.name = query.value(1).toString();
        record.createdAt = query.value(2).toLongLong();
        record.updatedAt = query.value(3).toLongLong();
        record.sortIndex = query.value(4).toInt();
        record.trackCount = query.value(5).toInt();
        playlists.append(record);
    }

    return playlists;
}

std::optional<PlaylistRecord> LibraryDatabase::loadPlaylist(int playlistId) const
{
    if (!schemaReady || playlistId <= 0)
        return std::nullopt;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return std::nullopt;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT p.id, p.name, p.created_at, p.updated_at, p.sort_index, "
        "(SELECT COUNT(*) FROM playlist_tracks pt WHERE pt.playlist_id = p.id) AS track_count "
        "FROM playlists p WHERE p.id = :id"));
    query.bindValue(QStringLiteral(":id"), playlistId);

    if (!execOrLog(query, "load playlist") || !query.next())
        return std::nullopt;

    PlaylistRecord record;
    record.id = query.value(0).toInt();
    record.name = query.value(1).toString();
    record.createdAt = query.value(2).toLongLong();
    record.updatedAt = query.value(3).toLongLong();
    record.sortIndex = query.value(4).toInt();
    record.trackCount = query.value(5).toInt();
    return record;
}

bool LibraryDatabase::createPlaylist(const QString& name, PlaylistRecord* outRecord)
{
    if (!schemaReady || name.trimmed().isEmpty())
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    if (!db.transaction())
    {
        std::cerr << "LibraryDatabase createPlaylist: failed to begin transaction" << std::endl;
        return false;
    }

    int sortIndex = 0;
    {
        QSqlQuery sortQuery(db);
        if (sortQuery.exec(QStringLiteral("SELECT COALESCE(MAX(sort_index), -1) + 1 FROM playlists")) && sortQuery.next())
            sortIndex = sortQuery.value(0).toInt();
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO playlists(name, created_at, updated_at, sort_index) "
        "VALUES (:name, :created, :updated, :sort)"));
    insert.bindValue(QStringLiteral(":name"), name.trimmed());
    insert.bindValue(QStringLiteral(":created"), now);
    insert.bindValue(QStringLiteral(":updated"), now);
    insert.bindValue(QStringLiteral(":sort"), sortIndex);

    if (!insert.exec())
    {
        std::cerr << "LibraryDatabase createPlaylist insert failed: "
                  << insert.lastError().text().toStdString() << std::endl;
        db.rollback();
        return false;
    }

    const int playlistId = insert.lastInsertId().toInt();

    if (!db.commit())
    {
        std::cerr << "LibraryDatabase createPlaylist: commit failed" << std::endl;
        db.rollback();
        return false;
    }

    if (outRecord)
    {
        outRecord->id = playlistId;
        outRecord->name = name.trimmed();
        outRecord->createdAt = now;
        outRecord->updatedAt = now;
        outRecord->sortIndex = sortIndex;
        outRecord->trackCount = 0;
    }

    return true;
}

bool LibraryDatabase::renamePlaylist(int playlistId, const QString& newName)
{
    if (!schemaReady || playlistId <= 0 || newName.trimmed().isEmpty())
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE playlists SET name = :name, updated_at = :updated WHERE id = :id"));
    query.bindValue(QStringLiteral(":name"), newName.trimmed());
    query.bindValue(QStringLiteral(":updated"), now);
    query.bindValue(QStringLiteral(":id"), playlistId);

    return execOrLog(query, "rename playlist");
}

bool LibraryDatabase::deletePlaylist(int playlistId)
{
    if (!schemaReady || playlistId <= 0)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM playlists WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), playlistId);
    return execOrLog(query, "delete playlist");
}

bool LibraryDatabase::updatePlaylistSortOrder(const QVector<int>& playlistIdsInOrder)
{
    if (!schemaReady)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    if (!db.transaction())
    {
        std::cerr << "LibraryDatabase updatePlaylistSortOrder: failed to begin transaction" << std::endl;
        return false;
    }

    QSqlQuery updateTemp(db);
    const int baseOffset = 1000000;
    for (int i = 0; i < playlistIdsInOrder.size(); ++i)
    {
        const int playlistId = playlistIdsInOrder.at(i);
        updateTemp.prepare(QStringLiteral("UPDATE playlists SET sort_index = :sort WHERE id = :id"));
        updateTemp.bindValue(QStringLiteral(":sort"), baseOffset + i);
        updateTemp.bindValue(QStringLiteral(":id"), playlistId);
        if (!updateTemp.exec())
        {
            std::cerr << "LibraryDatabase updatePlaylistSortOrder temp update failed: "
                      << updateTemp.lastError().text().toStdString() << std::endl;
            db.rollback();
            return false;
        }
    }

    QSqlQuery updateFinal(db);
    for (int i = 0; i < playlistIdsInOrder.size(); ++i)
    {
        const int playlistId = playlistIdsInOrder.at(i);
        updateFinal.prepare(QStringLiteral("UPDATE playlists SET sort_index = :sort WHERE id = :id"));
        updateFinal.bindValue(QStringLiteral(":sort"), i);
        updateFinal.bindValue(QStringLiteral(":id"), playlistId);
        if (!updateFinal.exec())
        {
            std::cerr << "LibraryDatabase updatePlaylistSortOrder final update failed: "
                      << updateFinal.lastError().text().toStdString() << std::endl;
            db.rollback();
            return false;
        }
    }

    if (!db.commit())
    {
        std::cerr << "LibraryDatabase updatePlaylistSortOrder: commit failed" << std::endl;
        db.rollback();
        return false;
    }

    return true;
}

QVector<PlaylistItemRecord> LibraryDatabase::loadPlaylistItems(int playlistId) const
{
    QVector<PlaylistItemRecord> items;
    if (!schemaReady || playlistId <= 0)
        return items;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return items;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT id, playlist_id, track_id, file_path, position, added_at "
        "FROM playlist_tracks WHERE playlist_id = :id ORDER BY position ASC"));
    query.bindValue(QStringLiteral(":id"), playlistId);

    if (!execOrLog(query, "load playlist items"))
        return items;

    while (query.next())
    {
        PlaylistItemRecord item;
        item.id = query.value(0).toInt();
        item.playlistId = query.value(1).toInt();
        item.trackId = query.value(2).toInt();
        item.filePath = query.value(3).toString();
        item.position = query.value(4).toInt();
        item.addedAt = query.value(5).toLongLong();
        items.append(item);
    }

    return items;
}

bool LibraryDatabase::addTrackToPlaylist(int playlistId, const QString& filePath, int insertPosition,
                                         PlaylistItemRecord* outItem)
{
    if (!schemaReady || playlistId <= 0 || filePath.isEmpty())
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    if (!db.transaction())
    {
        std::cerr << "LibraryDatabase addTrackToPlaylist: failed to begin transaction" << std::endl;
        return false;
    }

    int trackId = -1;
    {
        QSqlQuery trackQuery(db);
        trackQuery.prepare(QStringLiteral("SELECT id FROM tracks WHERE file_path = :path"));
        trackQuery.bindValue(QStringLiteral(":path"), filePath);
        if (!execOrLog(trackQuery, "lookup track id") || !trackQuery.next())
        {
            db.rollback();
            return false;
        }
        trackId = trackQuery.value(0).toInt();
    }

    int itemCount = 0;
    {
        QSqlQuery countQuery(db);
        countQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM playlist_tracks WHERE playlist_id = :id"));
        countQuery.bindValue(QStringLiteral(":id"), playlistId);
        if (execOrLog(countQuery, "count playlist items") && countQuery.next())
            itemCount = countQuery.value(0).toInt();
    }

    int position = insertPosition;
    if (position < 0 || position > itemCount)
        position = itemCount;

    if (position < itemCount)
    {
        QSqlQuery shift(db);
        shift.prepare(QStringLiteral(
            "UPDATE playlist_tracks SET position = position + 1 "
            "WHERE playlist_id = :id AND position >= :pos"));
        shift.bindValue(QStringLiteral(":id"), playlistId);
        shift.bindValue(QStringLiteral(":pos"), position);
        if (!shift.exec())
        {
            std::cerr << "LibraryDatabase addTrackToPlaylist shift failed: "
                      << shift.lastError().text().toStdString() << std::endl;
            db.rollback();
            return false;
        }
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO playlist_tracks (playlist_id, track_id, file_path, position, added_at) "
        "VALUES (:playlist_id, :track_id, :file_path, :position, :added_at)"));
    insert.bindValue(QStringLiteral(":playlist_id"), playlistId);
    insert.bindValue(QStringLiteral(":track_id"), trackId);
    insert.bindValue(QStringLiteral(":file_path"), filePath);
    insert.bindValue(QStringLiteral(":position"), position);
    insert.bindValue(QStringLiteral(":added_at"), now);

    if (!insert.exec())
    {
        std::cerr << "LibraryDatabase addTrackToPlaylist insert failed: "
                  << insert.lastError().text().toStdString() << std::endl;
        db.rollback();
        return false;
    }

    const int itemId = insert.lastInsertId().toInt();

    if (!db.commit())
    {
        std::cerr << "LibraryDatabase addTrackToPlaylist: commit failed" << std::endl;
        db.rollback();
        return false;
    }

    if (outItem)
    {
        outItem->id = itemId;
        outItem->playlistId = playlistId;
        outItem->trackId = trackId;
        outItem->filePath = filePath;
        outItem->position = position;
        outItem->addedAt = now;
    }

    return true;
}

bool LibraryDatabase::removePlaylistItem(int itemId)
{
    if (!schemaReady || itemId <= 0)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery lookup(db);
    lookup.prepare(QStringLiteral("SELECT playlist_id, position FROM playlist_tracks WHERE id = :id"));
    lookup.bindValue(QStringLiteral(":id"), itemId);

    if (!execOrLog(lookup, "lookup playlist item") || !lookup.next())
        return false;

    const int playlistId = lookup.value(0).toInt();
    const int position = lookup.value(1).toInt();

    if (!db.transaction())
    {
        std::cerr << "LibraryDatabase removePlaylistItem: failed to begin transaction" << std::endl;
        return false;
    }

    QSqlQuery remove(db);
    remove.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE id = :id"));
    remove.bindValue(QStringLiteral(":id"), itemId);
    if (!remove.exec())
    {
        std::cerr << "LibraryDatabase removePlaylistItem delete failed: "
                  << remove.lastError().text().toStdString() << std::endl;
        db.rollback();
        return false;
    }

    QSqlQuery shift(db);
    shift.prepare(QStringLiteral(
        "UPDATE playlist_tracks SET position = position - 1 "
        "WHERE playlist_id = :id AND position > :pos"));
    shift.bindValue(QStringLiteral(":id"), playlistId);
    shift.bindValue(QStringLiteral(":pos"), position);
    if (!shift.exec())
    {
        std::cerr << "LibraryDatabase removePlaylistItem shift failed: "
                  << shift.lastError().text().toStdString() << std::endl;
        db.rollback();
        return false;
    }

    if (!db.commit())
    {
        std::cerr << "LibraryDatabase removePlaylistItem: commit failed" << std::endl;
        db.rollback();
        return false;
    }

    return true;
}

bool LibraryDatabase::clearPlaylist(int playlistId)
{
    if (!schemaReady || playlistId <= 0)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id = :id"));
    query.bindValue(QStringLiteral(":id"), playlistId);
    return execOrLog(query, "clear playlist");
}

bool LibraryDatabase::reorderPlaylistItems(int playlistId, const QVector<int>& orderedItemIds)
{
    if (!schemaReady || playlistId <= 0)
        return false;

    if (orderedItemIds.isEmpty())
        return true;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    if (!db.transaction())
    {
        std::cerr << "LibraryDatabase reorderPlaylistItems: failed to begin transaction" << std::endl;
        return false;
    }

    const int baseOffset = 1000000;
    QSqlQuery update(db);

    for (int i = 0; i < orderedItemIds.size(); ++i)
    {
        update.prepare(QStringLiteral(
            "UPDATE playlist_tracks SET position = :pos WHERE id = :id AND playlist_id = :playlist"));
        update.bindValue(QStringLiteral(":pos"), baseOffset + i);
        update.bindValue(QStringLiteral(":id"), orderedItemIds.at(i));
        update.bindValue(QStringLiteral(":playlist"), playlistId);
        if (!update.exec())
        {
            std::cerr << "LibraryDatabase reorderPlaylistItems temp update failed: "
                      << update.lastError().text().toStdString() << std::endl;
            db.rollback();
            return false;
        }
    }

    for (int i = 0; i < orderedItemIds.size(); ++i)
    {
        update.prepare(QStringLiteral(
            "UPDATE playlist_tracks SET position = :pos WHERE id = :id AND playlist_id = :playlist"));
        update.bindValue(QStringLiteral(":pos"), i);
        update.bindValue(QStringLiteral(":id"), orderedItemIds.at(i));
        update.bindValue(QStringLiteral(":playlist"), playlistId);
        if (!update.exec())
        {
            std::cerr << "LibraryDatabase reorderPlaylistItems final update failed: "
                      << update.lastError().text().toStdString() << std::endl;
            db.rollback();
            return false;
        }
    }

    if (!db.commit())
    {
        std::cerr << "LibraryDatabase reorderPlaylistItems: commit failed" << std::endl;
        db.rollback();
        return false;
    }

    return true;
}

bool LibraryDatabase::ensureSchema()
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery userVersion(db);
    if (!userVersion.exec(QStringLiteral("PRAGMA user_version")))
    {
    std::cerr << "LibraryDatabase: failed to query schema version: "
          << userVersion.lastError().text().toStdString() << std::endl;
        return false;
    }

    int version = 0;
    if (userVersion.next())
        version = userVersion.value(0).toInt();

    bool needsVersionUpdate = false;

    if (version == 0)
    {
        if (!createSchema())
            return false;
        version = kSchemaVersion;
        needsVersionUpdate = true;
    }
    else if (version < kSchemaVersion)
    {
        while (version < kSchemaVersion)
        {
            if (version == 1)
            {
                const char* migrations[] = {
                    "ALTER TABLE tracks ADD COLUMN analyzed_at INTEGER DEFAULT 0",
                    "ALTER TABLE tracks ADD COLUMN analysis_algorithm TEXT",
                    "ALTER TABLE tracks ADD COLUMN first_beat_offset REAL DEFAULT 0",
                    "ALTER TABLE tracks ADD COLUMN track_length REAL DEFAULT 0",
                    "ALTER TABLE tracks ADD COLUMN beat_grid TEXT",
                    "ALTER TABLE tracks ADD COLUMN analysis_failed INTEGER DEFAULT 0"
                };

                for (const char* sql : migrations)
                {
                    QSqlQuery migrate(db);
                    if (!migrate.exec(QString::fromUtf8(sql)))
                    {
                        std::cerr << "LibraryDatabase: migration step failed: "
                                  << migrate.lastError().text().toStdString() << std::endl;
                        return false;
                    }
                }

                version = 2;
                needsVersionUpdate = true;
                continue;
            }

            if (version == 2)
            {
                QSqlQuery migrate(db);
                if (!migrate.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN cue_points TEXT")))
                {
                    std::cerr << "LibraryDatabase: migration to add cue_points failed: "
                              << migrate.lastError().text().toStdString() << std::endl;
                    return false;
                }
                version = 3;
                needsVersionUpdate = true;
                continue;
            }

            if (version == 3)
            {
                if (!ensurePlaylistTables(db))
                    return false;
                version = 4;
                needsVersionUpdate = true;
                continue;
            }

            if (version == 4)
            {
                // Add waveform caching support
                const char* migrations[] = {
                    "ALTER TABLE tracks ADD COLUMN waveform_max_bins BLOB",
                    "ALTER TABLE tracks ADD COLUMN waveform_min_bins BLOB",
                    "ALTER TABLE tracks ADD COLUMN waveform_audio_start_offset REAL DEFAULT 0",
                    "ALTER TABLE tracks ADD COLUMN waveform_analyzed_at INTEGER DEFAULT 0"
                };

                for (const char* sql : migrations)
                {
                    QSqlQuery migrate(db);
                    if (!migrate.exec(QString::fromUtf8(sql)))
                    {
                        std::cerr << "LibraryDatabase: waveform migration failed: "
                                  << migrate.lastError().text().toStdString() << std::endl;
                        return false;
                    }
                }
                
                version = 5;
                needsVersionUpdate = true;
                std::cout << "LibraryDatabase: migrated to schema v5 (waveform caching)" << std::endl;
                continue;
            }

            std::cerr << "LibraryDatabase: unsupported schema upgrade path from version "
                      << version << std::endl;
            return false;
        }
    }
    else if (version > kSchemaVersion)
    {
        std::cerr << "LibraryDatabase: database schema version " << version
                  << " is newer than supported " << kSchemaVersion << std::endl;
        return false;
    }

    if (needsVersionUpdate || version != kSchemaVersion)
    {
        QSqlQuery setVersion(db);
        if (!setVersion.exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion)))
        {
            std::cerr << "LibraryDatabase: failed to set schema version: "
                      << setVersion.lastError().text().toStdString() << std::endl;
            return false;
        }
    }

    return true;
}

bool LibraryDatabase::createSchema()
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isValid() || !db.isOpen())
        return false;

    QSqlQuery createTracks(db);
    if (!createTracks.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS tracks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "file_path TEXT NOT NULL UNIQUE,"
            "file_name TEXT NOT NULL,"
            "title TEXT,"
            "artist TEXT,"
            "album TEXT,"
            "genre TEXT,"
            "year TEXT,"
            "duration REAL DEFAULT 0,"
            "bpm REAL DEFAULT 0,"
            "file_size INTEGER DEFAULT 0,"
            "comment TEXT,"
            "track_key TEXT,"
            "file_modified INTEGER DEFAULT 0,"
            "added_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL,"
            "analyzed_at INTEGER DEFAULT 0,"
            "analysis_algorithm TEXT,"
            "first_beat_offset REAL DEFAULT 0,"
            "track_length REAL DEFAULT 0,"
            "beat_grid TEXT,"
            "analysis_failed INTEGER DEFAULT 0,"
            "cue_points TEXT,"
            "rating INTEGER DEFAULT 0,"
            "play_count INTEGER DEFAULT 0,"
            "waveform_max_bins BLOB,"
            "waveform_min_bins BLOB,"
            "waveform_audio_start_offset REAL DEFAULT 0,"
            "waveform_analyzed_at INTEGER DEFAULT 0"
            ")")))
    {
    std::cerr << "LibraryDatabase: failed to create tracks table: "
          << createTracks.lastError().text().toStdString() << std::endl;
        return false;
    }

    QSqlQuery idxPath(db);
    idxPath.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_tracks_path ON tracks(file_path)"));

    QSqlQuery idxArtist(db);
    idxArtist.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist COLLATE NOCASE)"));

    QSqlQuery idxTitle(db);
    idxTitle.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title COLLATE NOCASE)"));

    if (!ensurePlaylistTables(db))
        return false;

    return true;
}
