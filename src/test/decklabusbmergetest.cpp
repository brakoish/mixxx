// SPDX-License-Identifier: GPL-2.0-or-later
// r7-library: one library entry per song on a USB stick (rows at its earlier
// addresses move or merge onto its current one), rows that follow Get Music's
// renames, and tracks on sticks that are out hidden until they are back. Generated rows and fixture paths only: no
// stick, no audio, no personal paths.
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <string>

#include "control/controlobject.h"
#include "decklab/decklabmedia.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabusbrelink.h"
#include "library/basetrackcache.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/librarytablemodel.h"
#include "library/missing_hidden/missingtablemodel.h"
#include "library/playlisttablemodel.h"
#include "library/trackset/crate/cratetablemodel.h"
#include "proto/keys.pb.h"
#include "test/librarytest.h"
#include "track/cueinfo.h"
#include "widget/decklabhistory.h"

namespace {

using decklab::RelinkPlan;
using decklab::RelinkProbe;
using decklab::RelinkRow;
using mixxx::CueType;

const decklab::UsbVolume kStick{QStringLiteral("1234-ABCD"),
        QStringLiteral("MY STICK"),
        QStringLiteral("/media/decklab/1234-ABCD")};
const QString kMount = QStringLiteral("/media/decklab/1234-ABCD");
const QString kNow = QStringLiteral("/media/decklab/1234-ABCD/");
// The appliance's link /media/<user>/<label> to the stick's mount.
const QString kLinkFolder = QStringLiteral("/media/user/MY STICK");
const QString kLink = QStringLiteral("/media/user/MY STICK/");
// udisks before Deck Lab: the label taken, so "MY STICK1" (left empty).
const QString kSecondFolder = QStringLiteral("/media/user/MY STICK1");
const QString kSecond = QStringLiteral("/media/user/MY STICK1/");
// udisks before Deck Lab, for a stick without a label.
const QString kUuid = QStringLiteral("/media/user/1234-ABCD/");

// The filesystem as planning sees it.
struct FakeDisk {
    QHash<QString, qint64> files;
    QHash<QString, QString> links; // folder -> where it leads
    QSet<QString> in;              // folders with something in them

    bool isFolder(const QString& path) const {
        if (in.contains(path)) {
            return true;
        }
        const QString prefix = path + QLatin1Char('/');
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            if (it.key().startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }
    RelinkProbe probe() const {
        RelinkProbe probe;
        probe.fileSize = [files = files](const QString& path) {
            return files.value(path, -1);
        };
        probe.resolve = [disk = *this](const QString& path) {
            if (disk.links.contains(path)) {
                return disk.links.value(path);
            }
            return disk.files.contains(path) || disk.isFolder(path) ? path : QString();
        };
        probe.isIn = [in = in](const QString& path) {
            return in.contains(path);
        };
        return probe;
    }
};

// The stick is in at its appliance mount; MY STICK is the appliance's link.
FakeDisk stickWithLink() {
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.in.insert(kLinkFolder);
    disk.links.insert(kLinkFolder, kMount);
    return disk;
}

RelinkRow row(qint64 trackId, const QString& location, qint64 size, bool missing = false) {
    RelinkRow result;
    result.locationId = trackId + 100;
    result.trackId = trackId;
    result.location = location;
    result.fileSize = size;
    result.missing = missing;
    return result;
}

} // namespace

// --- Which folders were this stick ---

TEST(DeckLabUsbMergeTest, EarlierMountNamesAreTheLabelOrUuidWithUdisksNumber) {
    using decklab::isEarlierMountName;
    EXPECT_TRUE(isEarlierMountName(kStick, QStringLiteral("MY STICK")));
    EXPECT_TRUE(isEarlierMountName(kStick, QStringLiteral("MY STICK1")));
    EXPECT_TRUE(isEarlierMountName(kStick, QStringLiteral("MY STICK12")));
    EXPECT_TRUE(isEarlierMountName(kStick, QStringLiteral("1234-ABCD")));
    EXPECT_TRUE(isEarlierMountName(kStick, QStringLiteral("1234-abcd")));
    EXPECT_TRUE(isEarlierMountName(kStick, QStringLiteral("1234-ABCD1")));
    EXPECT_FALSE(isEarlierMountName(kStick, QStringLiteral("MY STICKS")));
    EXPECT_FALSE(isEarlierMountName(kStick, QStringLiteral("MY STICK 2")));
    EXPECT_FALSE(isEarlierMountName(kStick, QStringLiteral("my stick")));
    EXPECT_FALSE(isEarlierMountName(kStick, QStringLiteral("OTHER")));
    EXPECT_FALSE(isEarlierMountName(kStick, QStringLiteral("..")));
    const decklab::UsbVolume unlabeled{QStringLiteral("1234-ABCD"), QString(), kMount};
    EXPECT_FALSE(isEarlierMountName(unlabeled, QStringLiteral("1")));
    // After a label of digits only, a number would be a guess.
    const decklab::UsbVolume numbered{QStringLiteral("1234-ABCD"), QStringLiteral("2024"), kMount};
    EXPECT_TRUE(isEarlierMountName(numbered, QStringLiteral("2024")));
    EXPECT_FALSE(isEarlierMountName(numbered, QStringLiteral("20241")));
    EXPECT_EQ(QStringList{QStringLiteral("Contents/a.mp3")},
            decklab::legacyRelativePaths(kStick, kSecond + QStringLiteral("Contents/a.mp3")));
}

TEST(DeckLabUsbMergeTest, EveryEarlierAddressMovesToTheCurrentOne) {
    FakeDisk disk = stickWithLink();
    // A link to the mount counts whatever it is called.
    disk.links.insert(QStringLiteral("/media/user/RENAMED"), kMount);
    disk.in.insert(QStringLiteral("/media/user/RENAMED"));
    for (const char* name : {"a", "b", "c", "d", "e", "f"}) {
        disk.files.insert(kNow + QStringLiteral("Music/%1.mp3").arg(QLatin1String(name)), 1000);
    }
    const QList<RelinkRow> rows{
            row(1, kLink + QStringLiteral("Music/a.mp3"), 1000),
            row(2, kSecond + QStringLiteral("Music/b.mp3"), 1000, true),
            row(3, kUuid + QStringLiteral("Music/c.mp3"), 1000, true),
            row(4, QStringLiteral("/run/media/user/MY STICK/Music/d.mp3"), 1000, true),
            row(5, QStringLiteral("/media/MY STICK/Music/e.mp3"), 1000, true),
            row(6, QStringLiteral("/media/user/RENAMED/Music/f.mp3"), 1000),
            row(7, QStringLiteral("/home/user/Music/a.mp3"), 1000), // the unit's own music
    };
    const RelinkPlan plan = decklab::planRelink(kStick, rows, {}, disk.probe());
    ASSERT_EQ(6, plan.groups.size());
    for (const auto& group : plan.groups) {
        EXPECT_TRUE(group.relocate);
        EXPECT_TRUE(group.merged.isEmpty());
        EXPECT_EQ(group.target.section(QLatin1Char('/'), -1), group.survivor.location.section(QLatin1Char('/'), -1));
        EXPECT_TRUE(group.target.startsWith(kNow));
    }
    EXPECT_EQ(0, plan.missing + plan.sizeMismatch + plan.busy + plan.blocked);
}

TEST(DeckLabUsbMergeTest, AnotherStickWithTheSameLabelIsLeftAlone) {
    FakeDisk disk;
    disk.in.insert(kMount);
    // MY STICK is a real mount of another stick, not a link to this one.
    disk.in.insert(kLinkFolder);
    disk.files.insert(kNow + QStringLiteral("a.mp3"), 1000);
    disk.files.insert(kLink + QStringLiteral("a.mp3"), 1000);
    const QList<RelinkRow> rows{row(1, kLink + QStringLiteral("a.mp3"), 1000),
            row(2, kNow + QStringLiteral("a.mp3"), 1000)};
    EXPECT_TRUE(decklab::planRelink(kStick, rows, {}, disk.probe()).isEmpty());
}

TEST(DeckLabUsbMergeTest, SizeMismatchMissingFilesAndTracksInUseAreLeft) {
    FakeDisk disk = stickWithLink();
    disk.files.insert(kNow + QStringLiteral("a.mp3"), 1000);
    disk.files.insert(kNow + QStringLiteral("b.mp3"), 2000);
    disk.files.insert(kNow + QStringLiteral("c.mp3"), 3000);
    const QList<RelinkRow> rows{
            row(1, kNow + QStringLiteral("a.mp3"), 1000),
            row(2, kSecond + QStringLiteral("a.mp3"), 999, true), // another file: never merged
            row(3, kUuid + QStringLiteral("a.mp3"), 0, true),     // unknown size: never merged
            row(4, kSecond + QStringLiteral("gone.mp3"), 10, true), // not on the stick
            row(5, kNow + QStringLiteral("b.mp3"), 2000),
            row(6, kLink + QStringLiteral("b.mp3"), 2000), // a twin, but 5 is loaded
            row(7, kNow + QStringLiteral("c.mp3"), 2500),  // the file changed since
            row(8, kLink + QStringLiteral("c.mp3"), 3000),
    };
    const RelinkPlan plan = decklab::planRelink(kStick, rows, {}, disk.probe(), {5});
    EXPECT_TRUE(plan.groups.isEmpty());
    EXPECT_EQ(3, plan.sizeMismatch);
    EXPECT_EQ(1, plan.missing);
    EXPECT_EQ(1, plan.busy);
}

TEST(DeckLabUsbMergeTest, LibraryFoldersFollowTheStick) {
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.files.insert(kNow + QStringLiteral("Music/a.mp3"), 1000);
    const QStringList folders{QStringLiteral("/home/user/Music"),
            QStringLiteral("/media/user/1234-ABCD"),
            QStringLiteral("/media/user/MY STICK1/Music"),
            kNow + QStringLiteral("Deck Lab Downloads"),
            QStringLiteral("/media/user/5678-EF01")};
    const RelinkPlan plan = decklab::planRelink(kStick, {}, folders, disk.probe());
    ASSERT_EQ(2, plan.folders.size());
    EXPECT_EQ(QStringLiteral("/media/user/1234-ABCD"), plan.folders[0].first);
    EXPECT_EQ(kMount, plan.folders[0].second);
    EXPECT_EQ(QStringLiteral("/media/user/MY STICK1/Music"), plan.folders[1].first);
    EXPECT_EQ(kNow + QStringLiteral("Music"), plan.folders[1].second);
}

// --- On Mixxx's real schema ---

namespace {

class DeckLabUsbMergeDbTest : public LibraryTest {
  protected:
    void TearDown() override {
        decklab::MediaPresence::instance().setRootProbeForTesting({});
        decklab::history::Store::instance().reset();
    }
    QSqlDatabase database() const {
        return internalCollection()->database();
    }
    void exec(const QString& sql) {
        QSqlQuery query(database());
        EXPECT_TRUE(query.exec(sql)) << query.lastError().text().toStdString() << " " << sql.toStdString();
    }
    QVariant value(const QString& sql) {
        QSqlQuery query(database());
        EXPECT_TRUE(query.exec(sql)) << query.lastError().text().toStdString() << " " << sql.toStdString();
        return query.next() ? query.value(0) : QVariant();
    }
    int count(const QString& tableOrQuery) {
        return value(QStringLiteral("SELECT COUNT(*) FROM ") + tableOrQuery).toInt();
    }
    // A library row for a generated location (never read).
    qint64 addTrack(const QString& location, qint64 size, bool missing = false) {
        QSqlQuery query(database());
        query.prepare(QStringLiteral(
                "INSERT INTO track_locations (location, filename, directory, filesize, fs_deleted, "
                "needs_verification) VALUES (:location, :filename, :directory, :size, :missing, 0)"));
        const QFileInfo info(location);
        query.bindValue(QStringLiteral(":location"), location);
        query.bindValue(QStringLiteral(":filename"), info.fileName());
        query.bindValue(QStringLiteral(":directory"), info.path());
        query.bindValue(QStringLiteral(":size"), size);
        query.bindValue(QStringLiteral(":missing"), missing ? 1 : 0);
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        const QVariant locationId = query.lastInsertId();
        query.prepare(QStringLiteral(
                "INSERT INTO library (location, title, artist, mixxx_deleted, timesplayed, rating) "
                "VALUES (:location, :title, 'Generated', 0, 0, 0)"));
        query.bindValue(QStringLiteral(":location"), locationId);
        query.bindValue(QStringLiteral(":title"), info.completeBaseName());
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        return query.lastInsertId().toLongLong();
    }
    void addCue(qint64 track, CueType type, double position, double length, int hotcue) {
        QSqlQuery query(database());
        query.prepare(QStringLiteral(
                "INSERT INTO cues (track_id, type, position, length, hotcue, label) "
                "VALUES (:track, :type, :position, :length, :hotcue, '')"));
        query.bindValue(QStringLiteral(":track"), track);
        query.bindValue(QStringLiteral(":type"), static_cast<int>(type));
        query.bindValue(QStringLiteral(":position"), position);
        query.bindValue(QStringLiteral(":length"), length);
        query.bindValue(QStringLiteral(":hotcue"), hotcue);
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
    }
    qint64 addCrate(const QString& name) {
        QSqlQuery query(database());
        query.prepare(QStringLiteral("INSERT INTO crates (name) VALUES (:name)"));
        query.bindValue(QStringLiteral(":name"), name);
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        return query.lastInsertId().toLongLong();
    }
    void addToCrate(qint64 crate, qint64 track) {
        exec(QStringLiteral("INSERT INTO crate_tracks (crate_id, track_id) VALUES (%1, %2)").arg(crate).arg(track));
    }
    PlaylistDAO& playlists() const {
        return internalCollection()->getPlaylistDAO();
    }
    int history(const QString& name) {
        return playlists().createPlaylist(name, PlaylistDAO::PLHT_SET_LOG);
    }
    void append(int playlist, qint64 track) {
        EXPECT_TRUE(playlists().appendTrackToPlaylist(TrackId(QVariant(track)), playlist));
    }
    QString locationOf(qint64 track) {
        return value(QStringLiteral("SELECT track_locations.location FROM library INNER JOIN track_locations "
                                    "ON library.location=track_locations.id WHERE library.id=%1")
                        .arg(track))
                .toString();
    }
    bool hidden(qint64 track) {
        return value(QStringLiteral("SELECT mixxx_deleted FROM library WHERE id=%1").arg(track)).toInt() != 0;
    }
    bool missing(qint64 track) {
        return value(QStringLiteral("SELECT fs_deleted FROM track_locations INNER JOIN library "
                                    "ON library.location=track_locations.id WHERE library.id=%1")
                        .arg(track))
                       .toInt() != 0;
    }
    RelinkPlan plan(const FakeDisk& disk, const QSet<qint64>& busy = {}) {
        return decklab::planRelink(kStick,
                decklab::relinkCandidates(database()),
                decklab::libraryFolders(database()),
                disk.probe(),
                busy);
    }
    decklab::RelinkResult apply(const RelinkPlan& plan) {
        QSqlDatabase db = database();
        return decklab::applyRelink(db, plan);
    }
    // Every row of every table the merge touches.
    QList<int> totals() {
        return {count(QStringLiteral("library")),
                count(QStringLiteral("track_locations")),
                count(QStringLiteral("cues")),
                count(QStringLiteral("crate_tracks")),
                count(QStringLiteral("PlaylistTracks")),
                count(QStringLiteral("crates")),
                count(QStringLiteral("Playlists"))};
    }

    // One song twice: at the stick's current address, and through the
    // appliance's link, each with its own cues, crates, history and data.
    struct Twins {
        qint64 current = -1;
        qint64 linked = -1;
        qint64 crateA = -1; // linked only
        qint64 crateB = -1; // both
        qint64 crateC = -1; // current only
        int earlierSet = -1;
        int currentSet = -1;
        int playlist = -1;
    };
    Twins twins() {
        Twins t;
        t.current = addTrack(kNow + QStringLiteral("Contents/a.mp3"), 1000);
        t.linked = addTrack(kLink + QStringLiteral("Contents/a.mp3"), 1000);
        // Cues: the survivor keeps its own hot cue 0 and its saved loop; the
        // other row's hot cue 1, hot loop 2 and main cue move over.
        addCue(t.current, CueType::HotCue, 100, 0, 0);
        addCue(t.current, CueType::Loop, 400, 100, -1);
        addCue(t.linked, CueType::HotCue, 200, 0, 0);
        addCue(t.linked, CueType::HotCue, 300, 0, 1);
        addCue(t.linked, CueType::Loop, 500, 200, 2);
        addCue(t.linked, CueType::MainCue, 50, 0, -1);
        addCue(t.linked, CueType::Loop, 400, 100, -1);
        t.crateA = addCrate(QStringLiteral("Generated crate A"));
        t.crateB = addCrate(QStringLiteral("Generated crate B"));
        t.crateC = addCrate(QStringLiteral("Generated crate C"));
        addToCrate(t.crateA, t.linked);
        addToCrate(t.crateB, t.linked);
        addToCrate(t.crateB, t.current);
        addToCrate(t.crateC, t.current);
        t.earlierSet = history(QStringLiteral("2026-10-01"));
        append(t.earlierSet, t.linked);
        append(t.earlierSet, t.linked);
        t.currentSet = history(QStringLiteral("2026-10-23"));
        append(t.currentSet, t.current);
        t.playlist = playlists().createPlaylist(QStringLiteral("Generated playlist"), PlaylistDAO::PLHT_NOT_HIDDEN);
        append(t.playlist, t.linked);
        // Counts, dates, rating and colour; a locked grid and a key the DJ
        // set on the linked row, analysis on the current one.
        mixxx::track::io::key::KeyMap userKey;
        userKey.set_global_key(mixxx::track::io::key::A_MINOR);
        userKey.set_source(mixxx::track::io::key::USER);
        mixxx::track::io::key::KeyMap analysedKey;
        analysedKey.set_global_key(mixxx::track::io::key::C_MAJOR);
        analysedKey.set_source(mixxx::track::io::key::ANALYZER);
        QSqlQuery query(database());
        query.prepare(QStringLiteral(
                "UPDATE library SET timesplayed=:times, last_played_at=:last, datetime_added=:added, "
                "rating=:rating, color=:color, bpm=:bpm, beats=:beats, beats_version='BeatGrid-2.0', "
                "bpm_lock=:lock, key=:key, key_id=:keyid, keys=:keys, keys_version='KeyMap-1.0' "
                "WHERE id=:id"));
        const auto set = [&query](qint64 id, int times, const QString& last, const QString& added,
                                 int rating, const QVariant& color, double bpm, const QByteArray& beats,
                                 int lock, const QString& key, int keyId, const std::string& keys) {
            query.bindValue(QStringLiteral(":times"), times);
            query.bindValue(QStringLiteral(":last"), last);
            query.bindValue(QStringLiteral(":added"), added);
            query.bindValue(QStringLiteral(":rating"), rating);
            query.bindValue(QStringLiteral(":color"), color);
            query.bindValue(QStringLiteral(":bpm"), bpm);
            query.bindValue(QStringLiteral(":beats"), beats);
            query.bindValue(QStringLiteral(":lock"), lock);
            query.bindValue(QStringLiteral(":key"), key);
            query.bindValue(QStringLiteral(":keyid"), keyId);
            query.bindValue(QStringLiteral(":keys"), QByteArray::fromStdString(keys));
            query.bindValue(QStringLiteral(":id"), id);
            EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        };
        set(t.current, 2, QStringLiteral("2026-09-01 20:00:00"), QStringLiteral("2026-06-01 10:00:00"),
                0, QVariant(), 126.0, QByteArray("analysed"), 0, QStringLiteral("C"),
                static_cast<int>(mixxx::track::io::key::C_MAJOR), analysedKey.SerializeAsString());
        set(t.linked, 7, QStringLiteral("2026-10-01 21:00:00"), QStringLiteral("2026-01-01 10:00:00"),
                4, QVariant(0xff0000), 126.5, QByteArray("locked"), 1, QStringLiteral("Am"),
                static_cast<int>(mixxx::track::io::key::A_MINOR), userKey.SerializeAsString());
        return t;
    }
    FakeDisk twinsDisk() {
        FakeDisk disk = stickWithLink();
        disk.files.insert(kNow + QStringLiteral("Contents/a.mp3"), 1000);
        return disk;
    }
    // The merge, as the application runs it (copy first).
    decklab::RelinkResult mergeTwins(const Twins&) {
        const RelinkPlan merge = plan(twinsDisk());
        EXPECT_EQ(1, merge.groups.size());
        EXPECT_TRUE(decklab::backupLibraryDatabase(database()).has_value()); // in memory: nothing to copy
        const auto result = apply(merge);
        EXPECT_TRUE(result.ok);
        return result;
    }
};

} // namespace

TEST_F(DeckLabUsbMergeDbTest, MergeUnitesCuesWithoutTwins) {
    const Twins t = twins();
    const auto result = mergeTwins(t);
    ASSERT_EQ(1, result.merged.size());
    EXPECT_EQ(t.current, result.merged.first().survivor);
    EXPECT_EQ(t.linked, result.merged.first().hidden);
    EXPECT_EQ(3, result.cues);
    // The survivor: its own hot cue 0 and loop, plus hot cue 1, hot loop 2
    // and the main cue; one cue per slot, the loop once.
    EXPECT_EQ(5, count(QStringLiteral("cues WHERE track_id=%1").arg(t.current)));
    EXPECT_EQ(100, value(QStringLiteral("SELECT position FROM cues WHERE track_id=%1 AND hotcue=0").arg(t.current)).toInt());
    EXPECT_EQ(300, value(QStringLiteral("SELECT position FROM cues WHERE track_id=%1 AND hotcue=1").arg(t.current)).toInt());
    EXPECT_EQ(200, value(QStringLiteral("SELECT length FROM cues WHERE track_id=%1 AND hotcue=2").arg(t.current)).toInt());
    EXPECT_EQ(1, count(QStringLiteral("cues WHERE track_id=%1 AND type=%2").arg(t.current).arg(static_cast<int>(CueType::MainCue))));
    EXPECT_EQ(1, count(QStringLiteral("cues WHERE track_id=%1 AND type=%2 AND hotcue=-1").arg(t.current).arg(static_cast<int>(CueType::Loop))));
    // What it already had stays with the hidden row.
    EXPECT_EQ(2, count(QStringLiteral("cues WHERE track_id=%1").arg(t.linked)));
    // Play count, dates, rating, colour; the locked grid and the DJ's key.
    EXPECT_EQ(7, value(QStringLiteral("SELECT timesplayed FROM library WHERE id=%1").arg(t.current)).toInt());
    EXPECT_EQ(QStringLiteral("2026-10-01 21:00:00"),
            value(QStringLiteral("SELECT last_played_at FROM library WHERE id=%1").arg(t.current)).toString());
    EXPECT_EQ(QStringLiteral("2026-01-01 10:00:00"),
            value(QStringLiteral("SELECT datetime_added FROM library WHERE id=%1").arg(t.current)).toString());
    EXPECT_EQ(4, value(QStringLiteral("SELECT rating FROM library WHERE id=%1").arg(t.current)).toInt());
    EXPECT_EQ(0xff0000, value(QStringLiteral("SELECT color FROM library WHERE id=%1").arg(t.current)).toInt());
    EXPECT_DOUBLE_EQ(126.5, value(QStringLiteral("SELECT bpm FROM library WHERE id=%1").arg(t.current)).toDouble());
    EXPECT_EQ(1, value(QStringLiteral("SELECT bpm_lock FROM library WHERE id=%1").arg(t.current)).toInt());
    EXPECT_EQ(QByteArray("locked"), value(QStringLiteral("SELECT beats FROM library WHERE id=%1").arg(t.current)).toByteArray());
    EXPECT_EQ(static_cast<int>(mixxx::track::io::key::A_MINOR),
            value(QStringLiteral("SELECT key_id FROM library WHERE id=%1").arg(t.current)).toInt());
    // The survivor keeps its address; the other row is hidden where it was.
    EXPECT_EQ(kNow + QStringLiteral("Contents/a.mp3"), locationOf(t.current));
    EXPECT_EQ(kLink + QStringLiteral("Contents/a.mp3"), locationOf(t.linked));
    EXPECT_FALSE(hidden(t.current));
    EXPECT_TRUE(hidden(t.linked));
}

TEST_F(DeckLabUsbMergeDbTest, MergeUnitesCrates) {
    const Twins t = twins();
    const auto result = mergeTwins(t);
    EXPECT_EQ(1, result.crates);
    EXPECT_EQ(QSet<qint64>{t.crateA}, result.crateIds);
    for (const qint64 crate : {t.crateA, t.crateB, t.crateC}) {
        EXPECT_EQ(1, count(QStringLiteral("crate_tracks WHERE crate_id=%1 AND track_id=%2").arg(crate).arg(t.current)))
                << crate;
    }
    // Crate B held both: one entry for the song in its list.
    CrateTableModel crate(nullptr, trackCollectionManager());
    crate.selectCrate(CrateId(QVariant(t.crateB)));
    crate.select();
    EXPECT_EQ(1, crate.rowCount());
}

TEST_F(DeckLabUsbMergeDbTest, MergePointsHistoryAndPlaylistsAtTheSurvivor) {
    const Twins t = twins();
    const auto result = mergeTwins(t);
    EXPECT_EQ(3, result.entries);
    EXPECT_EQ((QSet<int>{t.earlierSet, t.playlist}), result.playlistIds);
    EXPECT_EQ(0, count(QStringLiteral("PlaylistTracks WHERE track_id=%1").arg(t.linked)));
    EXPECT_EQ(2, count(QStringLiteral("PlaylistTracks WHERE track_id=%1 AND playlist_id=%2").arg(t.current).arg(t.earlierSet)));
    EXPECT_EQ(1, count(QStringLiteral("PlaylistTracks WHERE track_id=%1 AND playlist_id=%2").arg(t.current).arg(t.playlist)));
    // Positions and times stay: the history reads as it was played.
    EXPECT_EQ(2, count(QStringLiteral("PlaylistTracks WHERE playlist_id=%1 AND position IN (1, 2)").arg(t.earlierSet)));
    // Deck Lab's history: both sets, the song under its one entry.
    auto& store = decklab::history::Store::instance();
    store.setBackend(decklab::history::makeMixxxBackend(trackCollectionManager(), UserSettingsPointer(),
            [] { return false; }, [](TrackId) { return 1; }));
    store.ensureLoaded();
    ASSERT_EQ(2, store.sessions().size());
    const auto earlier = store.entries(store.sessions().at(1).id);
    ASSERT_EQ(2, earlier.size());
    EXPECT_EQ(t.current, earlier.at(0).trackId);
    EXPECT_EQ(kNow + QStringLiteral("Contents/a.mp3"), earlier.at(1).location);
}

TEST_F(DeckLabUsbMergeDbTest, NothingIsDeleted) {
    const Twins t = twins();
    const QList<int> before = totals();
    mergeTwins(t);
    EXPECT_EQ(before, totals());
}

TEST_F(DeckLabUsbMergeDbTest, SecondRunChangesNothing) {
    const Twins t = twins();
    const qint64 alone = addTrack(kSecond + QStringLiteral("Contents/b.mp3"), 2000, true);
    exec(QStringLiteral("INSERT INTO directories (directory) VALUES ('/media/user/1234-ABCD')"));
    FakeDisk disk = twinsDisk();
    disk.files.insert(kNow + QStringLiteral("Contents/b.mp3"), 2000);
    const auto first = apply(plan(disk));
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(1, first.relocated.size());
    EXPECT_EQ(1, first.merged.size());
    EXPECT_EQ(1, first.folders);
    EXPECT_EQ(kNow + QStringLiteral("Contents/b.mp3"), locationOf(alone));
    const QList<int> after = totals();
    const QString timesPlayed = value(QStringLiteral("SELECT timesplayed FROM library WHERE id=%1").arg(t.current)).toString();
    const RelinkPlan again = plan(disk);
    EXPECT_TRUE(again.isEmpty());
    const auto second = apply(again);
    EXPECT_TRUE(second.ok);
    EXPECT_TRUE(second.relocated.isEmpty());
    EXPECT_TRUE(second.merged.isEmpty());
    EXPECT_EQ(after, totals());
    EXPECT_EQ(timesPlayed, value(QStringLiteral("SELECT timesplayed FROM library WHERE id=%1").arg(t.current)).toString());
}

TEST_F(DeckLabUsbMergeDbTest, SizeMismatchIsNotMerged) {
    const qint64 current = addTrack(kNow + QStringLiteral("a.mp3"), 1000);
    const qint64 other = addTrack(kSecond + QStringLiteral("a.mp3"), 999, true);
    addCue(other, CueType::HotCue, 10, 0, 0);
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.files.insert(kNow + QStringLiteral("a.mp3"), 1000);
    const RelinkPlan merge = plan(disk);
    EXPECT_TRUE(merge.isEmpty());
    EXPECT_EQ(1, merge.sizeMismatch);
    EXPECT_FALSE(hidden(other));
    EXPECT_EQ(kSecond + QStringLiteral("a.mp3"), locationOf(other));
    EXPECT_EQ(0, count(QStringLiteral("cues WHERE track_id=%1").arg(current)));
}

// udisks named the stick "MY STICK1" (the label was taken); that folder is
// empty now. A twin merges, a row alone moves, a file that is gone stays.
TEST_F(DeckLabUsbMergeDbTest, Label1AliasMovesAndMerges) {
    const qint64 current = addTrack(kNow + QStringLiteral("a.mp3"), 1000);
    const qint64 twin = addTrack(kSecond + QStringLiteral("a.mp3"), 1000, true);
    const qint64 alone = addTrack(kSecond + QStringLiteral("b.mp3"), 2000, true);
    const qint64 gone = addTrack(kSecond + QStringLiteral("c.mp3"), 3000, true);
    addCue(twin, CueType::HotCue, 10, 0, 3);
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.files.insert(kNow + QStringLiteral("a.mp3"), 1000);
    disk.files.insert(kNow + QStringLiteral("b.mp3"), 2000);
    const RelinkPlan merge = plan(disk);
    EXPECT_EQ(2, merge.groups.size());
    EXPECT_EQ(1, merge.missing);
    const auto result = apply(merge);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(hidden(twin));
    EXPECT_EQ(1, count(QStringLiteral("cues WHERE track_id=%1 AND hotcue=3").arg(current)));
    EXPECT_EQ(kNow + QStringLiteral("b.mp3"), locationOf(alone));
    EXPECT_FALSE(missing(alone));
    EXPECT_FALSE(hidden(alone));
    EXPECT_EQ(kSecond + QStringLiteral("c.mp3"), locationOf(gone));
    EXPECT_TRUE(missing(gone));
    EXPECT_FALSE(hidden(gone));
}

// The appliance's link still answers at the old address: same filesystem, so
// the row moves to the mount anyway (and its twin would merge).
TEST_F(DeckLabUsbMergeDbTest, SymlinkAliasMoves) {
    const qint64 linked = addTrack(kLink + QStringLiteral("Music/a.mp3"), 1000);
    FakeDisk disk = stickWithLink();
    disk.files.insert(kNow + QStringLiteral("Music/a.mp3"), 1000);
    disk.files.insert(kLink + QStringLiteral("Music/a.mp3"), 1000);
    const auto result = apply(plan(disk));
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(1, result.relocated.size());
    EXPECT_EQ(kNow + QStringLiteral("Music/a.mp3"), locationOf(linked));
    EXPECT_EQ(kNow + QStringLiteral("Music"),
            value(QStringLiteral("SELECT directory FROM track_locations WHERE location='%1'")
                            .arg(kNow + QStringLiteral("Music/a.mp3")))
                    .toString());
}

// udisks named a stick without a label after its FS UUID (any case); its
// library folder follows it too.
TEST_F(DeckLabUsbMergeDbTest, UuidPathAliasMovesWithItsLibraryFolder) {
    const qint64 upper = addTrack(kUuid + QStringLiteral("Music/a.mp3"), 1000, true);
    const qint64 lower = addTrack(QStringLiteral("/run/media/user/1234-abcd/Music/b.mp3"), 2000, true);
    exec(QStringLiteral("INSERT INTO directories (directory) VALUES ('/media/user/1234-ABCD')"));
    exec(QStringLiteral("INSERT INTO directories (directory) VALUES ('/home/user/Music')"));
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.files.insert(kNow + QStringLiteral("Music/a.mp3"), 1000);
    disk.files.insert(kNow + QStringLiteral("Music/b.mp3"), 2000);
    const auto result = apply(plan(disk));
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(2, result.relocated.size());
    EXPECT_EQ(kNow + QStringLiteral("Music/a.mp3"), locationOf(upper));
    EXPECT_EQ(kNow + QStringLiteral("Music/b.mp3"), locationOf(lower));
    EXPECT_EQ(1, result.folders);
    QStringList folders = decklab::libraryFolders(database());
    folders.sort();
    EXPECT_EQ((QStringList{QStringLiteral("/home/user/Music"), kMount}), folders);
}

// The copy beside the library file (the tests' library lives in memory: a
// database file of its own here). The newest two are kept.
TEST(DeckLabUsbMergeTest, BackupKeepsTheNewestTwo) {
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    const QString file = folder.filePath(QStringLiteral("library.sqlite"));
    QStringList copies;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("decklab-backup"));
        database.setDatabaseName(file);
        ASSERT_TRUE(database.open());
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("CREATE TABLE generated (value INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral("INSERT INTO generated (value) VALUES (42)")));
        for (int n = 0; n < 3; ++n) {
            const auto copy = decklab::backupLibraryDatabase(database);
            ASSERT_TRUE(copy.has_value());
            ASSERT_FALSE(copy->isEmpty());
            copies << *copy;
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("decklab-backup"));
    EXPECT_TRUE(copies.at(0).startsWith(file + QStringLiteral(".decklab-merge-")));
    EXPECT_FALSE(QFileInfo::exists(copies.at(0)));
    EXPECT_TRUE(QFileInfo::exists(copies.at(1)));
    EXPECT_TRUE(QFileInfo::exists(copies.at(2)));
    // A real copy of the library.
    {
        QSqlDatabase copy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("decklab-backup-copy"));
        copy.setDatabaseName(copies.at(2));
        ASSERT_TRUE(copy.open());
        QSqlQuery query(copy);
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT value FROM generated")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(42, query.value(0).toInt());
        copy.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("decklab-backup-copy"));
}

// The session glue: nothing while a deck plays; the stick's turn comes once
// nothing does.
TEST_F(DeckLabUsbMergeDbTest, RelinkerWaitsWhileAnythingPlays) {
    const qint64 alone = addTrack(kSecond + QStringLiteral("b.mp3"), 2000, true);
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.files.insert(kNow + QStringLiteral("b.mp3"), 2000);
    decklab::UsbRelinker relinker(std::shared_ptr<TrackCollectionManager>(
            trackCollectionManager(), [](TrackCollectionManager*) {}));
    relinker.setProbe(disk.probe());
    bool playing = true;
    relinker.setQuiet([&playing] { return !playing; });
    QSignalSpy done(&relinker, &decklab::UsbRelinker::relinked);
    QJsonArray drives;
    drives.append(QJsonObject{{"id", kStick.uuid}, {"label", kStick.label}, {"mount", kMount}});
    relinker.relinkDrives(drives);
    EXPECT_FALSE(relinker.isRunning());
    EXPECT_FALSE(done.wait(200));
    EXPECT_EQ(kSecond + QStringLiteral("b.mp3"), locationOf(alone));
    playing = false;
    relinker.relinkDrives(drives);
    ASSERT_TRUE(done.wait(5000));
    EXPECT_EQ(1, done.first().at(0).toInt());
    EXPECT_EQ(kNow + QStringLiteral("b.mp3"), locationOf(alone));
}

// r7: one rule for every background job (decklab/decklabquiet.h): nothing
// plays or records, Mixxx's library scan is not running and no utility window
// is open. The relinker's merge and the rescan after a utility both wait for
// it, and run as soon as it is quiet.
TEST_F(DeckLabUsbMergeDbTest, BackgroundJobsWaitUntilItIsQuiet) {
    ControlObject play(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("play")));
    ControlObject recording(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("status")));
    QTemporaryDir run;
    DeckLabSystem::Services services;
    services.config = config();
    services.library = std::shared_ptr<TrackCollectionManager>(trackCollectionManager(), [](TrackCollectionManager*) {});
    DeckLabSystem system(services, nullptr, run.filePath(QStringLiteral("decklabd.sock")));
    system.utilities()->setStateDirectory(run.filePath(QStringLiteral("decklab")));
    int scans = 0;
    DeckLabUtilities::Hooks hooks;
    hooks.quiet = [&system] { return system.quiet(); };
    hooks.startQuietScan = [&scans] {
        ++scans;
        return true;
    };
    system.utilities()->setHooks(hooks);
    EXPECT_TRUE(system.quiet());
    play.set(1);
    EXPECT_TRUE(system.activity().playing);
    EXPECT_FALSE(system.quiet());
    play.set(0);
    recording.set(2);
    EXPECT_TRUE(system.activity().recording);
    EXPECT_FALSE(system.quiet());
    recording.set(1); // ready, not recording
    EXPECT_TRUE(system.quiet());
    recording.set(0);
    emit trackCollectionManager()->libraryScanStarted();
    EXPECT_TRUE(system.activity().scanning);
    EXPECT_FALSE(system.quiet());
    emit trackCollectionManager()->libraryScanFinished();
    EXPECT_TRUE(system.quiet());
    system.utilities()->onAppsChanged(QJsonObject{{QStringLiteral("app"), QStringLiteral("get-music")}});
    EXPECT_TRUE(system.activity().utilityOpen);
    EXPECT_FALSE(system.quiet());

    // A stick while Get Music is open: its merge waits.
    const qint64 alone = addTrack(kSecond + QStringLiteral("b.mp3"), 2000, true);
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.files.insert(kNow + QStringLiteral("b.mp3"), 2000);
    decklab::UsbRelinker relinker(services.library);
    relinker.setProbe(disk.probe());
    relinker.setQuiet([&system] { return system.quiet(); });
    QSignalSpy done(&relinker, &decklab::UsbRelinker::relinked);
    QJsonArray drives;
    drives.append(QJsonObject{{"id", kStick.uuid}, {"label", kStick.label}, {"mount", kMount}});
    relinker.relinkDrives(drives);
    EXPECT_FALSE(relinker.isRunning());
    EXPECT_FALSE(done.wait(200));
    EXPECT_EQ(kSecond + QStringLiteral("b.mp3"), locationOf(alone));

    // Get Music closes while a deck plays: the rescan waits for the deck.
    play.set(1);
    system.utilities()->onAppsChanged(QJsonObject{{QStringLiteral("app"), QJsonValue::Null}});
    EXPECT_TRUE(system.utilities()->refreshPending());
    QTest::qWait(DeckLabUtilities::kQuietRetryMs * 2 + 200);
    EXPECT_EQ(0, scans) << "never while a deck plays";
    play.set(0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return scans == 1; }, DeckLabUtilities::kQuietRetryMs * 3));
    EXPECT_FALSE(system.utilities()->refreshPending());
    // ... and the merge runs once it is quiet too.
    relinker.relinkDrives(drives);
    ASSERT_TRUE(done.wait(5000));
    EXPECT_EQ(kNow + QStringLiteral("b.mp3"), locationOf(alone));
}

// r7: Mixxx starts its library scan before the skin loads, so Deck Lab never
// hears that one start. The scanner's own state counts: nothing runs beside
// the start-up scan, and the relinker's start-up pass follows its end.
TEST_F(DeckLabUsbMergeDbTest, TheStartUpScanCountsThoughItBeganFirst) {
    struct Reset {
        TrackCollectionManager* library;
        ~Reset() {
            library->setLibraryScanRunningForTesting(false);
        }
    } reset{trackCollectionManager()};
    trackCollectionManager()->setLibraryScanRunningForTesting(true); // started, its signal long gone
    QTemporaryDir run;
    DeckLabSystem::Services services;
    services.config = config();
    services.library = std::shared_ptr<TrackCollectionManager>(trackCollectionManager(), [](TrackCollectionManager*) {});
    DeckLabSystem system(services, nullptr, run.filePath(QStringLiteral("decklabd.sock")));
    EXPECT_TRUE(system.activity().scanning);
    EXPECT_FALSE(system.quiet());

    const qint64 alone = addTrack(kSecond + QStringLiteral("b.mp3"), 2000, true);
    FakeDisk disk;
    disk.in.insert(kMount);
    disk.files.insert(kNow + QStringLiteral("b.mp3"), 2000);
    decklab::UsbRelinker relinker(services.library);
    relinker.setProbe(disk.probe());
    QSignalSpy done(&relinker, &decklab::UsbRelinker::relinked);
    QJsonArray drives;
    drives.append(QJsonObject{{"id", kStick.uuid}, {"label", kStick.label}, {"mount", kMount}});
    relinker.relinkDrives(drives);
    EXPECT_FALSE(relinker.isRunning()) << "never beside the scan's write transaction";
    EXPECT_FALSE(done.wait(200));
    EXPECT_EQ(kSecond + QStringLiteral("b.mp3"), locationOf(alone));

    // The scan ends: that signal does come.
    trackCollectionManager()->setLibraryScanRunningForTesting(false);
    emit trackCollectionManager()->libraryScanFinished();
    EXPECT_FALSE(system.activity().scanning);
    EXPECT_TRUE(system.quiet());
    ASSERT_TRUE(done.wait(5000));
    EXPECT_EQ(kNow + QStringLiteral("b.mp3"), locationOf(alone));
}

// --- Get Music's rename maps ---

namespace {

const QString kNewMusic = QStringLiteral("/home/user/Music/New Music");

QByteArray renameMap(const QList<decklab::RenameEntry>& entries) {
    QJsonArray list;
    for (const auto& entry : entries) {
        list.append(QJsonObject{{"old", entry.oldName}, {"new", entry.newName}, {"size", entry.size}});
    }
    return QJsonDocument(list).toJson();
}

} // namespace

TEST(DeckLabRenameMapTest, MalformedMapsAndEntriesAreIgnored) {
    using decklab::parseRenameMap;
    using decklab::RenameEntry;
    EXPECT_FALSE(parseRenameMap(QByteArray("{not json")).has_value());
    EXPECT_FALSE(parseRenameMap(QByteArray("{\"old\": \"a.mp3\"}")).has_value());
    EXPECT_FALSE(parseRenameMap(QByteArray()).has_value());
    ASSERT_TRUE(parseRenameMap(QByteArray("[]")).has_value());
    EXPECT_TRUE(parseRenameMap(QByteArray("[]"))->isEmpty());
    const QByteArray mixed(R"([
        {"old": "a.mp3", "new": "01 a.mp3", "size": 1000},
        {"old": "../b.mp3", "new": "02 b.mp3", "size": 1000},
        {"old": "c.mp3", "new": "sub/03 c.mp3", "size": 1000},
        {"old": "d.mp3", "new": "04 d.mp3"},
        {"old": "e.mp3", "new": "05 e.mp3", "size": "1000"},
        {"old": "f.mp3", "new": "06 f.mp3", "size": -5},
        {"old": "g.mp3", "new": "07 g.mp3", "size": 1.5},
        {"old": "h.mp3", "new": "h.mp3", "size": 1000},
        {"old": 8, "new": "08 i.mp3", "size": 1000},
        "j.mp3",
        {"old": "k.mp3", "new": "..", "size": 1000},
        {"old": "l.mp3", "new": "09 l.mp3", "size": 2000}
    ])");
    const auto entries = parseRenameMap(mixed);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ((QList<RenameEntry>{{QStringLiteral("a.mp3"), QStringLiteral("01 a.mp3"), 1000},
                      {QStringLiteral("l.mp3"), QStringLiteral("09 l.mp3"), 2000}}),
            *entries);
    // Only the newest entries are read.
    const auto newest = parseRenameMap(mixed, 1);
    ASSERT_TRUE(newest.has_value());
    ASSERT_EQ(1, newest->size());
    EXPECT_EQ(QStringLiteral("l.mp3"), newest->first().oldName);
}

TEST(DeckLabRenameMapTest, MapsAreLookedForWhereLibraryRowsAre) {
    const QList<RelinkRow> rows{
            row(1, kNewMusic + QStringLiteral("/a.mp3"), 1000),
            row(2, kNewMusic + QStringLiteral("/Generated Playlist/b.mp3"), 1000),
            row(3, kNow + QStringLiteral("Deck Lab Downloads/Generated Playlist/c.mp3"), 1000),
            row(4, QStringLiteral("/home/user/Elsewhere/d.mp3"), 1000), // not in a library folder
    };
    const QStringList folders{QStringLiteral("/home/user/Music"), kNow + QStringLiteral("Deck Lab Downloads/")};
    EXPECT_EQ((QStringList{QStringLiteral("/home/user/Music"),
                      kNewMusic,
                      kNewMusic + QStringLiteral("/Generated Playlist"),
                      kNow + QStringLiteral("Deck Lab Downloads"),
                      kNow + QStringLiteral("Deck Lab Downloads/Generated Playlist")}),
            decklab::renameMapFolders(rows, folders));
}

TEST(DeckLabRenameMapTest, RowsFollowRenamesAndRenamesInARow) {
    const QString in = kNewMusic + QLatin1Char('/');
    RelinkRow hidden = row(4, in + QStringLiteral("d.mp3"), 4000);
    hidden.hidden = true;
    const QList<RelinkRow> rows{
            row(1, in + QStringLiteral("a.mp3"), 1000),
            row(2, in + QStringLiteral("b.mp3"), 2000),
            row(3, in + QStringLiteral("c.mp3"), 3000),
            hidden,
            row(5, in + QStringLiteral("e.mp3"), 5000), // its file is still there
            row(6, in + QStringLiteral("f.mp3"), 6000), // the map names another file
    };
    decklab::RenameMaps maps;
    maps.insert(kNewMusic,
            {{QStringLiteral("a.mp3"), QStringLiteral("01 a.mp3"), 1000},
                    {QStringLiteral("b.mp3"), QStringLiteral("02 b.mp3"), 2000},
                    {QStringLiteral("02 b.mp3"), QStringLiteral("03 b.mp3"), 2000},
                    {QStringLiteral("c.mp3"), QStringLiteral("04 c.mp3"), 3000},
                    {QStringLiteral("d.mp3"), QStringLiteral("05 d.mp3"), 4000},
                    {QStringLiteral("e.mp3"), QStringLiteral("06 e.mp3"), 5000},
                    {QStringLiteral("f.mp3"), QStringLiteral("07 f.mp3"), 1234}});
    FakeDisk disk;
    disk.files.insert(in + QStringLiteral("01 a.mp3"), 1000);
    disk.files.insert(in + QStringLiteral("03 b.mp3"), 2000);
    disk.files.insert(in + QStringLiteral("04 c.mp3"), 2999); // changed since: not this file
    disk.files.insert(in + QStringLiteral("05 d.mp3"), 4000);
    disk.files.insert(in + QStringLiteral("e.mp3"), 5000);
    disk.files.insert(in + QStringLiteral("06 e.mp3"), 5000);
    disk.files.insert(in + QStringLiteral("07 f.mp3"), 1234);
    const RelinkPlan plan = decklab::planRenames(rows, maps, disk.probe());
    ASSERT_EQ(2, plan.groups.size());
    EXPECT_EQ(2, plan.renamed);
    EXPECT_EQ(1, plan.groups[0].survivor.trackId);
    EXPECT_EQ(in + QStringLiteral("01 a.mp3"), plan.groups[0].target);
    EXPECT_TRUE(plan.groups[0].relocate);
    EXPECT_EQ(2, plan.groups[1].survivor.trackId);
    EXPECT_EQ(in + QStringLiteral("03 b.mp3"), plan.groups[1].target);
    EXPECT_EQ(1, plan.sizeMismatch);
    // A track in use waits.
    EXPECT_EQ(1, decklab::planRenames(rows, maps, disk.probe(), {2}).busy);
}

namespace {

class DeckLabRenameDbTest : public DeckLabUsbMergeDbTest {
  protected:
    // A rename planned and applied on generated rows.
    decklab::RelinkResult applyRenames(const decklab::RenameMaps& maps, const FakeDisk& disk) {
        const RelinkPlan renames = decklab::planRenames(decklab::libraryRows(database()), maps, disk.probe());
        last = renames;
        return apply(renames);
    }
    RelinkPlan last;
};

} // namespace

// The real thing: a folder of the library, a file Get Music renamed and its
// map beside it; the relinker moves the row in place. Cues, crates and
// history stay with it; the map is only read.
TEST_F(DeckLabRenameDbTest, InPlaceMoveKeepsCuesCratesAndHistory) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = root.filePath(QStringLiteral("New Music/Generated Playlist"));
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString renamed = folder + QStringLiteral("/01 Generated Tone.mp3");
    {
        QFile file(renamed);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(4096, '\x5a'));
    }
    const QString mapFile = folder + QLatin1Char('/') + decklab::kRenameMapName;
    {
        QFile file(mapFile);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(renameMap({{QStringLiteral("Generated Tone.mp3"), QStringLiteral("01 Generated Tone.mp3"), 4096}}));
    }
    const QByteArray mapBytes = [&mapFile] {
        QFile file(mapFile);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    exec(QStringLiteral("INSERT INTO directories (directory) VALUES ('%1')").arg(root.path()));
    const qint64 track = addTrack(folder + QStringLiteral("/Generated Tone.mp3"), 4096);
    addCue(track, CueType::HotCue, 100, 0, 0);
    addCue(track, CueType::Loop, 400, 100, 1);
    const qint64 crate = addCrate(QStringLiteral("Generated crate"));
    addToCrate(crate, track);
    const int set = history(QStringLiteral("2026-10-23"));
    append(set, track);
    const QList<int> before = totals();

    decklab::UsbRelinker relinker(std::shared_ptr<TrackCollectionManager>(
            trackCollectionManager(), [](TrackCollectionManager*) {}));
    QSignalSpy done(&relinker, &decklab::UsbRelinker::relinked);
    relinker.relinkRenames();
    ASSERT_TRUE(done.wait(5000));
    EXPECT_EQ(1, done.first().at(0).toInt());
    EXPECT_EQ(renamed, locationOf(track));
    EXPECT_FALSE(missing(track));
    EXPECT_EQ(folder, value(QStringLiteral("SELECT directory FROM track_locations WHERE location='%1'").arg(renamed)).toString());
    EXPECT_EQ(QStringLiteral("01 Generated Tone.mp3"),
            value(QStringLiteral("SELECT filename FROM track_locations WHERE location='%1'").arg(renamed)).toString());
    EXPECT_EQ(2, count(QStringLiteral("cues WHERE track_id=%1").arg(track)));
    EXPECT_EQ(1, count(QStringLiteral("crate_tracks WHERE crate_id=%1 AND track_id=%2").arg(crate).arg(track)));
    EXPECT_EQ(1, count(QStringLiteral("PlaylistTracks WHERE playlist_id=%1 AND track_id=%2").arg(set).arg(track)));
    EXPECT_EQ(before, totals());
    // The map was only read.
    QFile file(mapFile);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(mapBytes, file.readAll());
}

TEST_F(DeckLabRenameDbTest, SizeMismatchIsIgnored) {
    const qint64 track = addTrack(kNewMusic + QStringLiteral("/a.mp3"), 1000);
    decklab::RenameMaps maps;
    maps.insert(kNewMusic, {{QStringLiteral("a.mp3"), QStringLiteral("01 a.mp3"), 1000}});
    FakeDisk disk;
    disk.files.insert(kNewMusic + QStringLiteral("/01 a.mp3"), 1001); // not the file the map named
    const auto result = applyRenames(maps, disk);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(last.groups.isEmpty());
    EXPECT_EQ(1, last.sizeMismatch);
    EXPECT_EQ(kNewMusic + QStringLiteral("/a.mp3"), locationOf(track));
    // A map that names another size than the row recorded: not this row.
    maps[kNewMusic].first().size = 1001;
    applyRenames(maps, disk);
    EXPECT_TRUE(last.groups.isEmpty());
    EXPECT_EQ(kNewMusic + QStringLiteral("/a.mp3"), locationOf(track));
}

// The library scan saw the new name first and added a row there: the DJ's
// row keeps its id, cues, crates and history and takes over the address;
// the scan's row is hidden at the old one with what the DJ's row already had.
TEST_F(DeckLabRenameDbTest, ARowAlreadyAtTheNewNameIsMerged) {
    const qint64 track = addTrack(kNewMusic + QStringLiteral("/a.mp3"), 1000, true);
    const qint64 scanned = addTrack(kNewMusic + QStringLiteral("/01 a.mp3"), 1000);
    addCue(track, CueType::HotCue, 100, 0, 0);
    addCue(scanned, CueType::MainCue, 20, 0, -1);
    const qint64 crate = addCrate(QStringLiteral("Generated crate"));
    addToCrate(crate, track);
    const int set = history(QStringLiteral("2026-10-23"));
    append(set, track);
    exec(QStringLiteral("UPDATE library SET timesplayed=3 WHERE id=%1").arg(track));
    const QList<int> before = totals();
    decklab::RenameMaps maps;
    maps.insert(kNewMusic, {{QStringLiteral("a.mp3"), QStringLiteral("01 a.mp3"), 1000}});
    FakeDisk disk;
    disk.files.insert(kNewMusic + QStringLiteral("/01 a.mp3"), 1000);
    const auto result = applyRenames(maps, disk);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(1, last.groups.size());
    EXPECT_TRUE(last.groups.first().takeOver);
    EXPECT_EQ(1, result.relocated.size());
    ASSERT_EQ(1, result.merged.size());
    EXPECT_EQ(track, result.merged.first().survivor);
    EXPECT_EQ(scanned, result.merged.first().hidden);
    EXPECT_EQ(kNewMusic + QStringLiteral("/01 a.mp3"), locationOf(track));
    EXPECT_FALSE(missing(track));
    EXPECT_FALSE(hidden(track));
    EXPECT_TRUE(hidden(scanned));
    EXPECT_EQ(kNewMusic + QStringLiteral("/a.mp3"), locationOf(scanned));
    // Its cue, crate, history and count stay; the scan row's main cue joins.
    EXPECT_EQ(2, count(QStringLiteral("cues WHERE track_id=%1").arg(track)));
    EXPECT_EQ(1, count(QStringLiteral("crate_tracks WHERE crate_id=%1 AND track_id=%2").arg(crate).arg(track)));
    EXPECT_EQ(1, count(QStringLiteral("PlaylistTracks WHERE playlist_id=%1 AND track_id=%2").arg(set).arg(track)));
    EXPECT_EQ(3, value(QStringLiteral("SELECT timesplayed FROM library WHERE id=%1").arg(track)).toInt());
    EXPECT_EQ(before, totals());
    // What Tracks lists: one entry, the DJ's.
    EXPECT_EQ(1,
            count(QStringLiteral("library INNER JOIN track_locations ON library.location=track_locations.id "
                                 "WHERE mixxx_deleted=0 AND fs_deleted=0")));
}

// r7: a rename takeover the pass had to leave because a track was in use
// (the background analysis or a deck held it) is tried again, a rename-only
// pass too, once that track is no longer in use.
TEST_F(DeckLabRenameDbTest, ATakeoverLeftForATrackInUseIsTriedAgain) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = root.filePath(QStringLiteral("New Music"));
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString renamed = folder + QStringLiteral("/01 Generated Tone.mp3");
    {
        QFile file(renamed);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(4096, '\x5a'));
    }
    {
        QFile file(folder + QLatin1Char('/') + decklab::kRenameMapName);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(renameMap({{QStringLiteral("Generated Tone.mp3"), QStringLiteral("01 Generated Tone.mp3"), 4096}}));
    }
    exec(QStringLiteral("INSERT INTO directories (directory) VALUES ('%1')").arg(root.path()));
    const qint64 track = addTrack(folder + QStringLiteral("/Generated Tone.mp3"), 4096, true);
    const qint64 scanned = addTrack(renamed, 4096);
    addCue(track, CueType::HotCue, 100, 0, 0);

    decklab::UsbRelinker relinker(std::shared_ptr<TrackCollectionManager>(
            trackCollectionManager(), [](TrackCollectionManager*) {}));
    relinker.setBusyRetryInterval(50);
    QSet<qint64> inUse{scanned}; // the analysis holds the newest download
    relinker.setBusyTracks([&inUse] { return inUse; });
    QSignalSpy done(&relinker, &decklab::UsbRelinker::relinked);
    relinker.relinkRenames();
    ASSERT_TRUE(done.wait(5000));
    EXPECT_EQ(0, done.last().at(1).toInt()) << "left while in use";
    EXPECT_FALSE(hidden(scanned));
    EXPECT_FALSE(done.wait(300)) << "nothing again while it is still in use";
    // The analysis lets go of it: the takeover follows without another scan.
    inUse.clear();
    ASSERT_TRUE(done.wait(5000));
    EXPECT_EQ(1, done.last().at(1).toInt());
    EXPECT_EQ(renamed, locationOf(track));
    EXPECT_TRUE(hidden(scanned));
}

TEST_F(DeckLabRenameDbTest, MalformedMapIsIgnored) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = root.filePath(QStringLiteral("New Music"));
    ASSERT_TRUE(QDir().mkpath(folder));
    {
        QFile file(folder + QStringLiteral("/01 a.mp3"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(1000, '\x5a'));
    }
    {
        QFile file(folder + QLatin1Char('/') + decklab::kRenameMapName);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("[{\"old\": \"a.mp3\", \"new\": \"01 a.mp3\", \"size\": 1000"); // cut short
    }
    exec(QStringLiteral("INSERT INTO directories (directory) VALUES ('%1')").arg(root.path()));
    const qint64 track = addTrack(folder + QStringLiteral("/a.mp3"), 1000);
    EXPECT_TRUE(decklab::readRenameMaps({folder}).isEmpty());
    decklab::UsbRelinker relinker(std::shared_ptr<TrackCollectionManager>(
            trackCollectionManager(), [](TrackCollectionManager*) {}));
    QSignalSpy done(&relinker, &decklab::UsbRelinker::relinked);
    relinker.relinkRenames();
    ASSERT_TRUE(done.wait(5000));
    EXPECT_EQ(0, done.first().at(0).toInt());
    EXPECT_EQ(folder + QStringLiteral("/a.mp3"), locationOf(track));
}

TEST_F(DeckLabRenameDbTest, SecondRunChangesNothing) {
    const qint64 moved = addTrack(kNewMusic + QStringLiteral("/a.mp3"), 1000);
    const qint64 track = addTrack(kNewMusic + QStringLiteral("/b.mp3"), 2000, true);
    const qint64 scanned = addTrack(kNewMusic + QStringLiteral("/02 b.mp3"), 2000);
    decklab::RenameMaps maps;
    maps.insert(kNewMusic,
            {{QStringLiteral("a.mp3"), QStringLiteral("01 a.mp3"), 1000},
                    {QStringLiteral("b.mp3"), QStringLiteral("02 b.mp3"), 2000}});
    FakeDisk disk;
    disk.files.insert(kNewMusic + QStringLiteral("/01 a.mp3"), 1000);
    disk.files.insert(kNewMusic + QStringLiteral("/02 b.mp3"), 2000);
    ASSERT_TRUE(applyRenames(maps, disk).ok);
    EXPECT_EQ(2, last.groups.size());
    const QList<int> after = totals();
    const QString locations = locationOf(moved) + locationOf(track) + locationOf(scanned);
    const auto second = applyRenames(maps, disk);
    EXPECT_TRUE(second.ok);
    EXPECT_TRUE(last.isEmpty());
    EXPECT_TRUE(second.relocated.isEmpty());
    EXPECT_TRUE(second.merged.isEmpty());
    EXPECT_EQ(after, totals());
    EXPECT_EQ(locations, locationOf(moved) + locationOf(track) + locationOf(scanned));
}

// --- Tracks on a stick that is out ---

TEST(DeckLabMediaTest, MediaRootsAreMountFolders) {
    using decklab::mediaRootOf;
    EXPECT_EQ(kMount, mediaRootOf(kNow + QStringLiteral("Contents/a.mp3")));
    EXPECT_EQ(kLinkFolder, mediaRootOf(kLink + QStringLiteral("a.mp3")));
    EXPECT_EQ(QStringLiteral("/media/MY STICK"), mediaRootOf(QStringLiteral("/media/MY STICK/a.mp3")));
    EXPECT_EQ(QStringLiteral("/run/media/user/MY STICK"), mediaRootOf(QStringLiteral("/run/media/user/MY STICK/x/a.mp3")));
    EXPECT_TRUE(mediaRootOf(QStringLiteral("/home/user/Music/a.mp3")).isEmpty());
    EXPECT_TRUE(mediaRootOf(QStringLiteral("/run/user/1234/doc/a.mp3")).isEmpty());
    EXPECT_TRUE(mediaRootOf(QStringLiteral("/media/a.mp3")).isEmpty());
    EXPECT_TRUE(mediaRootOf(QStringLiteral("relative/media/a.mp3")).isEmpty());
}

TEST(DeckLabMediaTest, AFolderIsInWhenSomethingIsInIt) {
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    const QString empty = folder.filePath(QStringLiteral("left-behind"));
    const QString stick = folder.filePath(QStringLiteral("mount"));
    ASSERT_TRUE(QDir().mkpath(empty));
    ASSERT_TRUE(QDir().mkpath(stick + QStringLiteral("/Contents")));
    EXPECT_FALSE(decklab::mediaRootIsIn(empty));
    EXPECT_TRUE(decklab::mediaRootIsIn(stick));
    EXPECT_FALSE(decklab::mediaRootIsIn(folder.filePath(QStringLiteral("nothing"))));
#ifdef Q_OS_LINUX
    // The appliance's link to a mount is in; a dangling one is out.
    const QString link = folder.filePath(QStringLiteral("MY STICK"));
    ASSERT_TRUE(QFile::link(stick, link));
    EXPECT_TRUE(decklab::mediaRootIsIn(link));
    ASSERT_TRUE(QDir(stick).removeRecursively());
    EXPECT_FALSE(decklab::mediaRootIsIn(link));
#endif
}

namespace {

class DeckLabMediaDbTest : public DeckLabUsbMergeDbTest {
  protected:
    // The library's track cache, as MixxxLibraryFeature sets it up.
    void connectLibraryCache() {
        const QStringList columns{LIBRARYTABLE_ID, LIBRARYTABLE_PLAYED, LIBRARYTABLE_TIMESPLAYED,
                LIBRARYTABLE_LAST_PLAYED_AT, LIBRARYTABLE_ALBUMARTIST, LIBRARYTABLE_ALBUM, LIBRARYTABLE_ARTIST,
                LIBRARYTABLE_TITLE, LIBRARYTABLE_YEAR, LIBRARYTABLE_RATING, LIBRARYTABLE_GENRE, LIBRARYTABLE_COMPOSER,
                LIBRARYTABLE_GROUPING, LIBRARYTABLE_TRACKNUMBER, LIBRARYTABLE_KEY, LIBRARYTABLE_KEY_ID, LIBRARYTABLE_BPM,
                LIBRARYTABLE_BPM_LOCK, LIBRARYTABLE_BEATS_VERSION, LIBRARYTABLE_DURATION, LIBRARYTABLE_BITRATE,
                LIBRARYTABLE_REPLAYGAIN, LIBRARYTABLE_FILETYPE, LIBRARYTABLE_DATETIMEADDED, TRACKLOCATIONSTABLE_LOCATION,
                TRACKLOCATIONSTABLE_DIRECTORY, TRACKLOCATIONSTABLE_FSDELETED, LIBRARYTABLE_COMMENT,
                LIBRARYTABLE_MIXXXDELETED, LIBRARYTABLE_COLOR, LIBRARYTABLE_COVERART_SOURCE, LIBRARYTABLE_COVERART_TYPE,
                LIBRARYTABLE_COVERART_LOCATION, LIBRARYTABLE_COVERART_COLOR, LIBRARYTABLE_COVERART_DIGEST,
                LIBRARYTABLE_COVERART_HASH, LIBRARYTABLE_WAVESUMMARYHEX};
        QStringList qualified;
        for (const QString& column : columns) {
            qualified << QString(mixxx::trackschema::tableForColumn(column) + QLatin1Char('.') + column);
        }
        exec(QStringLiteral("CREATE TEMPORARY VIEW IF NOT EXISTS library_cache_view AS SELECT %1 "
                            "FROM library INNER JOIN track_locations ON library.location = track_locations.id")
                        .arg(qualified.join(QLatin1Char(','))));
        m_cache = QSharedPointer<BaseTrackCache>(new BaseTrackCache(internalCollection(),
                QStringLiteral("library_cache_view"), LIBRARYTABLE_ID, columns,
                QStringList{LIBRARYTABLE_ARTIST, LIBRARYTABLE_TITLE}, true));
        internalCollection()->connectTrackSource(m_cache);
    }
    void TearDown() override {
        internalCollection()->disconnectTrackSource();
        DeckLabUsbMergeDbTest::TearDown();
    }

  private:
    QSharedPointer<BaseTrackCache> m_cache;
};

} // namespace

// Like a CDJ: the songs of a stick that is out are in no list (Tracks,
// crates, playlists, history, Missing), the counts leave them out, and they
// come back when it is plugged in. Nothing is written but the temporary
// table; nothing is deleted.
TEST_F(DeckLabMediaDbTest, AbsentStickHiddenThenShownWhenMounted) {
    const QString away = QStringLiteral("/media/user/5678-EF01/");
    const qint64 onStick = addTrack(kNow + QStringLiteral("a.mp3"), 1000);
    const qint64 awayB = addTrack(away + QStringLiteral("away-b.mp3"), 2000);
    const qint64 awayC = addTrack(away + QStringLiteral("away-c.mp3"), 3000, true);
    const qint64 home = addTrack(QStringLiteral("/home/user/Music/d.mp3"), 4000);
    const qint64 crate = addCrate(QStringLiteral("Generated crate"));
    addToCrate(crate, onStick);
    addToCrate(crate, awayB);
    addToCrate(crate, awayC);
    const int earlierSet = history(QStringLiteral("2026-10-01"));
    append(earlierSet, awayB);
    append(earlierSet, awayC);
    const int currentSet = history(QStringLiteral("2026-10-23"));
    append(currentSet, awayB);
    append(currentSet, home);
    const int playlist = playlists().createPlaylist(QStringLiteral("Generated playlist"), PlaylistDAO::PLHT_NOT_HIDDEN);
    append(playlist, awayC);
    append(playlist, onStick);
    const QList<int> before = totals();

    QSet<QString> in{kMount};
    auto& presence = decklab::MediaPresence::instance();
    presence.setRootProbeForTesting([&in](const QString& root) { return in.contains(root); });
    QSignalSpy changed(&presence, &decklab::MediaPresence::changed);
    EXPECT_TRUE(presence.refresh(database()));
    EXPECT_EQ(QStringList{away.chopped(1)}, presence.awayRoots());
    EXPECT_EQ(2, presence.awayCount());
    EXPECT_EQ(1, changed.size());
    EXPECT_FALSE(presence.refresh(database())); // nothing new
    EXPECT_EQ(1, changed.size());

    connectLibraryCache();
    LibraryTableModel tracks(nullptr, trackCollectionManager(), "mixxx.db.model.decklab.media.tracks");
    tracks.select();
    CrateTableModel crateList(nullptr, trackCollectionManager());
    crateList.selectCrate(CrateId(QVariant(crate)));
    crateList.select();
    PlaylistTableModel currentList(nullptr, trackCollectionManager(), "mixxx.db.model.decklab.media.history");
    currentList.selectPlaylist(currentSet);
    currentList.select();
    PlaylistTableModel playlistList(nullptr, trackCollectionManager(), "mixxx.db.model.decklab.media.playlist", true);
    playlistList.selectPlaylist(playlist);
    playlistList.select();
    MissingTableModel missingList(nullptr, trackCollectionManager());
    missingList.select();
    EXPECT_EQ(2, tracks.rowCount()); // a and d
    EXPECT_EQ(1, crateList.rowCount());
    EXPECT_EQ(1, currentList.rowCount());
    EXPECT_EQ(1, playlistList.rowCount());
    EXPECT_EQ(0, missingList.rowCount()); // c is away, not missing: it cannot be purged
    EXPECT_EQ(1, value(QStringLiteral("SELECT track_count FROM crate_summary WHERE id=%1").arg(crate)).toInt());
    // The search finds nothing of it either.
    tracks.search(QStringLiteral("away"));
    EXPECT_EQ(0, tracks.rowCount());
    tracks.search(QString());

    auto& store = decklab::history::Store::instance();
    store.setBackend(decklab::history::makeMixxxBackend(trackCollectionManager(), UserSettingsPointer(),
            [] { return false; }, [](TrackId) { return 1; }));
    store.ensureLoaded();
    ASSERT_EQ(2, store.sessions().size());
    EXPECT_EQ(1, store.sessions().at(0).count);
    EXPECT_EQ(1, store.sessions().at(0).away);
    EXPECT_EQ(0, store.sessions().at(1).count);
    EXPECT_EQ(2, store.sessions().at(1).away);
    ASSERT_EQ(1, store.entries(store.sessions().at(0).id).size());
    EXPECT_EQ(home, store.entries(store.sessions().at(0).id).first().trackId);
    // The earlier set, played only from the stick that is out, waits for it.
    const auto rows = decklab::history::sessionRows(store.sessions(), QDate(2026, 10, 23));
    EXPECT_FALSE(std::any_of(rows.cbegin(), rows.cend(), [&store](const decklab::history::ListRow& listRow) {
        return listRow.sessionId == store.sessions().at(1).id;
    }));

    // Plugged in: every list selects again by itself, the history reads again.
    in.insert(away.chopped(1));
    EXPECT_TRUE(presence.refresh(database()));
    EXPECT_EQ(2, changed.size());
    EXPECT_TRUE(presence.awayRoots().isEmpty());
    EXPECT_EQ(3, tracks.rowCount()); // a, b, d (c is missing on the stick)
    EXPECT_EQ(3, crateList.rowCount());
    EXPECT_EQ(2, currentList.rowCount());
    EXPECT_EQ(2, playlistList.rowCount());
    EXPECT_EQ(1, missingList.rowCount());
    EXPECT_EQ(3, value(QStringLiteral("SELECT track_count FROM crate_summary WHERE id=%1").arg(crate)).toInt());
    QTest::qWait(50); // the history's reload is queued
    EXPECT_EQ(2, store.sessions().at(0).count);
    EXPECT_EQ(2, store.sessions().at(1).count);
    EXPECT_EQ(before, totals());
}

// r7: a stick plugged in mid-set stays cheap: only the list on screen reads
// again. Every other list, the crate and playlist counts and the history's
// sets wait until they are shown or until it is quiet.
TEST_F(DeckLabMediaDbTest, MidSetOnlyTheListOnScreenReadsAgain) {
    const QString away = QStringLiteral("/media/user/5678-EF01/");
    addTrack(kNow + QStringLiteral("a.mp3"), 1000);
    const qint64 awayB = addTrack(away + QStringLiteral("away-b.mp3"), 2000);
    const qint64 crate = addCrate(QStringLiteral("Generated crate"));
    addToCrate(crate, awayB);
    const int set = history(QStringLiteral("2026-10-23"));
    append(set, awayB);

    QSet<QString> in{kMount};
    auto& presence = decklab::MediaPresence::instance();
    presence.setRootProbeForTesting([&in](const QString& root) { return in.contains(root); });
    bool quiet = false;
    presence.setQuiet([&quiet] { return quiet; });
    struct Reset {
        ~Reset() {
            decklab::MediaPresence::instance().setQuiet({});
            decklab::MediaPresence::instance().setShown(nullptr);
        }
    } reset;
    EXPECT_TRUE(presence.refresh(database()));
    connectLibraryCache();
    LibraryTableModel tracks(nullptr, trackCollectionManager(), "mixxx.db.model.decklab.media.midset");
    tracks.select();
    CrateTableModel crateList(nullptr, trackCollectionManager());
    crateList.selectCrate(CrateId(QVariant(crate)));
    crateList.select();
    presence.setShown(&tracks);
    EXPECT_EQ(1, tracks.rowCount());
    EXPECT_EQ(0, crateList.rowCount());
    auto& store = decklab::history::Store::instance();
    store.setBackend(decklab::history::makeMixxxBackend(trackCollectionManager(), UserSettingsPointer(),
            [] { return false; }, [](TrackId) { return 1; }));
    store.ensureLoaded();
    ASSERT_EQ(1, store.sessions().size());
    EXPECT_EQ(0, store.sessions().at(0).count);
    decklab::UsbRelinker relinker(std::shared_ptr<TrackCollectionManager>(
            trackCollectionManager(), [](TrackCollectionManager*) {}));
    relinker.setQuiet([&quiet] { return quiet; });
    int counts = 0;
    QObject::connect(internalCollection(), &TrackCollection::crateSummaryChanged, &relinker, [&counts] { ++counts; });

    // Plugged in while a deck plays.
    in.insert(away.chopped(1));
    relinker.refreshMedia();
    EXPECT_EQ(2, tracks.rowCount()) << "the list on screen reads at once";
    EXPECT_EQ(0, crateList.rowCount()) << "the others wait";
    EXPECT_TRUE(crateList.mediaStale());
    EXPECT_EQ(0, counts) << "the counts wait";
    QTest::qWait(50);
    EXPECT_EQ(0, store.sessions().at(0).count) << "the history waits";
    EXPECT_TRUE(store.stale());
    // A list that is shown reads again then.
    presence.setShown(&crateList);
    EXPECT_EQ(1, crateList.rowCount());
    EXPECT_FALSE(crateList.mediaStale());
    // The History list reads again when it is shown.
    store.ensureFresh();
    EXPECT_EQ(1, store.sessions().at(0).count);
    // Pulled again: the crate is on screen now; everything else reads once
    // it is quiet.
    in.remove(away.chopped(1));
    relinker.refreshMedia();
    EXPECT_EQ(0, crateList.rowCount()) << "the list on screen";
    EXPECT_EQ(2, tracks.rowCount());
    EXPECT_TRUE(tracks.mediaStale());
    EXPECT_TRUE(store.stale());
    quiet = true;
    ASSERT_TRUE(QTest::qWaitFor([&] { return counts > 0; }, decklab::MediaPresence::kSettleCheckMs + 2000));
    EXPECT_EQ(1, tracks.rowCount());
    EXPECT_FALSE(tracks.mediaStale());
    QTest::qWait(50);
    EXPECT_FALSE(store.stale());
    EXPECT_EQ(0, store.sessions().at(0).count);
}
