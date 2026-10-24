// SPDX-License-Identifier: GPL-2.0-or-later
// r6-keys: the one key reader and formatter (decklab/decklabkeys.h), and
// Browse's KEY cells, key values and folder sort through it. Generated key
// texts only: as a rekordbox export, a file tag or Mixxx would store them.
#include <gtest/gtest.h>

#include <QListWidget>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardItemModel>
#include <QTableView>
#include <algorithm>
#include <memory>

#include "control/controlobject.h"
#include "decklab/decklabkeys.h"
#include "library/columncache.h"
#include "library/dao/trackschema.h"
#include "library/library_prefs.h"
#include "library/proxytrackmodel.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/searchquery.h"
#include "library/searchqueryparser.h"
#include "library/trackmodel.h"
#include "test/decklabgeneratedfolder.h"
#include "test/decklabpaintedtext.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "widget/decklabbrowse.h"

using namespace mixxx::track::io::key;
using decklab::KeyFormat;

namespace {

// Every test leaves the process on the default format.
class KeyFormatScope {
  public:
    explicit KeyFormatScope(KeyFormat format) {
        decklab::setKeyFormat(format);
    }
    ~KeyFormatScope() {
        decklab::setKeyFormat(KeyFormat::Lancelot);
    }
};

QList<ChromaticKey> allKeys() {
    QList<ChromaticKey> keys;
    for (int value = C_MAJOR; value <= B_MINOR; ++value) {
        keys.append(static_cast<ChromaticKey>(value));
    }
    return keys;
}

} // namespace

TEST(DeckLabBrowseMatchTest, KeyTextReadsLancelotOpenKeyAndMusicalNames) {
    // Every key, printed in each format, reads back as itself.
    for (const auto format : {KeyFormat::Lancelot, KeyFormat::OpenKey, KeyFormat::Musical}) {
        for (const ChromaticKey key : allKeys()) {
            const QString text = decklab::formatKey(key, format);
            EXPECT_EQ(key, decklab::parseKeyText(text)) << text.toStdString();
        }
    }
    // Lancelot, as rekordbox "Alphanumeric" and tag writers store it.
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("8A")));
    EXPECT_EQ(C_MAJOR, decklab::parseKeyText(QStringLiteral("8B")));
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("8a")));
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("08A")));
    EXPECT_EQ(E_MAJOR, decklab::parseKeyText(QStringLiteral(" 12 B ")));
    EXPECT_EQ(G_SHARP_MINOR, decklab::parseKeyText(QStringLiteral("1A")));
    // Open Key.
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("1m")));
    EXPECT_EQ(C_MAJOR, decklab::parseKeyText(QStringLiteral("1d")));
    EXPECT_EQ(F_MAJOR, decklab::parseKeyText(QStringLiteral("12d")));
    EXPECT_EQ(F_SHARP_MINOR, decklab::parseKeyText(QStringLiteral("4m")));
    // Musical names, as rekordbox "Classic" and most tags store them:
    // minors and majors, sharps and flats in ASCII or as signs.
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("Am")));
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("A minor")));
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("Amin")));
    EXPECT_EQ(A_MAJOR, decklab::parseKeyText(QStringLiteral("A")));
    EXPECT_EQ(A_MAJOR, decklab::parseKeyText(QStringLiteral("Amaj")));
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("a"))); // Mixxx: lower case is minor
    EXPECT_EQ(F_SHARP_MINOR, decklab::parseKeyText(QStringLiteral("F#m")));
    EXPECT_EQ(F_SHARP_MINOR, decklab::parseKeyText(QStringLiteral(u"F♯m")));
    EXPECT_EQ(E_FLAT_MINOR, decklab::parseKeyText(QStringLiteral("Ebm")));
    EXPECT_EQ(B_FLAT_MAJOR, decklab::parseKeyText(QStringLiteral("Bbmaj")));
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("Am +50"))); // tuning noted after the key
    // Enharmonics are one key.
    EXPECT_EQ(D_FLAT_MAJOR, decklab::parseKeyText(QStringLiteral("C#")));
    EXPECT_EQ(D_FLAT_MAJOR, decklab::parseKeyText(QStringLiteral("Db")));
    EXPECT_EQ(G_SHARP_MINOR, decklab::parseKeyText(QStringLiteral("Abm")));
    EXPECT_EQ(E_FLAT_MINOR, decklab::parseKeyText(QStringLiteral("D#m")));
    EXPECT_EQ(F_SHARP_MAJOR, decklab::parseKeyText(QStringLiteral("Gb")));
    EXPECT_EQ(B_MAJOR, decklab::parseKeyText(QStringLiteral("Cb")));
    EXPECT_EQ(F_MAJOR, decklab::parseKeyText(QStringLiteral("E#")));
    // Combined notations whose parts agree, Mixxx's two-name keys included.
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("8A (Am)")));
    EXPECT_EQ(A_MINOR, decklab::parseKeyText(QStringLiteral("1m / Am")));
    EXPECT_EQ(F_SHARP_MAJOR, decklab::parseKeyText(QStringLiteral(u"F♯/G♭")));
    EXPECT_EQ(E_FLAT_MINOR, decklab::parseKeyText(QStringLiteral(u"D♯m/E♭m")));
}

TEST(DeckLabBrowseMatchTest, KeyTextThatIsNotSurelyAKeyIsNeverGuessed) {
    for (const char* junk : {"", "   ", "Unknown", "H", "o", "x", "-", "0A", "13A", "8C", "8AA", "123",
                 "8D", "8M", "1 2A", "Am7", "Dance", "8A (C)", "Am / 9A",
                 "a very long comment that happens to start with A minor"}) {
        EXPECT_EQ(INVALID, decklab::parseKeyText(QString::fromUtf8(junk))) << junk;
        // Shown exactly as stored: never blank, never another key.
        EXPECT_EQ(QString::fromUtf8(junk), decklab::displayKey(QString::fromUtf8(junk))) << junk;
    }
}

TEST(DeckLabBrowseMatchTest, KeyFormatsPrintEveryKeyOnceAndLancelotMatchesTheWheel) {
    EXPECT_EQ(QStringLiteral("8A"), decklab::formatKey(A_MINOR, KeyFormat::Lancelot));
    EXPECT_EQ(QStringLiteral("1m"), decklab::formatKey(A_MINOR, KeyFormat::OpenKey));
    EXPECT_EQ(QStringLiteral("Am"), decklab::formatKey(A_MINOR, KeyFormat::Musical));
    EXPECT_EQ(QStringLiteral("8B"), decklab::formatKey(C_MAJOR, KeyFormat::Lancelot));
    EXPECT_EQ(QStringLiteral("1d"), decklab::formatKey(C_MAJOR, KeyFormat::OpenKey));
    EXPECT_EQ(QStringLiteral("C"), decklab::formatKey(C_MAJOR, KeyFormat::Musical));
    EXPECT_EQ(QStringLiteral("2B"), decklab::formatKey(F_SHARP_MAJOR, KeyFormat::Lancelot));
    EXPECT_EQ(QStringLiteral(u"F♯"), decklab::formatKey(F_SHARP_MAJOR, KeyFormat::Musical));
    EXPECT_EQ(QStringLiteral("2A"), decklab::formatKey(E_FLAT_MINOR, KeyFormat::Lancelot));
    EXPECT_EQ(QStringLiteral("7m"), decklab::formatKey(E_FLAT_MINOR, KeyFormat::OpenKey));
    EXPECT_EQ(QStringLiteral(u"E♭m"), decklab::formatKey(E_FLAT_MINOR, KeyFormat::Musical));
    for (const auto format : {KeyFormat::Lancelot, KeyFormat::OpenKey, KeyFormat::Musical}) {
        EXPECT_TRUE(decklab::formatKey(INVALID, format).isEmpty());
        QSet<QString> names;
        for (const ChromaticKey key : allKeys()) {
            const QString& name = decklab::formatKey(key, format);
            EXPECT_FALSE(name.isEmpty());
            EXPECT_FALSE(name.contains(QLatin1Char('/'))) << name.toStdString(); // one name per key
            EXPECT_LE(name.size(), 3) << name.toStdString();                     // fits every key slot
            names.insert(name);
        }
        EXPECT_EQ(24, names.size());
    }
    for (const ChromaticKey key : allKeys()) {
        EXPECT_EQ(decklab::lancelotText(key), decklab::formatKey(key, KeyFormat::Lancelot));
    }
    // KeyUtils gets the same names (Mixxx's own tables and filters).
    const auto map = decklab::keyNotationMap(KeyFormat::Musical);
    EXPECT_EQ(24, map.size());
    EXPECT_EQ(QStringLiteral(u"E♭m"), map.value(E_FLAT_MINOR));
}

TEST(DeckLabBrowseMatchTest, DisplayKeyFollowsTheChosenFormatKeyIdFirst) {
    {
        KeyFormatScope lancelot(KeyFormat::Lancelot);
        EXPECT_EQ(QStringLiteral("8A"), decklab::displayKey(QStringLiteral("Am")));
        EXPECT_EQ(QStringLiteral("8A"), decklab::displayKey(QStringLiteral("1m")));
        EXPECT_EQ(QStringLiteral("11A"), decklab::displayKey(QStringLiteral("F#m")));
        EXPECT_EQ(QStringLiteral("8B"), decklab::displayKey(C_MAJOR, QStringLiteral("Am"))); // the id wins
        EXPECT_EQ(QStringLiteral("8A"), decklab::displayKey(INVALID, QStringLiteral("Am")));
        EXPECT_EQ(QStringLiteral("Unknown"), decklab::displayKey(INVALID, QStringLiteral("Unknown")));
        EXPECT_TRUE(decklab::displayKey(INVALID, QString()).isEmpty());
        EXPECT_EQ(QString::fromUtf8("Relative 8A→8B"), decklab::keyRelationText(C_MAJOR, A_MINOR));
    }
    {
        KeyFormatScope openKey(KeyFormat::OpenKey);
        EXPECT_EQ(QStringLiteral("1m"), decklab::displayKey(QStringLiteral("Am")));
        EXPECT_EQ(QStringLiteral("1m"), decklab::displayKey(QStringLiteral("8A")));
        EXPECT_EQ(QStringLiteral("1d"), decklab::formatKey(C_MAJOR));
    }
    {
        KeyFormatScope musical(KeyFormat::Musical);
        EXPECT_EQ(QStringLiteral("Am"), decklab::displayKey(QStringLiteral("8A")));
        EXPECT_EQ(QStringLiteral("Am"), decklab::displayKey(QStringLiteral("Am")));
        EXPECT_EQ(QStringLiteral(u"F♯m"), decklab::displayKey(QStringLiteral("11A")));
        EXPECT_EQ(QStringLiteral(u"E♭m"), decklab::displayKey(QStringLiteral("D#m")));
        EXPECT_EQ(QString::fromUtf8("Relative Am→C"), decklab::keyRelationText(C_MAJOR, A_MINOR));
    }
    EXPECT_EQ(KeyFormat::Lancelot, decklab::keyFormat());
}

TEST(DeckLabBrowseMatchTest, KeyWheelOrderIsLancelotOrOpenKeyOrder) {
    QList<ChromaticKey> keys = allKeys();
    const auto sorted = [&keys](KeyFormat format) {
        std::sort(keys.begin(), keys.end(), [format](ChromaticKey a, ChromaticKey b) {
            return decklab::keyWheelOrder(a, format) < decklab::keyWheelOrder(b, format);
        });
        QStringList names;
        for (const ChromaticKey key : keys) {
            names << decklab::formatKey(key, format);
        }
        return names;
    };
    EXPECT_EQ(QStringLiteral("1A 1B 2A 2B 3A 3B 4A 4B 5A 5B 6A 6B 7A 7B 8A 8B 9A 9B 10A 10B 11A 11B 12A 12B"),
            sorted(KeyFormat::Lancelot).join(QLatin1Char(' ')));
    EXPECT_EQ(QStringLiteral("1d 1m 2d 2m 3d 3m 4d 4m 5d 5m 6d 6m 7d 7m 8d 8m 9d 9m 10d 10m 11d 11m 12d 12m"),
            sorted(KeyFormat::OpenKey).join(QLatin1Char(' ')));
    // Musical names keep the Open Key wheel (C, Am, G, Em ...), as Mixxx sorts them.
    EXPECT_EQ(QStringLiteral("C Am G Em"), sorted(KeyFormat::Musical).mid(0, 4).join(QLatin1Char(' ')));
    EXPECT_EQ(0, decklab::keyWheelOrder(INVALID, KeyFormat::Lancelot));
    for (const ChromaticKey key : allKeys()) {
        // Mixxx's library sort uses the same order.
        EXPECT_EQ(KeyUtils::keyToCircleOfFifthsOrder(key, KeyUtils::KeyNotation::Lancelot),
                decklab::keyWheelOrder(key, KeyFormat::Lancelot));
    }
}

TEST(DeckLabBrowseMatchTest, KeyFormatKeepsMixxxsConfigValues) {
    EXPECT_EQ(KeyFormat::Lancelot, decklab::keyFormatFromConfig(QStringLiteral("Lancelot")));
    EXPECT_EQ(KeyFormat::OpenKey, decklab::keyFormatFromConfig(QStringLiteral("OpenKey")));
    EXPECT_EQ(KeyFormat::Musical, decklab::keyFormatFromConfig(QStringLiteral("Traditional")));
    // Missing or the desktop's other notations: the closest of the three.
    EXPECT_EQ(KeyFormat::Lancelot, decklab::keyFormatFromConfig(QString()));
    EXPECT_EQ(KeyFormat::Lancelot, decklab::keyFormatFromConfig(QStringLiteral("Custom")));
    // The desktop preferences' combined notations, as Mixxx writes them
    // (KEY_NOTATION_*_AND_TRADITIONAL).
    EXPECT_EQ(KeyFormat::Lancelot, decklab::keyFormatFromConfig(QStringLiteral("Lancelot/Traditional")));
    EXPECT_EQ(KeyFormat::OpenKey, decklab::keyFormatFromConfig(QStringLiteral("OpenKey/Traditional")));
    EXPECT_EQ(KeyFormat::Lancelot, decklab::keyFormatFromConfig(QStringLiteral("Junk")));
    for (const auto format : {KeyFormat::Lancelot, KeyFormat::OpenKey, KeyFormat::Musical}) {
        EXPECT_EQ(format, decklab::keyFormatFromConfig(decklab::keyFormatConfigValue(format)));
        // Mixxx's preferences read the same value as the same notation.
        EXPECT_EQ(decklab::keyFormatNotation(format),
                static_cast<int>(KeyUtils::keyNotationFromString(decklab::keyFormatConfigValue(format))));
    }
    EXPECT_EQ(QStringLiteral("Lancelot (8A)"), decklab::keyFormatName(KeyFormat::Lancelot));
    EXPECT_EQ(QStringLiteral("Open Key (1m)"), decklab::keyFormatName(KeyFormat::OpenKey));
    EXPECT_EQ(QStringLiteral("Musical (Am)"), decklab::keyFormatName(KeyFormat::Musical));
}

// Browse's KEY cell, the delegate WTrackTableView installs: the text each
// source hands the view (a rekordbox export's "Am" and "8A", a file's
// "F#m" and Open Key "1m", Mixxx's formatted key id, a note that is not a
// key), painted in the chosen format.
TEST(DeckLabBrowseTest, KeyCellPaintsEverySourcesKeyInTheChosenFormat) {
    QStandardItemModel model(6, 2);
    const QStringList stored{QStringLiteral("Am"), QStringLiteral("8A"), QStringLiteral("F#m"),
            QStringLiteral("1m"), KeyUtils::keyToString(C_MAJOR, KeyUtils::KeyNotation::OpenKey),
            QStringLiteral("Unknown")};
    for (int row = 0; row < stored.size(); ++row) {
        model.setData(model.index(row, 0), stored[row]);
        model.setData(model.index(row, 1), 120.0 + row);
    }
    QTableView view;
    view.setModel(&model);
    view.resize(400, 300);
    std::unique_ptr<QAbstractItemDelegate> cell(decklab::makeKeyCellDelegate(&view, 0, 1));
    const auto painted = [&](int row) {
        QStyleOptionViewItem option;
        option.initFrom(&view);
        option.widget = &view;
        option.rect = QRect(0, 0, 76, 44);
        option.font = view.font();
        option.state = QStyle::State_Enabled;
        return DeckLabPaintedText::of(*cell, option, model.index(row, 0)).join(QLatin1Char('|'));
    };
    {
        KeyFormatScope lancelot(KeyFormat::Lancelot);
        const QStringList expected{"8A", "8A", "11A", "8A", "8B", "Unknown"};
        for (int row = 0; row < expected.size(); ++row) {
            EXPECT_EQ(expected[row], painted(row)) << stored[row].toStdString();
        }
    }
    {
        KeyFormatScope musical(KeyFormat::Musical);
        const QStringList expected{QStringLiteral("Am"), QStringLiteral("Am"), QStringLiteral(u"F♯m"),
                QStringLiteral("Am"), QStringLiteral("C"), QStringLiteral("Unknown")};
        for (int row = 0; row < expected.size(); ++row) {
            EXPECT_EQ(expected[row], painted(row)) << stored[row].toStdString();
        }
    }
    {
        KeyFormatScope openKey(KeyFormat::OpenKey);
        EXPECT_EQ(QStringLiteral("1m"), painted(0));
        EXPECT_EQ(QStringLiteral("4m"), painted(2));
    }
}

// Browse › Key: values labelled in the chosen format round its wheel; a
// change of format relabels and reorders the shown list in place.
TEST(DeckLabBrowseTest, KeyValuesFollowTheChosenFormatAndItsWheel) {
    QStandardItemModel model(6, 1);
    const QStringList stored{QStringLiteral("Am"), QStringLiteral("8A"), QStringLiteral("F#m"),
            QStringLiteral("1d"), QStringLiteral("G#m"), QString()};
    for (int row = 0; row < stored.size(); ++row) {
        model.setData(model.index(row, 0), stored[row]);
    }
    QListWidget list;
    {
        KeyFormatScope lancelot(KeyFormat::Lancelot);
        const auto values = decklab::collectCategoryValues(model, 0, decklab::Category::Key);
        QStringList labels;
        for (const auto& value : values) {
            labels << value.label;
            auto* item = new QListWidgetItem(value.label, &list);
            item->setData(DeckLabListDelegate::QueryRole, value.query);
        }
        EXPECT_EQ(QStringLiteral("1A 8A 8B 11A No key"), labels.join(QLatin1Char(' ')));
        EXPECT_EQ(QStringLiteral("key:8A"), values[1].query);
        EXPECT_EQ(2, values[1].count); // "Am" and "8A"
    }
    list.setCurrentRow(1); // 8A
    {
        KeyFormatScope musical(KeyFormat::Musical);
        decklab::relabelKeyValues(list);
        QStringList labels;
        for (int row = 0; row < list.count(); ++row) {
            labels << list.item(row)->text();
        }
        // Open Key wheel: C (1d), Am (1m), F♯m (4m), G♯m (6m); queries unchanged.
        EXPECT_EQ(QString::fromUtf8("C Am F♯m G♯m No key"), labels.join(QLatin1Char(' ')));
        ASSERT_NE(nullptr, list.currentItem());
        EXPECT_EQ(QStringLiteral("Am"), list.currentItem()->text());
        EXPECT_EQ(QStringLiteral("key:8A"), list.currentItem()->data(DeckLabListDelegate::QueryRole).toString());
        const auto values = decklab::collectCategoryValues(model, 0, decklab::Category::Key);
        EXPECT_EQ(QStringLiteral("C"), values[0].label);
    }
}

// The track card's KEY chip: the track's own key first; a track Mixxx has
// no key for shows the key its row stores (a rekordbox export's "Am") in the
// chosen format, and text that is not a key as stored, never "—".
TEST(DeckLabBrowseTest, TrackCardShowsTheRowsKeyWhenTheTrackHasNone) {
    DeckLabTrackCard card;
    TrackPointer track = Track::newTemporary();
    {
        KeyFormatScope lancelot(KeyFormat::Lancelot);
        card.showTrack(track, QStringLiteral("Am"));
        EXPECT_EQ(QStringLiteral("8A"), card.keyText());
        card.showTrack(track, QStringLiteral("Unknown"));
        EXPECT_EQ(QStringLiteral("Unknown"), card.keyText());
        card.showTrack(track);
        EXPECT_EQ(QString(), card.keyText()); // painted as "—": nothing stored
        track->setKey(C_MAJOR, mixxx::track::io::key::USER);
        card.showTrack(track, QStringLiteral("Am"));
        EXPECT_EQ(QStringLiteral("8B"), card.keyText());
    }
    {
        KeyFormatScope musical(KeyFormat::Musical);
        EXPECT_EQ(QStringLiteral("C"), card.keyText()); // a format change is a repaint
    }
}

// A USB folder sorted by key goes round the wheel of the notation the
// library shows ([Library],key_notation), not alphabetically ("10A" before
// "1A", "Am" before "Ebm"); tags that are not keys come first, as in the
// library.
TEST(DeckLabBrowseTest, FolderTracksSortByKeyRoundTheWheel) {
    std::unique_ptr<ControlObject> owned;
    if (!ControlObject::getControl(mixxx::library::prefs::kKeyNotationConfigKey, ControlFlag::NoWarnIfMissing)) {
        owned = std::make_unique<ControlObject>(mixxx::library::prefs::kKeyNotationConfigKey);
    }
    auto* notation = ControlObject::getControl(mixxx::library::prefs::kKeyNotationConfigKey);
    ASSERT_NE(nullptr, notation);
    const double before = notation->get();
    DeckLabGeneratedFolder folder({QStringLiteral("Am"), QStringLiteral("F#m"), QStringLiteral("1d"),
            QStringLiteral("Ebm"), QStringLiteral("Unknown"), QStringLiteral("G#m"), QStringLiteral("12B")});
    ProxyTrackModel proxy(&folder);
    const auto keys = [&proxy] {
        QStringList shown;
        for (int row = 0; row < proxy.rowCount(); ++row) {
            shown << proxy.index(row, 1).data().toString();
        }
        return shown.join(QLatin1Char(' '));
    };
    notation->set(decklab::keyFormatNotation(KeyFormat::Lancelot));
    proxy.sort(1, Qt::AscendingOrder);
    // 1A 2A 8A 8B 11A 12B
    EXPECT_EQ(QStringLiteral("Unknown G#m Ebm Am 1d F#m 12B"), keys());
    proxy.sort(1, Qt::DescendingOrder);
    EXPECT_EQ(QStringLiteral("12B F#m 1d Am Ebm G#m Unknown"), keys());
    proxy.sort(1, Qt::AscendingOrder);
    EXPECT_EQ(QStringLiteral("Unknown G#m Ebm Am 1d F#m 12B"), keys());
    // A new notation, then the same column and order (what a format change
    // does to a table sorted by key): Qt would skip it, the rows still go
    // round the new wheel. 1d 1m 4m 5d 6m 7m
    notation->set(decklab::keyFormatNotation(KeyFormat::OpenKey));
    proxy.sort(1, Qt::AscendingOrder);
    EXPECT_EQ(QStringLiteral("Unknown 1d Am F#m 12B G#m Ebm"), keys());
    proxy.sort(1, Qt::AscendingOrder); // and again: nothing moves
    EXPECT_EQ(QStringLiteral("Unknown 1d Am F#m 12B G#m Ebm"), keys());
    notation->set(decklab::keyFormatNotation(KeyFormat::Musical)); // Open Key order too, as Mixxx
    proxy.sort(1, Qt::AscendingOrder);
    EXPECT_EQ(QStringLiteral("Unknown 1d Am F#m 12B G#m Ebm"), keys());
    notation->set(decklab::keyFormatNotation(KeyFormat::Lancelot));
    proxy.sort(1, Qt::AscendingOrder);
    EXPECT_EQ(QStringLiteral("Unknown G#m Ebm Am 1d F#m 12B"), keys());
    // Other columns still sort as text.
    proxy.sort(0, Qt::DescendingOrder);
    EXPECT_EQ(QStringLiteral("Generated Tone 6"), proxy.index(0, 0).data().toString());
    notation->set(before);
}

// A rekordbox USB's rows through the import's own table and INSERT
// (RekordboxFeature): each export key text ("Am" Classic, "8A" Alphanumeric)
// gets its key_id at import, so the row reads in the chosen format, sorts round
// the wheel (ColumnCache's CASE key_id, which failed on a table without it)
// and key: / Match filters select by key. Generated rows, an in-memory table.
TEST(DeckLabBrowseTest, RekordboxRowsReadSortAndFilterByKey) {
    std::unique_ptr<ControlObject> owned;
    if (!ControlObject::getControl(mixxx::library::prefs::kKeyNotationConfigKey, ControlFlag::NoWarnIfMissing)) {
        owned = std::make_unique<ControlObject>(mixxx::library::prefs::kKeyNotationConfigKey);
    }
    auto* notation = ControlObject::getControl(mixxx::library::prefs::kKeyNotationConfigKey);
    ASSERT_NE(nullptr, notation);
    const double before = notation->get();
    notation->set(decklab::keyFormatNotation(KeyFormat::Lancelot));
    const QString connection = QStringLiteral("decklab-r6-keys-rekordbox");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QStringLiteral(":memory:"));
        ASSERT_TRUE(database.open());
        ASSERT_TRUE(RekordboxFeature::createLibraryTableForTesting(database));
        const QStringList stored{QStringLiteral("Am"), QStringLiteral("8A"), QStringLiteral("F#m"),
                QStringLiteral("Ebm"), QStringLiteral("Unknown"), QString(), QStringLiteral("C")};
        for (int row = 0; row < stored.size(); ++row) {
            ASSERT_TRUE(RekordboxFeature::insertLibraryTrackForTesting(
                    database, row + 1, QStringLiteral("Generated Tone %1").arg(row), stored[row]))
                    << stored[row].toStdString();
        }
        // Every row imported, each with the key its text names (0: none).
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT key, key_id FROM rekordbox_library ORDER BY rb_id")));
        QList<int> ids;
        QStringList shown;
        QStringList musical;
        while (query.next()) {
            ids << query.value(1).toInt();
            // What the feature's model shows (KeyUtils' key-or-text rule),
            // then Browse's KEY cell (displayKey) in each format.
            const QString cell = KeyUtils::keyFromKeyTextAndIdFields(query.value(0), query.value(1)).toString();
            shown << decklab::displayKey(cell);
            KeyFormatScope scope(KeyFormat::Musical);
            musical << decklab::displayKey(cell);
        }
        EXPECT_EQ((QList<int>{A_MINOR, A_MINOR, F_SHARP_MINOR, E_FLAT_MINOR, INVALID, INVALID, C_MAJOR}), ids);
        EXPECT_EQ((QStringList{"8A", "8A", "11A", "2A", "Unknown", "", "8B"}), shown);
        EXPECT_EQ((QStringList{"Am", "Am", QStringLiteral(u"F♯m"), QStringLiteral(u"E♭m"), "Unknown", "", "C"}),
                musical);
        // The feature's columns carry key_id, so the library's key sort and
        // key filter apply. Sort as BaseSqlTableModel asks for it.
        const QStringList columns = RekordboxFeature::libraryColumns();
        ASSERT_TRUE(columns.contains(LIBRARYTABLE_KEY_ID));
        ColumnCache cache(columns);
        const auto keysInOrder = [&](const QString& where) {
            QSqlQuery sorted(database);
            const QString sql = QStringLiteral("SELECT key FROM rekordbox_library%1 ORDER BY %2, rb_id")
                                        .arg(where.isEmpty() ? QString() : QStringLiteral(" WHERE ") + where,
                                                cache.columnSortForFieldIndex(
                                                        cache.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY)));
            EXPECT_TRUE(sorted.exec(sql)) << sql.toStdString();
            QStringList keys;
            while (sorted.next()) {
                keys << sorted.value(0).toString();
            }
            return keys.join(QLatin1Char(','));
        };
        // Lancelot: no key, 2A, 8A, 8A, 8B, 11A.
        EXPECT_EQ(QStringLiteral("Unknown,,Ebm,Am,8A,C,F#m"), keysInOrder(QString()));
        // Open Key (and Musical): no key, 1d, 1m, 1m, 4m, 7m.
        notation->set(decklab::keyFormatNotation(KeyFormat::OpenKey));
        EXPECT_EQ(QStringLiteral("Unknown,,C,Am,8A,F#m,Ebm"), keysInOrder(QString()));
        notation->set(decklab::keyFormatNotation(KeyFormat::Lancelot));
        // key: and the Match filter, parsed as BaseTrackCache parses them.
        SearchQueryParser parser(nullptr, {LIBRARYTABLE_TITLE});
        parser.setAvailableColumns(columns);
        const auto where = [&parser](const QString& search) {
            return parser.parseQuery(search, QString())->toSql();
        };
        EXPECT_EQ(QStringLiteral("Am,8A"), keysInOrder(where(QStringLiteral("key:8A"))));
        EXPECT_EQ(QStringLiteral("Am,8A"), keysInOrder(where(QStringLiteral("key:Am"))));
        decklab::MatchReference reference;
        reference.deck = 1;
        reference.key = A_MINOR;
        // 8A, 8B, 7A and 9A: the two A minors and C major.
        EXPECT_EQ(QStringLiteral("Am,8A,C"), keysInOrder(where(decklab::matchFilterQuery(reference))));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    notation->set(before);
}
