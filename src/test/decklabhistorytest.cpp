// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab round 5: track history and "played" marks (widget/decklabhistory.h,
// widget/decklabhistoryview.h). Generated data only: invented titles and
// files under a temporary folder that are never read.
#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QListWidget>
#include <QLocale>
#include <QObject>
#include <QPainter>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>

#include "library/dao/playlistdao.h"
#include "test/librarytest.h"
#include "widget/decklabbrowse.h"
#include "widget/decklabbrowsemix.h"
#include "widget/decklabhistory.h"
#include "widget/decklabhistoryview.h"

using namespace decklab::history;

namespace {

QDateTime at(int year, int month, int day, int hour, int minute) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute));
}

// A history with no database: what a backend reports, set by the test.
class GeneratedHistory final : public Backend {
  public:
    QVector<Session> sets; // newest first
    QHash<int, QVector<Entry>> tracks;
    int newSets = 0;
    QVector<Session> sessions() override {
        return sets;
    }
    QVector<Entry> entries(int sessionId) override {
        return tracks.value(sessionId);
    }
    void playedLocations(int currentId, QSet<QString>* session, QSet<QString>* earlier) override {
        for (auto it = tracks.cbegin(); it != tracks.cend(); ++it) {
            for (const Entry& entry : it.value()) {
                (it.key() == currentId ? session : earlier)->insert(entry.location);
            }
        }
    }
    bool startNewSet() override {
        ++newSets;
        return false;
    }
};

class DeckLabHistoryDbTest : public LibraryTest {
  protected:
    void TearDown() override {
        Store::instance().reset();
    }
    // A library row for a generated file (never read).
    TrackId addTrack(const QString& title, double bpm, mixxx::track::io::key::ChromaticKey key) {
        const QString location = QDir(m_folder.path()).filePath(title + QStringLiteral(".wav"));
        QSqlQuery query(internalCollection()->database());
        query.prepare(QStringLiteral(
                "INSERT INTO track_locations (location, filename, directory, filesize, fs_deleted, "
                "needs_verification) VALUES (:location, :filename, :directory, 1000, 0, 0)"));
        query.bindValue(QStringLiteral(":location"), location);
        query.bindValue(QStringLiteral(":filename"), QFileInfo(location).fileName());
        query.bindValue(QStringLiteral(":directory"), QFileInfo(location).path());
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        const QVariant locationId = query.lastInsertId();
        query.prepare(QStringLiteral(
                "INSERT INTO library (location, title, artist, bpm, key_id) "
                "VALUES (:location, :title, 'Deck Lab Fixture', :bpm, :key)"));
        query.bindValue(QStringLiteral(":location"), locationId);
        query.bindValue(QStringLiteral(":title"), title);
        query.bindValue(QStringLiteral(":bpm"), bpm);
        query.bindValue(QStringLiteral(":key"), static_cast<int>(key));
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        m_locations.insert(title, location);
        return TrackId(query.lastInsertId());
    }
    QString location(const QString& title) const {
        return m_locations.value(title);
    }
    PlaylistDAO& playlists() {
        return internalCollection()->getPlaylistDAO();
    }

  private:
    QTemporaryDir m_folder;
    QHash<QString, QString> m_locations;
};

} // namespace

// The sets and their tracks are Mixxx's history playlists: newest first, the
// newest is the current set, every track in play order; other playlists are
// not history.
TEST_F(DeckLabHistoryDbTest, SetsAndPlayOrderComeFromMixxxHistoryPlaylists) {
    using mixxx::track::io::key::A_MINOR;
    const TrackId first = addTrack(QStringLiteral("Generated Tone 122"), 122, A_MINOR);
    const TrackId second = addTrack(QStringLiteral("Generated Tone 124"), 124, mixxx::track::io::key::E_MINOR);
    const TrackId tonight = addTrack(QStringLiteral("Generated Tone 126"), 126, mixxx::track::io::key::C_MAJOR);
    const TrackId crated = addTrack(QStringLiteral("Generated Tone 128"), 128, A_MINOR);
    const int earlier = playlists().createPlaylist(QStringLiteral("2026-10-18"), PlaylistDAO::PLHT_SET_LOG);
    ASSERT_TRUE(playlists().appendTrackToPlaylist(first, earlier));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(second, earlier));
    const int crate = playlists().createPlaylist(QStringLiteral("Generated playlist"), PlaylistDAO::PLHT_NOT_HIDDEN);
    ASSERT_TRUE(playlists().appendTrackToPlaylist(crated, crate));
    const int current = playlists().createPlaylist(QStringLiteral("2026-10-23"), PlaylistDAO::PLHT_SET_LOG);
    ASSERT_TRUE(playlists().appendTrackToPlaylist(tonight, current));

    Store& store = Store::instance();
    store.setBackend(makeMixxxBackend(trackCollectionManager(), [] { return false; }, [](TrackId) { return 2; }));
    EXPECT_FALSE(store.loaded());
    EXPECT_EQ(Played::No, store.played(location(QStringLiteral("Generated Tone 126")))); // nothing read yet
    store.ensureLoaded();
    ASSERT_EQ(2, store.sessions().size());
    EXPECT_EQ(current, store.currentId());
    EXPECT_TRUE(store.sessions().at(0).current);
    EXPECT_FALSE(store.sessions().at(1).current);
    EXPECT_EQ(1, store.sessions().at(0).count);
    EXPECT_EQ(2, store.sessions().at(1).count);
    EXPECT_TRUE(store.sessions().at(1).first.isValid());
    EXPECT_EQ(1, store.currentCount());
    // One lookup per row: this set, an earlier set, or neither.
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 126"))));
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 122"))));
    EXPECT_EQ(Played::No, store.played(location(QStringLiteral("Generated Tone 128"))));
    const QVector<Entry> set = store.entries(earlier);
    ASSERT_EQ(2, set.size());
    EXPECT_EQ(1, set.at(0).position);
    EXPECT_EQ(2, set.at(1).position);
    EXPECT_EQ(QStringLiteral("Generated Tone 122"), set.at(0).title);
    EXPECT_EQ(QStringLiteral("Deck Lab Fixture"), set.at(0).artist);
    EXPECT_DOUBLE_EQ(122, set.at(0).bpm);
    EXPECT_EQ(A_MINOR, set.at(0).key);
    EXPECT_EQ(location(QStringLiteral("Generated Tone 122")), set.at(0).location);
    EXPECT_TRUE(set.at(0).played.isValid());
    // Mixxx does not store the deck: unknown for sets this unit did not see.
    EXPECT_EQ(0, set.at(0).deck);
}

// Played marks follow the history live: PlaylistDAO::trackAdded (the setlog
// adding the track that became audible) marks it at once and tells listeners
// which row changed; "New set" turns this set's marks into earlier marks.
TEST_F(DeckLabHistoryDbTest, MarksFollowTheHistoryLiveAndNewSetMovesThemToEarlier) {
    const TrackId opener = addTrack(QStringLiteral("Generated Tone 120"), 120, mixxx::track::io::key::D_MINOR);
    const TrackId next = addTrack(QStringLiteral("Generated Tone 121"), 121, mixxx::track::io::key::D_MINOR);
    const int current = playlists().createPlaylist(QStringLiteral("2026-10-23"), PlaylistDAO::PLHT_SET_LOG);
    ASSERT_TRUE(playlists().appendTrackToPlaylist(opener, current));
    int newSets = 0;
    Store& store = Store::instance();
    store.setBackend(makeMixxxBackend(
            trackCollectionManager(),
            [this, &newSets] {
                ++newSets; // what SetlogFeature::slotGetNewPlaylist does
                return playlists().createPlaylist(QStringLiteral("2026-10-23 #2"), PlaylistDAO::PLHT_SET_LOG) !=
                        kInvalidPlaylistId;
            },
            [](TrackId) { return 2; }));
    QObject owner;
    QList<Change> heard;
    store.listen(&owner, [&heard](const Change& change) { heard.append(change); });
    store.ensureLoaded();
    ASSERT_EQ(1, heard.size());
    EXPECT_TRUE(heard.first().locations.contains(location(QStringLiteral("Generated Tone 120"))));
    heard.clear();
    EXPECT_EQ(Played::No, store.played(location(QStringLiteral("Generated Tone 121"))));

    // Deck 2 became the audible deck: Mixxx's setlog appends its track.
    ASSERT_TRUE(playlists().appendTrackToPlaylist(next, current));
    ASSERT_EQ(1, heard.size());
    EXPECT_EQ(QSet<QString>{location(QStringLiteral("Generated Tone 121"))}, heard.first().locations);
    EXPECT_TRUE(heard.first().history);
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 121"))));
    EXPECT_EQ(2, store.currentCount());
    const QVector<Entry> set = store.entries(current);
    ASSERT_EQ(2, set.size());
    EXPECT_EQ(QStringLiteral("Generated Tone 121"), set.last().title);
    EXPECT_EQ(2, set.last().deck);
    EXPECT_EQ(0, set.first().deck);

    // Another playlist changing is not history.
    const int crate = playlists().createPlaylist(QStringLiteral("Generated playlist"), PlaylistDAO::PLHT_NOT_HIDDEN);
    heard.clear();
    ASSERT_TRUE(playlists().appendTrackToPlaylist(opener, crate));
    EXPECT_TRUE(heard.isEmpty());

    // New set: the last set's tracks keep a quieter mark; nothing is played in the new one.
    EXPECT_TRUE(store.startNewSet());
    EXPECT_EQ(1, newSets);
    EXPECT_NE(current, store.currentId());
    EXPECT_EQ(2, store.sessions().size());
    EXPECT_EQ(0, store.currentCount());
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 120"))));
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 121"))));
    ASSERT_FALSE(heard.isEmpty());
    EXPECT_TRUE(heard.last().locations.contains(location(QStringLiteral("Generated Tone 121"))));
    // The deck this unit saw survives the reload; an empty set never starts another.
    EXPECT_EQ(2, store.entries(current).last().deck);
    EXPECT_FALSE(store.startNewSet());
    EXPECT_EQ(1, newSets);
}

// The sets list: this set first, then earlier sets under the day they began,
// each saying what it holds.
TEST(DeckLabBrowseTest, HistoryGroupsEarlierSetsUnderTheirDayAndSaysWhatEachHolds) {
    const QDate today(2026, 10, 23);
    Session current;
    current.id = 9;
    current.current = true;
    current.count = 14;
    current.first = at(2026, 10, 23, 21, 4);
    current.last = at(2026, 10, 23, 22, 31);
    Session lastNight;
    lastNight.id = 8;
    lastNight.count = 37;
    lastNight.first = at(2026, 10, 22, 21, 0);
    lastNight.last = at(2026, 10, 23, 1, 30);
    Session warmUp;
    warmUp.id = 7;
    warmUp.count = 1;
    warmUp.first = at(2026, 10, 22, 18, 0);
    warmUp.last = warmUp.first;
    Session longAgo;
    longAgo.id = 3;
    longAgo.count = 50;
    longAgo.first = at(2025, 12, 31, 22, 0);
    longAgo.last = at(2026, 1, 1, 3, 15);
    const auto rows = sessionRows({current, lastNight, warmUp, longAgo}, today);
    ASSERT_EQ(6, rows.size());
    EXPECT_EQ(RowKind::Session, rows[0].kind);
    EXPECT_EQ(9, rows[0].sessionId);
    EXPECT_EQ(QStringLiteral("This set"), rows[0].text);
    EXPECT_EQ(RowKind::Header, rows[1].kind);
    EXPECT_EQ(QStringLiteral("Yesterday"), rows[1].text);
    EXPECT_EQ(8, rows[2].sessionId);
    EXPECT_EQ(7, rows[3].sessionId);
    EXPECT_EQ(RowKind::Header, rows[4].kind);
    EXPECT_EQ(QLocale().toString(QDate(2025, 12, 31), QStringLiteral("d MMM yyyy")), rows[4].text);
    EXPECT_EQ(3, rows[5].sessionId);
    EXPECT_EQ(QStringLiteral("21:00 – 01:30"), sessionTitle(lastNight));
    EXPECT_EQ(QStringLiteral("18:00"), sessionTitle(warmUp));
    EXPECT_EQ(QStringLiteral("Since 21:04 · 14 tracks"), sessionDetail(current));
    EXPECT_EQ(QStringLiteral("37 tracks · 4 h 30 min"), sessionDetail(lastNight));
    EXPECT_EQ(QStringLiteral("1 track"), sessionDetail(warmUp));
    Session fresh;
    fresh.current = true;
    EXPECT_EQ(QStringLiteral("Nothing played yet"), sessionDetail(fresh));
    EXPECT_EQ(QStringLiteral("Today"), dayLabel(today, today));
    EXPECT_EQ(QLocale().standaloneDayName(QDate(2026, 10, 20).dayOfWeek(), QLocale::LongFormat),
            dayLabel(QDate(2026, 10, 20), today));
    EXPECT_EQ(QLocale().toString(QDate(2026, 10, 11), QStringLiteral("ddd d MMM")), dayLabel(QDate(2026, 10, 11), today));
    EXPECT_EQ(QString(), durationText(59));
    EXPECT_EQ(QStringLiteral("50 min"), durationText(3000));
    EXPECT_EQ(QStringLiteral("2 h 05 min"), durationText(7500));
    EXPECT_EQ(QStringLiteral("21:04"), clockText(current.first));
}

// LOAD and Preview read the set's own Mixxx table: the row with the same
// position and file, else the same file elsewhere, never another track.
TEST(DeckLabBrowseTest, HistoryRowFindsItsTrackInTheSetsTableAndNeverAnother) {
    QStandardItemModel model(0, 2);
    const QStringList files{QStringLiteral("/generated/a.wav"), QStringLiteral("/generated/b.wav"),
            QStringLiteral("/generated/a.wav")};
    for (int row = 0; row < files.size(); ++row) {
        model.appendRow({new QStandardItem(QString::number(row + 1)), new QStandardItem(files[row])});
    }
    const auto locationOf = [&model](int row) { return model.index(row, 1).data().toString(); };
    EXPECT_EQ(2, modelRowFor(model, 0, 3, files[0], locationOf)); // played twice: the second time
    EXPECT_EQ(1, modelRowFor(model, 0, 2, files[1], locationOf));
    EXPECT_EQ(0, modelRowFor(model, 0, 2, files[0], locationOf)); // position moved: same file
    EXPECT_EQ(-1, modelRowFor(model, 0, 1, QStringLiteral("/generated/c.wav"), locationOf));
    EXPECT_EQ(0, modelRowFor(model, -1, 3, files[0], locationOf)); // no position column
}

// The rows: a set's tracks in play order with time, deck, BPM and key; the
// current set opens on its latest track, an earlier one on its first; date
// headers and empty rows are never highlighted.
TEST(DeckLabBrowseTest, HistoryRowsKeepPlayOrderAndOpenOnTheLatestTrack) {
    QVector<Entry> entries;
    for (int n = 1; n <= 3; ++n) {
        Entry entry;
        entry.position = n;
        entry.played = at(2026, 10, 23, 21, n * 7);
        entry.deck = n == 2 ? 0 : n;
        entry.title = QStringLiteral("Generated Tone %1").arg(119 + n);
        entry.artist = QStringLiteral("Deck Lab Fixture");
        entry.bpm = 119 + n;
        entry.key = mixxx::track::io::key::A_MINOR;
        entry.location = QStringLiteral("/generated/%1.wav").arg(n);
        entries.append(entry);
    }
    QListWidget list;
    EXPECT_EQ(2, fillTracks(&list, entries, true, -1));
    ASSERT_EQ(3, list.count());
    EXPECT_EQ(QStringLiteral("Generated Tone 120"), list.item(0)->text());
    EXPECT_EQ(static_cast<int>(RowKind::Track), list.item(0)->data(RowRole).toInt());
    EXPECT_EQ(QStringLiteral("21:07"), list.item(0)->data(TimeRole).toString());
    EXPECT_EQ(1, list.item(0)->data(DeckRole).toInt());
    EXPECT_EQ(0, list.item(1)->data(DeckRole).toInt());
    EXPECT_DOUBLE_EQ(122, list.item(2)->data(BpmRole).toDouble());
    EXPECT_FALSE(list.item(0)->data(KeyRole).toString().isEmpty());
    EXPECT_EQ(QStringLiteral("/generated/3.wav"), list.item(2)->data(LocationRole).toString());
    EXPECT_EQ(0, fillTracks(&list, entries, false, -1));
    EXPECT_EQ(1, fillTracks(&list, entries, false, 2));
    EXPECT_EQ(-1, fillTracks(&list, {}, true, -1));
    ASSERT_EQ(1, list.count());
    EXPECT_FALSE(list.item(0)->flags() & Qt::ItemIsEnabled);

    Session current;
    current.id = 5;
    current.current = true;
    current.count = 3;
    current.first = entries.first().played;
    current.last = entries.last().played;
    Session earlier;
    earlier.id = 4;
    earlier.count = 12;
    earlier.first = at(2026, 10, 22, 21, 0);
    earlier.last = at(2026, 10, 22, 23, 0);
    EXPECT_EQ(0, fillSessions(&list, {current, earlier}, QDate(2026, 10, 23), -1));
    ASSERT_EQ(3, list.count());
    EXPECT_EQ(QStringLiteral("3"), list.item(0)->data(CountRole).toString());
    EXPECT_FALSE(list.item(1)->flags() & Qt::ItemIsEnabled); // "Yesterday"
    EXPECT_EQ(2, fillSessions(&list, {current, earlier}, QDate(2026, 10, 23), 4));
    EXPECT_EQ(QStringLiteral("12 tracks · 2 h 00 min"), list.item(2)->data(DetailRole).toString());
}

// The mark: a muted disc with a dark check (this set), a dim ring (an earlier
// set), never red; a selected row never tints it; the Mixes rows carry it.
TEST(DeckLabBrowseTest, PlayedMarksAreQuietNeverRedAndReachTheMixesRows) {
    const auto render = [](Played played) {
        QImage image(kMarkSize, kMarkSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintPlayedMark(painter, QRectF(0, 0, kMarkSize, kMarkSize), played);
        return image;
    };
    const QImage session = render(Played::Session);
    const QImage earlier = render(Played::Earlier);
    const QImage none = render(Played::No);
    int sessionInk = 0;
    int earlierInk = 0;
    for (int y = 0; y < kMarkSize; ++y) {
        for (int x = 0; x < kMarkSize; ++x) {
            const QColor a = session.pixelColor(x, y);
            const QColor b = earlier.pixelColor(x, y);
            EXPECT_FALSE(a.alpha() > 0 && a.red() > 200 && a.green() < 120) << "red in the mark";
            EXPECT_FALSE(b.alpha() > 0 && b.red() > 200 && b.green() < 120) << "red in the mark";
            sessionInk += a.alpha() > 0;
            earlierInk += b.alpha() > 0;
            EXPECT_EQ(0, none.pixelColor(x, y).alpha());
        }
    }
    EXPECT_GT(sessionInk, earlierInk); // filled disc against a ring
    EXPECT_EQ(0, earlier.pixelColor(kMarkSize / 2 - 4, kMarkSize / 2 + 3).alpha()); // the ring is hollow
    const QIcon icon = playedIcon(Played::Session);
    ASSERT_FALSE(icon.isNull());
    EXPECT_EQ(icon.pixmap(QSize(kMarkSize, kMarkSize), QIcon::Normal).toImage(),
            icon.pixmap(QSize(kMarkSize, kMarkSize), QIcon::Selected).toImage());
    EXPECT_TRUE(playedIcon(Played::No).isNull());

    // A Mixes row of a track played in this set: the mark at its right edge.
    auto generated = std::make_unique<GeneratedHistory>();
    Session set;
    set.id = 1;
    set.current = true;
    set.count = 1;
    generated->sets = {set};
    Entry played;
    played.position = 1;
    played.location = QStringLiteral("/generated/played.wav");
    generated->tracks.insert(1, {played});
    Store::instance().setBackend(std::move(generated));
    Store::instance().ensureLoaded();
    // An export's list may double a slash where Mixxx keeps the path clean.
    EXPECT_EQ(Played::Session, Store::instance().played(QStringLiteral("/generated//played.wav")));
    EXPECT_EQ(Played::No, Store::instance().played(QStringLiteral("/generated//other.wav")));
    QListWidget list;
    auto* item = new QListWidgetItem(QStringLiteral("Generated Tone 124"), &list);
    item->setData(DeckLabListDelegate::MixRole, true);
    item->setData(DeckLabListDelegate::MixKeyRole, QStringLiteral("8A"));
    item->setData(DeckLabListDelegate::MixBpmRole, 124.0);
    item->setData(LocationRole, played.location);
    QImage row(520, 44, QImage::Format_ARGB32_Premultiplied);
    row.fill(Qt::black);
    {
        QPainter painter(&row);
        QStyleOptionViewItem option;
        option.rect = row.rect();
        option.font = list.font();
        decklab::paintMixRow(painter, option, list.model()->index(0, 0));
    }
    const QColor mark = row.pixelColor(row.width() - 12 - kMarkSize / 2 - 4, row.height() / 2 - 4);
    EXPECT_EQ(decklab::colors::muted.name(), mark.name());
    Store::instance().reset();
}
