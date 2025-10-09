#include "LibraryDatabase.h"

#include "LibraryManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>
#include <iostream>
#include <optional>

namespace
{
constexpr auto kConnectionPrefix = "LibraryConnection";
constexpr int kSchemaVersion = 2;

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
        "analyzed_at, analysis_algorithm, first_beat_offset, track_length, beat_grid, analysis_failed "
        "FROM tracks ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE, title COLLATE NOCASE")))
    {
    std::cerr << "LibraryDatabase loadAllTracks failed: "
          << query.lastError().text().toStdString() << std::endl;
        return tracks;
    }

    tracks.reserve(query.size() > 0 ? query.size() : 128);

    while (query.next())
    {
        TrackInfo track(query.value(0).toString());
        track.title = query.value(1).toString();
        track.artist = query.value(2).toString();
        track.album = query.value(3).toString();
        track.genre = query.value(4).toString();
        track.year = query.value(5).toString();
        track.duration = query.value(6).toDouble();
        track.bpm = query.value(7).toDouble();
        track.fileSize = query.value(8).toLongLong();
        track.comment = query.value(9).toString();
        track.key = query.value(10).toString();
        track.lastModified = query.value(11).toLongLong();
        track.addedAt = query.value(12).toLongLong();
        track.updatedAt = query.value(13).toLongLong();
        track.analyzedAt = query.value(14).toLongLong();
        track.analysisAlgorithm = query.value(15).toString();
        track.firstBeatOffset = query.value(16).toDouble();
        track.trackLengthSeconds = query.value(17).toDouble();
        track.beatPositions = deserializeBeats(query.value(18).toString());
        track.analysisFailed = query.value(19).toInt() != 0;
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
        "analyzed_at, analysis_algorithm, first_beat_offset, track_length, beat_grid, analysis_failed "
        "FROM tracks WHERE file_path = :file_path"));
    query.bindValue(QStringLiteral(":file_path"), filePath);

    if (!execOrLog(query, "loadTrackByPath"))
        return std::nullopt;

    if (!query.next())
        return std::nullopt;

    TrackInfo track(query.value(0).toString());
    track.title = query.value(1).toString();
    track.artist = query.value(2).toString();
    track.album = query.value(3).toString();
    track.genre = query.value(4).toString();
    track.year = query.value(5).toString();
    track.duration = query.value(6).toDouble();
    track.bpm = query.value(7).toDouble();
    track.fileSize = query.value(8).toLongLong();
    track.comment = query.value(9).toString();
    track.key = query.value(10).toString();
    track.lastModified = query.value(11).toLongLong();
    track.addedAt = query.value(12).toLongLong();
    track.updatedAt = query.value(13).toLongLong();
    track.analyzedAt = query.value(14).toLongLong();
    track.analysisAlgorithm = query.value(15).toString();
    track.firstBeatOffset = query.value(16).toDouble();
    track.trackLengthSeconds = query.value(17).toDouble();
    track.beatPositions = deserializeBeats(query.value(18).toString());
    track.analysisFailed = query.value(19).toInt() != 0;

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
        "track_length=:track_length, beat_grid=:beat_grid, analysis_failed=:analysis_failed "
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
    update.bindValue(QStringLiteral(":file_path"), track.filePath);

    if (!execOrLog(update, "update track"))
        return false;

    if (update.numRowsAffected() > 0)
        return true;

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO tracks (file_path, file_name, title, artist, album, genre, year, duration, bpm, file_size, comment, track_key, "
        "file_modified, added_at, updated_at, analyzed_at, analysis_algorithm, first_beat_offset, track_length, beat_grid, analysis_failed) "
        "VALUES (:file_path, :file_name, :title, :artist, :album, :genre, :year, :duration, :bpm, :file_size, :comment, :track_key, "
        ":file_modified, :added_at, :updated_at, :analyzed_at, :analysis_algorithm, :first_beat_offset, :track_length, :beat_grid, :analysis_failed)"));
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
        }
        else
        {
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
            "rating INTEGER DEFAULT 0,"
            "play_count INTEGER DEFAULT 0"
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

    return true;
}
