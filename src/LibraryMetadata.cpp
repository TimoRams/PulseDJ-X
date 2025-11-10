#include "LibraryManager.h"
#include "AudioFormatGuard.h"
#include "CoverArtExtractor.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QImage>

#include <cstring>
#include <memory>
#include <utility>

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

ID3LoaderThread::ID3LoaderThread (const QStringList& files,
                                  std::shared_ptr<juce::AudioFormatManager> formatManager,
                                  QObject* parent)
    : QThread (parent), filesToProcess (files), audioFormatManager (std::move (formatManager))
{
}

void ID3LoaderThread::run()
{
    int current = 0;
    const int total = filesToProcess.size();

    for (const QString& filePath : filesToProcess)
    {
        if (shouldStop)
            break;

        TrackInfo track = loadTrackInfo (filePath);
        emit trackLoaded (track);

        ++current;
        emit progressUpdated (current, total);

        msleep (1);
    }

    emit finished ();
}

TrackInfo ID3LoaderThread::loadTrackInfo (const QString& filePath)
{
    TrackInfo track (filePath);

    if (! audioFormatManager)
        return track;
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
                reader.reset (audioFormatManager->createReaderFor (audioFile));
            }

            if (reader)
            {
                if (reader->sampleRate > 0)
                {
                    track.duration = reader->lengthInSamples / reader->sampleRate;
                    track.trackLengthSeconds = track.duration;
                }
                
                if (reader->bitsPerSample > 0 && reader->sampleRate > 0 && reader->numChannels > 0)
                {
                    track.bitrate = static_cast<int>((reader->bitsPerSample * reader->sampleRate * reader->numChannels) / 1000);
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
    
    // COVER ART EXTRACTION: Extract embedded artwork using TagLib
    auto [coverData, coverFormat] = CoverArtExtractor::extractCoverArt(filePath);
    if (!coverData.isEmpty())
    {
        track.coverArtData = coverData;
        track.coverArtFormat = coverFormat;
    }

    applyId3v1Fallback (id3v1Tag, track);
    applyFilenameHeuristics (fileInfo, track);
    finaliseMetadata (fileInfo, track);

    return track;
}
