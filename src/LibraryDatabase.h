#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <optional>

class QSqlDatabase;
struct TrackInfo;

struct PlaylistRecord {
    int id = -1;
    QString name;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    int sortIndex = 0;
    int trackCount = 0;
};

struct PlaylistItemRecord {
    int id = -1;
    int playlistId = -1;
    int trackId = -1;
    QString filePath;
    int position = 0;
    qint64 addedAt = 0;
};

class LibraryDatabase : public QObject
{
    Q_OBJECT
public:
    explicit LibraryDatabase(QObject* parent = nullptr);
    ~LibraryDatabase() override;

    bool open(const QString& databasePath);
    QVector<TrackInfo> loadAllTracks() const;
    std::optional<TrackInfo> loadTrackByPath(const QString& filePath) const;
    bool upsertTrack(const TrackInfo& track);
    bool removeTrack(const QString& filePath);
    bool removeAllTracks();
    bool vacuum();

    QVector<PlaylistRecord> loadAllPlaylists() const;
    std::optional<PlaylistRecord> loadPlaylist(int playlistId) const;
    bool createPlaylist(const QString& name, PlaylistRecord* outRecord = nullptr);
    bool renamePlaylist(int playlistId, const QString& newName);
    bool deletePlaylist(int playlistId);
    bool updatePlaylistSortOrder(const QVector<int>& playlistIdsInOrder);
    QVector<PlaylistItemRecord> loadPlaylistItems(int playlistId) const;
    bool addTrackToPlaylist(int playlistId, const QString& filePath, int insertPosition = -1,
                            PlaylistItemRecord* outItem = nullptr);
    bool removePlaylistItem(int itemId);
    bool clearPlaylist(int playlistId);
    bool reorderPlaylistItems(int playlistId, const QVector<int>& orderedItemIds);

    QString databasePath() const { return dbPath; }

private:
    bool ensureSchema();
    bool createSchema();

    QString connectionName;
    QString dbPath;
    mutable bool schemaReady = false;
};
