// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabsmoke.h"
#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSqlQuery> // r5-history
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <memory>
#include "control/controlobject.h"
#include "decklab/decklabdclient.h"
#include "decklab/decklabfakeservice.h"
#include "decklab/decklabrail.h"
#include "decklab/decklabsettings.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabsystempages.h"
#include "decklab/decklabsystemui.h"
#include "decklab/decklabui.h"
#include "library/decklabknob.h"
#include "library/coverartutils.h"
#include "library/library.h"
#include "library/sidebarmodel.h"
#include "library/dao/playlistdao.h" // r5-history
#include "library/playlisttablemodel.h" // r5-history
#include "library/trackcollection.h" // r5-history
#include "library/trackcollectionmanager.h"
#include "library/trackmodel.h"
#include "mixer/playerinfo.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/cmdlineargs.h"
#include "widget/decklabbrowse.h"
#include "widget/decklabsampler.h"
#include "widget/decklabdecktools.h" // r5-decks
#include "widget/decklabgridmode.h"
#include "widget/decklabkeysheet.h"
#include "widget/decklabbrowsemix.h"
#include "widget/decklabbrowsedock.h" // r5-history
#include "widget/decklabhistory.h"    // r5-history
#include "widget/wdecklab.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wtracktableview.h"
#include "widget/woverview.h"

namespace {
// Deck Lab system pages, the first-run flow and banners, captured against a
// canned decklabd on a private socket so the pages show real content. The
// host's own decklabd and devices are never touched.
constexpr int kSystemFirst = 21;
// r4-system: +2 (Settings driven by the knob, a rail notice over Decks).
constexpr int kSystemSteps = 27;
constexpr int kLargeFirst = kSystemFirst + 20; // two 1024x600 captures
constexpr int kLargeCount = 2;
constexpr int kSheetFirst = kLargeFirst + kLargeCount; // Deck Lab sheets
constexpr int kSheetCount = 3;

void clickVisible(QWidget* root, const QString& name, const QString& text = QString()) {
    for (auto* button : root->findChildren<QPushButton*>(name)) {
        if (button->isVisibleTo(root) && (text.isEmpty() || button->text() == text)) {
            button->click();
            return;
        }
    }
}

// r4-labfx: Lab FX is one page. Its five captures (app-screen-05 to 09) show
// ECHO on deck 1, FILTER with the MID band out, ROLL, DUCK on MASTER and the
// SMP link to the Sampler page. Every deck is empty here: nothing sounds.
void labFxSmokeStep(QWidget* surface, int index) {
    const auto press = [surface](const char* name) { clickVisible(surface, QString::fromLatin1(name)); };
    switch (index) {
    case 0: press("decklab-fx-effect-0"); press("decklab-fx-target-1"); break;
    case 1: press("decklab-fx-effect-2"); press("decklab-fx-band-1"); break;
    case 2: press("decklab-fx-band-1"); press("decklab-fx-effect-3"); break;
    case 3: press("decklab-fx-effect-4"); press("decklab-fx-target-5"); break;
    default: press("decklab-fx-effect-0"); press("decklab-fx-target-6"); break;
    }
}

// Every control on the system pages, in Deck Setup, in Deck Lab sheets and in
// Settings (decklab-settings-*: category buttons, toggles and combo boxes) is
// a 44 px+ target in the real surface style, not just in unit tests.
void checkSystemTouchTargets(QWidget* surface, QJsonArray& failures) {
    QList<QWidget*> roots;
    for (const auto& name : {QStringLiteral("decklab-system-page"), QStringLiteral("decklab-setup-wizard"),
                 QStringLiteral("decklab-confirm"), QStringLiteral("decklab-settings"),
                 QStringLiteral("decklab-status-sheet"), QStringLiteral("decklab-rail")}) {
        roots.append(surface->window()->findChildren<QWidget*>(name));
    }
    for (auto* root : std::as_const(roots)) {
        if (!root->isVisible()) {
            continue;
        }
        QList<QWidget*> controls;
        for (auto* button : root->findChildren<QPushButton*>()) {
            controls.append(button);
        }
        for (auto* combo : root->findChildren<QComboBox*>()) {
            controls.append(combo);
        }
        for (auto* control : std::as_const(controls)) {
            if (control->isVisible() && control->height() < 44) {
                const auto* button = qobject_cast<QPushButton*>(control);
                failures.append(QStringLiteral("Small touch target %1 (%2 px)")
                                        .arg(!control->objectName().isEmpty() ? control->objectName()
                                                        : button                      ? button->text()
                                                                                      : root->objectName())
                                        .arg(control->height()));
            }
        }
    }
}

// Barlow everywhere: Settings, the System pages, Deck Setup and Deck Lab
// sheets must resolve the brand face, never the system fallback (DejaVu).
void checkBarlow(QWidget* surface, QJsonArray& failures) {
    QStringList wrong;
    for (const auto& name : {QStringLiteral("decklab-system-page"), QStringLiteral("decklab-setup-wizard"),
                 QStringLiteral("decklab-confirm"), QStringLiteral("decklab-settings")}) {
        for (auto* root : surface->window()->findChildren<QWidget*>(name)) {
            if (!root->isVisible()) {
                continue;
            }
            for (auto* widget : root->findChildren<QWidget*>()) {
                if (!widget->isVisible() || !(qobject_cast<QLabel*>(widget) || qobject_cast<QAbstractButton*>(widget) ||
                                                    qobject_cast<QComboBox*>(widget) || qobject_cast<QLineEdit*>(widget))) {
                    continue;
                }
                const QString family = QFontInfo(widget->font()).family();
                if (!family.startsWith(QStringLiteral("Barlow")) && wrong.size() < 5) {
                    wrong.append(QStringLiteral("%1 %2 uses %3")
                                         .arg(QString::fromLatin1(widget->metaObject()->className()),
                                                 widget->objectName(),
                                                 family));
                }
            }
        }
    }
    for (const auto& text : std::as_const(wrong)) {
        failures.append(QString(QStringLiteral("Not Barlow: ") + text));
    }
}

void closeSheets(QWidget* surface) {
    for (auto* sheet : surface->window()->findChildren<decklab::ui::Confirm*>()) {
        sheet->close();
    }
    for (auto* sheet : surface->window()->findChildren<DeckLabStatusSheet*>()) {
        sheet->hide();
    }
}

void systemSmokeStep(int index, WDeckLab* surface) {
    using Section = DeckLabSystemPage::Section;
    using Step = DeckLabSetupWizard::Step;
    auto* system = surface->window()->findChild<DeckLabSystem*>();
    auto* ui = surface->findChild<DeckLabSystemUi*>();
    if (index == 0 && system && !surface->findChild<DeckLabFakeService*>()) {
        auto* fake = new DeckLabFakeService(surface);
        const QString path = QDir(QDir::tempPath()).filePath(
                QStringLiteral("decklab-smoke-%1/decklabd.sock").arg(QCoreApplication::applicationPid()));
        fake->listen(path);
        system->client()->setSocketPath(path);
        using Kind = decklab::AudioDevice::Kind;
        const auto device = [](const QString& id, const QString& name, int outputs, Kind kind, bool controller) {
            decklab::AudioDevice entry;
            entry.id = id;
            entry.name = name;
            entry.outputs = outputs;
            entry.kind = kind;
            entry.controllerCard = controller;
            return entry;
        };
        system->setAudioDevicesOverride({device(QStringLiteral("DDJ-FLX4: USB Audio"), QStringLiteral("DDJ-FLX4"), 4, Kind::External, true),
                device(QStringLiteral("bcm2835 Headphones: -"), QStringLiteral("Deck Lab headphone jack"), 2, Kind::BuiltinJack, false),
                device(QStringLiteral("vc4-hdmi-0: MAI PCM i2s-hifi-0"), QStringLiteral("HDMI 1"), 2, Kind::Hdmi, false),
                device(QStringLiteral("pipewire"), QStringLiteral("System audio (Bluetooth)"), 2, Kind::Network, false)});
    }
    auto* page = surface->findChild<DeckLabSystemPage*>(QStringLiteral("decklab-system-page"));
    const auto settings = [surface, page](Section section) {
        surface->selectPage(4);
        if (auto* category = surface->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-5"))) {
            category->click();
        }
        if (page) {
            page->showSection(section);
        }
    };
    auto* wizard = ui ? ui->wizard() : nullptr;
    switch (index) {
    case 0: settings(Section::Home); break;
    case 1: settings(Section::Wifi); break;
    case 2:
        settings(Section::Wifi);
        clickVisible(surface, QStringLiteral("decklab-wifi-network"), QStringLiteral("Example-Network"));
        break;
    case 3: settings(Section::Bluetooth); break;
    case 4: settings(Section::Screen); break;
    case 5: settings(Section::Time); break;
    case 6:
        settings(Section::Time);
        clickVisible(surface, QStringLiteral("decklab-time-zone"));
        break;
    case 7: settings(Section::Storage); break;
    case 8: settings(Section::Power); break;
    case 9: settings(Section::About); break;
    case 10: settings(Section::Maintenance); break;
    case 11:
        surface->selectPage(0);
        if (ui) {
            ui->openFirstRun();
        }
        break;
    case 12:
    case 13:
    case 14:
        clickVisible(surface, QStringLiteral("decklab-setup-next"));
        break;
    case 15:
        if (wizard) {
            wizard->showMappingPicker();
        }
        break;
    case 16:
        if (wizard) {
            wizard->start(Step::Audio);
        }
        break;
    case 17:
        if (wizard) {
            wizard->start(Step::Layout);
        }
        break;
    case 18:
        if (wizard) {
            wizard->start(Step::Done);
        }
        break;
    case 19:
        clickVisible(surface, QStringLiteral("decklab-setup-close"));
        surface->selectPage(0);
        if (auto* fake = surface->findChild<DeckLabFakeService*>()) {
            fake->pushEvent(QStringLiteral("thermal.changed"),
                    QJsonObject{{QStringLiteral("temp_c"), 81.2}, {QStringLiteral("throttled"), QStringLiteral("0x20002")},
                            {QStringLiteral("undervoltage_now"), false}, {QStringLiteral("level"), QStringLiteral("hot")}});
        }
        break;
    case 20: settings(Section::Home); break;
    case 21:
        if (ui) {
            ui->openFirstRun();
        }
        break;
    case 22:
        // r4-system: the rail status sheet, one line per subsystem (with no
        // sound output the zone itself is the FIX button for Deck Setup).
        clickVisible(surface, QStringLiteral("decklab-setup-close"));
        surface->selectPage(0);
        if (auto* rail = surface->findChild<DeckLabRail*>()) {
            rail->openSheet();
        }
        break;
    case 23:
        // Making the read-only rekordbox stick writable asks first. The page
        // rebuilds its drive cards when shown, and new children show queued.
        closeSheets(surface);
        settings(Section::Storage);
        QTimer::singleShot(250, surface, [surface] { clickVisible(surface, QStringLiteral("decklab-drive-record")); });
        break;
    case 24:
        // A Bluetooth row tap opens details; it never connects or disconnects.
        closeSheets(surface);
        settings(Section::Bluetooth);
        QTimer::singleShot(250, surface, [surface] { clickVisible(surface, QStringLiteral("decklab-bt-device")); });
        break;
    case 25:
        // r4-system: Settings driven by the knob: two turns, a press (into
        // Headphones), a turn (the focus ring on the first row).
        closeSheets(surface);
        surface->selectPage(4);
        if (auto* page = surface->findChild<DeckLabSettingsPage*>()) {
            page->showCategory(DeckLabSettingsPage::Decks);
            QTimer::singleShot(150, surface, [] {
                if (QWidget* knob = decklab::knobClaimant()) {
                    for (int key : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Return, Qt::Key_Down}) {
                        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
                        QApplication::sendEvent(knob, &press);
                    }
                }
            });
        }
        break;
    case 26:
        // r4-system: system news slides into the rail, never over the page.
        surface->selectPage(0);
        if (ui) {
            ui->showOverlayProbe();
        }
        break;
    default: break;
    }
}
// Generated demo library for the Browse captures: invented titles and
// artists, silent audio and drawn artwork. No personal media is involved.
struct DemoTrack {
    const char* title;
    const char* artist;
    const char* album;
    double bpm;
    const char* key;
    int rating;
    unsigned color;
    const char* comment;
    double seconds;
};
const DemoTrack kDemoTracks[] = {
        {"Night Drive", "Demo Artist A", "Demo Album One", 122.0, "8A", 4, 0x2fb36d, "Warm-up, long intro", 392},
        {"Signal Fire", "Demo Artist B", "Demo Album Two", 124.0, "9A", 5, 0xd74448, "Peak-time roller", 361},
        {"Harbor Lights", "Demo Artist A", "Demo Album One", 121.0, "8B", 3, 0x5ab0ff, "", 415},
        {"Concrete Bloom", "Demo Artist C", "Demo Album Three", 130.0, "2A", 4, 0, "Breakdown at 3:10", 402},
        {"Glass Garden", "Demo Artist D", "Demo Album Four", 123.0, "7A", 5, 0xf5c542, "Vocal", 348},
        {"Late Checkout", "Demo Artist B", "Demo Album Two", 125.0, "9B", 3, 0, "", 377},
        {"Deep Current", "Demo Artist C", "Demo Album Three", 132.0, "11A", 2, 0, "", 420},
        {"Spiral Steps", "Demo Artist E", "Demo Album Five", 120.0, "8A", 4, 0x9b6bff, "Long outro for mixing", 455},
        {"Static Hearts", "Demo Artist F", "", 116.0, "4B", 3, 0, "", 301},
        {"Low Tide", "Demo Artist D", "Demo Album Four", 118.0, "5A", 4, 0, "", 333},
};

bool writeSilentWav(const QString& path, int seconds) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const quint32 rate = 8000;
    const quint16 channels = 1;
    const quint16 bits = 16;
    const quint32 bytes = rate * channels * (bits / 8) * static_cast<quint32>(seconds);
    QByteArray header;
    QDataStream out(&header, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4); out << quint32(36 + bytes); out.writeRawData("WAVE", 4);
    out.writeRawData("fmt ", 4);
    out << quint32(16) << quint16(1) << channels << rate << quint32(rate * channels * (bits / 8))
        << quint16(channels * (bits / 8)) << bits;
    out.writeRawData("data", 4); out << bytes;
    return file.write(header) == header.size() && file.write(QByteArray(int(bytes), '\0')) == int(bytes);
}

QImage demoCover(int n) {
    QImage image(256, 256, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, 256, 256);
    gradient.setColorAt(0, QColor::fromHsv((n * 47) % 360, 170, 210));
    gradient.setColorAt(1, QColor::fromHsv((n * 47 + 150) % 360, 200, 60));
    painter.fillRect(image.rect(), gradient);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 46));
    painter.drawEllipse(QPointF(60 + (n * 29) % 120, 190 - (n * 17) % 90), 92, 92);
    painter.setBrush(QColor(0, 0, 0, 70));
    painter.drawRect(QRectF(150, 24 + (n * 13) % 60, 70, 170));
    return image;
}

void seedDemoLibrary(Library* library, const QString& folder) {
    if (!library || !QDir().mkpath(folder)) return;
    int n = 0;
    for (const DemoTrack& demo : kDemoTracks) {
        const QString base = QDir(folder).filePath(QStringLiteral("%1 %2")
                        .arg(n + 1, 2, 10, QLatin1Char('0'))
                        .arg(QString::fromUtf8(demo.title)));
        if (!writeSilentWav(base + QStringLiteral(".wav"), 2)) continue;
        demoCover(n++).save(base + QStringLiteral(".png"));
        const TrackPointer track = library->trackCollectionManager()->getOrAddTrack(
                TrackRef::fromFilePath(base + QStringLiteral(".wav")));
        if (!track) continue;
        track->setTitle(QString::fromUtf8(demo.title));
        track->setArtist(QString::fromUtf8(demo.artist));
        track->setAlbum(QString::fromUtf8(demo.album));
        track->trySetBpm(demo.bpm);
        track->setKeyText(QString::fromUtf8(demo.key));
        track->setRating(demo.rating);
        if (demo.color) track->setColor(mixxx::RgbColor::optional(demo.color));
        track->setComment(QString::fromUtf8(demo.comment));
        track->setDuration(demo.seconds);
        CoverInfoGuesser().guessAndSetCoverInfoForTrack(track);
        library->trackCollectionManager()->saveTrack(track);
    }
    library->searchTracksInCollection(QString());
}

// A generated plain USB stick (no rekordbox export) in the smoke profile, so
// the source list shows a stick first. One silent track and one folder.
void seedDemoStick(const QString& root) {
    const QString stick = QDir(root).filePath(QStringLiteral("decklab/3A1F-0C2D"));
    if (!QDir().mkpath(QDir(stick).filePath(QStringLiteral("House")))) return;
    writeSilentWav(QDir(stick).filePath(QStringLiteral("Demo Stick Track.wav")), 2);
    decklab::setUsbStickRootsForTesting({QDir(root).filePath(QStringLiteral("decklab"))});
}

// r5-history: a generated history for the History captures: last night's
// set (two demo tracks, moved back a day) and this set (three, two of them
// on paused decks). Rows go into Mixxx's own history playlists as the setlog
// adds them when a deck becomes audible; nothing plays.
bool seedDemoHistory(Library* library) {
    if (!library || !library->trackCollectionManager()) return false;
    TrackCollection* collection = library->trackCollectionManager()->internalCollection();
    PlaylistDAO& playlists = collection->getPlaylistDAO();
    QSqlDatabase database = collection->database();
    const auto trackId = [&database](const char* title) {
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT id FROM library WHERE title = :title"));
        query.bindValue(QStringLiteral(":title"), QString::fromUtf8(title));
        return query.exec() && query.next() ? TrackId(query.value(0)) : TrackId();
    };
    const auto currentSet = [&database] {
        QSqlQuery query(database);
        return query.exec(QStringLiteral("SELECT MAX(id) FROM Playlists WHERE hidden = 2")) && query.next()
                ? query.value(0).toInt()
                : -1;
    };
    // Spread a set's times: its tracks were appended within one second.
    const auto retime = [&database](int set, const QString& shift) {
        QSqlQuery query(database);
        query.prepare(QStringLiteral("UPDATE PlaylistTracks SET pl_datetime_added = "
                                     "datetime(pl_datetime_added, :shift, '+' || (position * 7) || ' minutes') "
                                     "WHERE playlist_id = :id"));
        query.bindValue(QStringLiteral(":shift"), shift);
        query.bindValue(QStringLiteral(":id"), set);
        const bool tracks = query.exec();
        query.prepare(QStringLiteral("UPDATE Playlists SET date_created = datetime(date_created, :shift) WHERE id = :id"));
        query.bindValue(QStringLiteral(":shift"), shift);
        query.bindValue(QStringLiteral(":id"), set);
        return tracks && query.exec();
    };
    auto& history = decklab::history::Store::instance();
    history.ensureLoaded();
    const int lastNight = currentSet();
    if (lastNight < 0) return false;
    for (const char* title : {"Signal Fire", "Deep Current"}) {
        const TrackId id = trackId(title);
        if (!id.isValid() || !playlists.appendTrackToPlaylist(id, lastNight)) return false;
    }
    if (!history.startNewSet() || !retime(lastNight, QStringLiteral("-1 day"))) return false;
    const int tonight = currentSet();
    if (tonight == lastNight) return false;
    for (const char* title : {"Night Drive", "Glass Garden", "Harbor Lights"}) {
        const TrackId id = trackId(title);
        if (!id.isValid() || !playlists.appendTrackToPlaylist(id, tonight)) return false;
    }
    if (!retime(tonight, QStringLiteral("-40 minutes"))) return false;
    history.reload();
    return history.currentCount() == 3;
}

bool selectListItem(WDeckLab* surface, const QString& text) {
    auto* list = surface->findChild<QListWidget*>(QStringLiteral("decklab-browse-categories"));
    if (!list) return false;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->text() == text) {
            list->setCurrentRow(row);
            return true;
        }
    }
    return false;
}

bool selectTrack(WDeckLab* surface, const QString& title) {
    auto* library = surface->findChild<WLibrary*>();
    auto* table = library ? library->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    if (!tracks) return false;
    const int column = tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Title);
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, column).data().toString() == title) {
            table->selectRow(row);
            return true;
        }
    }
    return false;
}

struct BrowseCapture {
    QString name;
    int waitMs;
    std::function<bool()> action;
};

// r4-sampler: the Sampler page with generated demo samples loaded (never
// played) and its edit sheet open. Like the Browse captures they reuse the
// surface checks and do not change the reported screen count.
void captureSampler(WDeckLab* surface, const QString& output, const std::function<void()>& check,
        const std::shared_ptr<QJsonArray>& failures, const std::function<void()>& done) {
    auto captures = std::make_shared<QList<BrowseCapture>>(QList<BrowseCapture>{
            {QString(), 900, [surface] {
                 // Browse shows the demo collection: load three pads from it.
                 surface->selectPage(1);
                 bool loaded = true;
                 for (const auto& [sampler, title] : {std::pair{1, "Signal Fire"}, std::pair{2, "Glass Garden"},
                              std::pair{5, "Spiral Steps"}}) {
                     loaded = selectTrack(surface, QString::fromUtf8(title)) && loaded;
                     const ConfigKey load(QStringLiteral("[Sampler%1]").arg(sampler), QStringLiteral("LoadSelectedTrack"));
                     ControlObject::set(load, 1);
                     ControlObject::set(load, 0);
                 }
                 return loaded;
             }},
            {QStringLiteral("app-sampler-01-pads.png"), 500, [surface] {
                 surface->selectPage(2);
                 return surface->currentPage() == 2;
             }},
            {QStringLiteral("app-sampler-02-edit.png"), 400, [surface] {
                 auto* page = surface->findChild<DeckLabSampler*>(QStringLiteral("decklab-sampler"));
                 if (!page) return false;
                 page->editPad(2);
                 return page->sheetOpen();
             }},
    });
    auto index = std::make_shared<int>(0);
    auto next = std::make_shared<std::function<void()>>();
    *next = [=] {
        if (*index >= captures->size()) {
            if (auto* page = surface->findChild<DeckLabSampler*>(QStringLiteral("decklab-sampler"))) page->closeSheet();
            done();
            return;
        }
        const BrowseCapture capture = captures->at(*index);
        if (!capture.action()) failures->append(QString(QStringLiteral("Sampler capture could not reach: ") +
                (capture.name.isEmpty() ? QStringLiteral("loading") : capture.name)));
        QTimer::singleShot(capture.waitMs, surface, [=] {
            check();
            if (!capture.name.isEmpty() && !surface->grab().save(QDir(output).filePath(capture.name)))
                failures->append("Screenshot failed");
            ++*index;
            (*next)();
        });
    };
    (*next)();
}

// r5-decks: the key sheet (after MATCH) and GRID mode in both layouts, with
// two generated demo tracks paused on decks 1 and 2 (8A and 2A: MATCH takes
// deck 1 up a semitone to 3A). Like the Browse and Sampler captures they reuse
// the surface checks and do not change the reported screen count.
void captureDeckTools(WDeckLab* surface, const QString& output, const std::function<void()>& check,
        const std::shared_ptr<QJsonArray>& failures, const std::function<void()>& done) {
    auto* tools = surface->findChild<DeckLabDeckTools*>(QStringLiteral("decklab-deck-tools"));
    const auto layout = [surface](int index) {
        QMetaObject::invokeMethod(surface->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, index));
    };
    const auto shown = [surface](const QString& name) {
        auto* widget = surface->findChild<QWidget*>(name);
        return widget && widget->isVisible();
    };
    auto captures = std::make_shared<QList<BrowseCapture>>(QList<BrowseCapture>{
            {QString(), 900, [surface] {
                 // Deck 2 gets a second demo track (never played).
                 surface->selectPage(1);
                 if (!selectTrack(surface, QStringLiteral("Concrete Bloom"))) return false;
                 ControlObject::set(ConfigKey("[Channel2]", "LoadSelectedTrack"), 1);
                 ControlObject::set(ConfigKey("[Channel2]", "LoadSelectedTrack"), 0);
                 return true;
             }},
            {QStringLiteral("app-decks-key-sheet.png"), 700, [surface, tools, layout, shown] {
                 // Silent fixtures have no grid until loaded: their demo tempo.
                 for (const auto& [deck, bpm] : {std::pair{1, 122.0}, std::pair{2, 130.0}}) {
                     if (const TrackPointer loaded = PlayerInfo::instance().getTrackInfo(
                                 QStringLiteral("[Channel%1]").arg(deck))) {
                         loaded->trySetBpm(bpm);
                     }
                 }
                 layout(1);
                 surface->selectPage(0);
                 QCoreApplication::processEvents();
                 if (!tools) return false;
                 tools->openKeySheet(1);
                 return shown(QStringLiteral("decklab-key-1"));
             }},
            {QStringLiteral("app-decks-key-matched.png"), 700, [surface, shown] {
                 clickVisible(surface, QStringLiteral("decklab-key-1-match"));
                 return shown(QStringLiteral("decklab-key-1"));
             }},
            {QStringLiteral("app-decks-grid.png"), 700, [surface, tools, shown] {
                 clickVisible(surface, QStringLiteral("decklab-key-1-done"));
                 if (!tools) return false;
                 tools->enterGrid(1);
                 return shown(QStringLiteral("decklab-grid-mode")) && !shown(QStringLiteral("decklab-info-hit-1"));
             }},
            {QStringLiteral("app-decks-four-key-sheet.png"), 900, [surface, tools, layout, shown] {
                 if (!tools) return false;
                 tools->leaveGrid();
                 layout(2);
                 surface->selectPage(0);
                 QCoreApplication::processEvents();
                 tools->openKeySheet(2);
                 return shown(QStringLiteral("decklab-card-key-2"));
             }},
            {QStringLiteral("app-decks-four-grid.png"), 700, [surface, tools, shown] {
                 clickVisible(surface, QStringLiteral("decklab-card-key-2-done"));
                 if (!tools) return false;
                 tools->enterGrid(1);
                 return shown(QStringLiteral("decklab-grid-mode")) && !shown(QStringLiteral("decklab-card-1"));
             }},
    });
    auto index = std::make_shared<int>(0);
    auto next = std::make_shared<std::function<void()>>();
    *next = [=] {
        if (*index >= captures->size()) {
            if (tools) {
                tools->leaveGrid();
                tools->closeKeySheets();
            }
            done();
            return;
        }
        const BrowseCapture capture = captures->at(*index);
        if (!capture.action()) failures->append(QString(QStringLiteral("Decks capture could not reach: ") +
                (capture.name.isEmpty() ? QStringLiteral("loading") : capture.name)));
        QTimer::singleShot(capture.waitMs, surface, [=] {
            check();
            // Every key of the sheet and GRID mode is a 44 px target, and
            // neither crosses the waveform lanes (native GL windows).
            auto* zone = surface->findChild<QWidget*>(QStringLiteral("decklab-wave-zone"));
            const QRect lanes = zone ? QRect(zone->mapTo(surface, QPoint()), zone->size()) : QRect();
            QList<QWidget*> panels;
            for (auto* sheet : surface->findChildren<DeckLabKeySheet*>()) panels << sheet;
            for (auto* grid : surface->findChildren<DeckLabGridPanel*>()) panels << grid;
            for (auto* tool : std::as_const(panels)) {
                if (!tool->isVisible()) continue;
                if (QRect(tool->mapTo(surface, QPoint()), tool->size()).intersects(lanes))
                    failures->append(QString(QStringLiteral("Over the waveform lanes: ") + tool->objectName()));
                for (auto* button : tool->findChildren<QPushButton*>()) {
                    if (button->isVisible() && (button->height() < 44 || button->width() < 44))
                        failures->append(QStringLiteral("Small Decks touch target %1 (%2x%3)")
                                                 .arg(button->objectName())
                                                 .arg(button->width())
                                                 .arg(button->height()));
                }
            }
            if (!capture.name.isEmpty() && !surface->grab().save(QDir(output).filePath(capture.name)))
                failures->append("Screenshot failed");
            ++*index;
            (*next)();
        });
    };
    (*next)();
}
} // namespace

void startDeckLabSmoke(WDeckLab* surface, UserSettingsPointer config) {
    const QString output = qEnvironmentVariable("DECKLAB_SMOKE_DIR");
    if (output.isEmpty() || !surface || !CmdlineArgs::Instance().getDeveloper()) return;
    // Do not turn a forgotten test environment variable into automation on a
    // personal profile. The build recipe creates this separate scratch profile.
    if (!QDir(config->getSettingsPath()).dirName().startsWith("decklab-native-smoke")) {
        qCritical("Refusing smoke check outside its dedicated profile"); qApp->exit(70); return;
    }
    auto* timer = new QTimer(surface);
    auto step = std::make_shared<int>(0);
    auto failures = std::make_shared<QJsonArray>();
    auto browseDone = std::make_shared<bool>(false);
    auto samplerDone = std::make_shared<bool>(false); // r4-sampler
    auto decksDone = std::make_shared<bool>(false); // r5-decks
    // A step whose action outlasts the timer interval (seen under qemu) must not
    // be run again, or the next step's action is skipped: wait for its capture.
    auto capturing = std::make_shared<bool>(false);
    const auto check = [surface, failures] {
        if (surface->size() != QSize(800, 480)) failures->append("Surface is not 800x480");
        for (int deck = 1; deck <= 4; ++deck) {
            auto* overview = surface->findChild<WOverview*>(QStringLiteral("decklab-overview-%1").arg(deck));
            bool positionBound = false;
            // WBaseWidget owns these via unique_ptr, not the QObject child tree.
            // Its developer tooltip reports the actual display connection.
            if (overview) {
                QHelpEvent help(QEvent::ToolTip, QPoint(), overview->mapToGlobal(QPoint()));
                QCoreApplication::sendEvent(overview, &help); QToolTip::hideText();
                const QString prefix = QStringLiteral("DisplayConnection: [Channel%1],playposition ").arg(deck);
                for (const QString& line : overview->toolTip().split(QChar('\n')))
                    if (line.startsWith(prefix) && line.contains("Direction: TO_WIDGET")) positionBound = true;
            }
            if (!positionBound) failures->append(QStringLiteral("Deck %1 overview missing read-only playposition binding").arg(deck));
        }
        for (auto* b : surface->findChildren<QPushButton*>()) {
            if (!b->isVisible()) continue;
            QScrollArea* scroll = nullptr;
            for (auto* parent = b->parentWidget(); parent; parent = parent->parentWidget())
                if ((scroll = qobject_cast<QScrollArea*>(parent))) break;
            if (scroll) {
                scroll->ensureWidgetVisible(b, 0, 0); QCoreApplication::processEvents();
                if (!scroll->viewport()->rect().contains(QRect(b->mapTo(scroll->viewport(), QPoint()), b->size())))
                    failures->append(QString(QStringLiteral("Unreachable scroll control: ") + b->text()));
            }
            const QRect rect(b->mapTo(surface, QPoint()), b->size());
            if (!surface->rect().contains(rect)) failures->append(QString(QStringLiteral("Clipped button: ") + b->text()));
        }
        for (auto* scroll : surface->findChildren<QScrollArea*>()) {
            scroll->verticalScrollBar()->setValue(0); scroll->horizontalScrollBar()->setValue(0);
        }
        QCoreApplication::processEvents();
        // The smoke check may navigate/configure views, never start audio.
        for (int deck = 1; deck <= 4; ++deck) if (ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play"))) failures->append("Unexpected playing deck");
        for (int slot = 1; slot <= 16; ++slot) if (ControlObject::get(ConfigKey(QStringLiteral("[Sampler%1]").arg(slot), "play"))) failures->append("Unexpected playing sampler");
        for (int unit = 1; unit <= 2; ++unit)
            if (ControlObject::get(ConfigKey(QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit), "loaded")) != 1)
                failures->append("Bundled native RMX processor is unavailable");
    };
    QObject::connect(timer, &QTimer::timeout, surface, [=] {
        if (*capturing) return;
        // Capture the real parser/Library/PlayerManager surface, including the
        // empty native waveform viewers. These are not moving-audio evidence.
        if (*step < 5) surface->findChild<QPushButton*>(QStringLiteral("decklab-tab-%1").arg(*step))->click();
        else if (*step < 10) { surface->selectPage(3); labFxSmokeStep(surface, *step - 5); }
        else if (*step < 15) { surface->selectPage(4); surface->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-%1").arg(*step - 10))->click(); }
        else if (*step == 15) surface->openCollection();
        else if (*step == 16) surface->findChild<QPushButton*>("decklab-search")->click();
        else if (*step == 17 || *step == 18) {
            auto* layout=surface->findChild<QComboBox*>("decklab-deck-layout");
            QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,*step==17?2:0));
            surface->selectPage(0);
        }
        else if (*step >= kSystemFirst && *step < kSystemFirst + kSystemSteps) {
            if (*step == kSystemFirst) timer->setInterval(900);
            systemSmokeStep(*step - kSystemFirst, surface);
        }
        else if (*step == 19 || *step == 20) {
            auto* waves=surface->findChild<QComboBox*>("decklab-waveform-layout");
            waves->setCurrentIndex(1);
            QMetaObject::invokeMethod(waves,"activated",Q_ARG(int,1));
            auto* layout=surface->findChild<QComboBox*>("decklab-deck-layout");
            QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,*step==19?1:2));
            surface->selectPage(0);
        }
        else if (!*browseDone) {
            // Browse captures with a generated library. They reuse the same
            // checks and do not change the reported screen count.
            timer->stop();
            surface->setFixedSize(800, 480);
            // The system captures end on Deck Lab sheets and Settings; close
            // them so Browse is what gets captured.
            closeSheets(surface);
            clickVisible(surface, QStringLiteral("decklab-setup-close"));
            seedDemoLibrary(surface->library(), QDir(config->getSettingsPath()).filePath("decklab-demo-music"));
            seedDemoStick(QDir(config->getSettingsPath()).filePath("decklab-demo-media"));
            const auto press = [surface] { surface->browsePress(); return true; };
            auto captures = std::make_shared<QList<BrowseCapture>>(QList<BrowseCapture>{
                    {"sources", 500, [surface] {
                         QMetaObject::invokeMethod(surface->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 1));
                         // r4-browse (move 1): the rail's USB lands on the source
                         // home, a 2x2 grid of tiles, with the demo stick first and
                         // highlighted; the deck dock and the knob legend below.
                         clickVisible(surface, QStringLiteral("decklab-usb"));
                         QCoreApplication::processEvents();
                         auto* list = surface->findChild<QListWidget*>(QStringLiteral("decklab-browse-categories"));
                         return list && list->currentItem() &&
                                 list->currentItem()->text() == QStringLiteral("USB stick") &&
                                 list->currentItem()->data(DeckLabListDelegate::TileRole).toInt() == 1 &&
                                 surface->findChild<QWidget*>(QStringLiteral("decklab-deck-dock"))->isVisible();
                     }},
                    {"categories", 400, [surface, press] {
                         return selectListItem(surface, QStringLiteral("Library")) && press();
                     }},
                    {"artists", 400, [surface] {
                         if (!selectListItem(surface, decklab::categoryName(decklab::Category::Artist))) return false;
                         surface->browsePress();
                         return selectListItem(surface, QStringLiteral("Demo Artist B"));
                     }},
                    {"artist-tracks", 900, [surface] { surface->browsePress(); return selectTrack(surface, "Signal Fire"); }},
                    {"keys", 400, [surface] {
                         surface->browseBack(); surface->browseBack();
                         if (!selectListItem(surface, decklab::categoryName(decklab::Category::Key))) return false;
                         surface->browsePress();
                         return true;
                     }},
                    {"reference-deck", 1800, [surface] {
                         surface->browseBack();
                         if (!selectListItem(surface, decklab::categoryName(decklab::Category::AllTracks))) return false;
                         surface->browsePress();
                         if (!selectTrack(surface, "Night Drive")) return false;
                         // Loads (never plays) a paused reference deck for match highlighting.
                         ControlObject::set(ConfigKey("[Channel1]", "LoadSelectedTrack"), 1);
                         ControlObject::set(ConfigKey("[Channel1]", "LoadSelectedTrack"), 0);
                         return true;
                     }},
                    {"match", 900, [surface] {
                         // Silent fixture files have no beat grid until loaded; give the
                         // paused reference deck its generated tempo.
                         if (const TrackPointer loaded = PlayerInfo::instance().getTrackInfo(QStringLiteral("[Channel1]"))) {
                             loaded->trySetBpm(122.0);
                         }
                         surface->selectPage(1);
                         return selectTrack(surface, "Spiral Steps");
                     }},
                    {"match-bar", 900, [surface] {
                         // Faster tracks only, filtered: the Match bar and the
                         // breadcrumb say what is shown.
                         clickVisible(surface, QStringLiteral("decklab-match-faster"));
                         clickVisible(surface, QStringLiteral("decklab-match-only"));
                         QCoreApplication::processEvents();
                         // The capture shows whatever the filter kept.
                         selectTrack(surface, "Signal Fire");
                         return surface->findChild<QWidget*>(QStringLiteral("decklab-match-bar"))->isVisible();
                     }},
                    {"four-decks", 700, [surface] {
                         clickVisible(surface, QStringLiteral("decklab-match-only"));
                         clickVisible(surface, QStringLiteral("decklab-match-both"));
                         QMetaObject::invokeMethod(surface->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2));
                         surface->selectPage(1);
                         return selectTrack(surface, "Signal Fire");
                     }},
                    {"mixes-row", 600, [surface] {
                         // r4-browse (move 5): back on the Library's categories,
                         // "Mixes with deck 1" leads them in the deck colour.
                         surface->browseBack();
                         QCoreApplication::processEvents();
                         return selectListItem(surface, decklab::mixesTitle(1));
                     }},
                    {"mixes", 900, [surface] {
                         // Its list, closest first, the relation in words; the
                         // four-deck dock offers LOAD on every paused deck.
                         surface->browsePress();
                         QCoreApplication::processEvents();
                         auto* list = surface->findChild<QListWidget*>(QStringLiteral("decklab-browse-categories"));
                         if (!list || !list->count() || !list->item(0)->data(DeckLabListDelegate::MixRole).toBool()) return false;
                         list->setCurrentRow(std::min(1, list->count() - 1));
                         return true;
                     }},
                    {"history", 2400, [surface, failures] {
                         // r5-history: Glass Garden is loaded (paused) on deck 2,
                         // the generated history goes in (Mixxx's history
                         // playlists), and the home's History chip opens the sets:
                         // this set first, last night's under "Yesterday".
                         surface->browseBack();
                         if (!selectListItem(surface, decklab::categoryName(decklab::Category::AllTracks))) return false;
                         surface->browsePress();
                         if (!selectTrack(surface, "Glass Garden")) return false;
                         ControlObject::set(ConfigKey("[Channel2]", "LoadSelectedTrack"), 1);
                         ControlObject::set(ConfigKey("[Channel2]", "LoadSelectedTrack"), 0);
                         // A load returns to Decks; come back once it is in.
                         QTimer::singleShot(1000, surface, [surface, failures] {
                             if (!seedDemoHistory(surface->library())) failures->append(QStringLiteral("Demo history could not be seeded"));
                             surface->selectPage(1);
                             surface->browseBack();
                             surface->browseBack();
                             auto* list = surface->findChild<QListWidget*>(QStringLiteral("decklab-browse-categories"));
                             // The stick's files first, so the track table holds no
                             // history track when History opens.
                             // round5 merge: r5-polish names a lone unlabeled plain stick "USB stick".
                             if (!selectListItem(surface, QStringLiteral("USB stick"))) failures->append(QStringLiteral("No demo stick"));
                             surface->browsePress();
                             for (int row = 0; list && row < list->count(); ++row) {
                                 if (list->item(row)->data(DeckLabListDelegate::KeyRole).toString() == QStringLiteral("tracks")) list->setCurrentRow(row);
                             }
                             surface->browsePress();
                             // A moment on the stick's tracks, as a DJ would, then History.
                             QTimer::singleShot(500, surface, [surface, failures, list] {
                                 surface->browseBack();
                                 surface->browseBack();
                                 if (!selectListItem(surface, QStringLiteral("History"))) failures->append(QStringLiteral("No History on the home"));
                                 surface->browsePress();
                                 if (!list || list->count() < 3 || list->item(0)->text() != QStringLiteral("This set"))
                                     failures->append(QStringLiteral("History does not list this set first"));
                                 if (ControlObject::get(ConfigKey(QStringLiteral("[DeckLabNative]"), QStringLiteral("track_level"))) != 0)
                                     failures->append(QStringLiteral("History sets report track_level"));
                             });
                         });
                         return true;
                     }},
                    {"history-set", 1800, [surface, failures] {
                         // This set in play order (time, deck, BPM, key), on its
                         // latest track; the paused decks offer LOAD from history.
                         surface->browsePress();
                         QCoreApplication::processEvents();
                         auto* list = surface->findChild<QListWidget*>(QStringLiteral("decklab-browse-categories"));
                         if (!list || list->count() != 3) return false;
                         QTimer::singleShot(400, surface, [surface, failures, list] {
                             // LOAD and Preview read the set's own Mixxx history
                             // playlist, on the highlighted track (by library id).
                             auto* library = surface->findChild<WLibrary*>();
                             auto* table = library ? library->getCurrentTrackTableView() : nullptr;
                             const auto* playlist = table ? dynamic_cast<PlaylistTableModel*>(table->model()) : nullptr;
                             const QListWidgetItem* item = list->currentItem();
                             const QModelIndexList rows = table && table->selectionModel() ? table->selectionModel()->selectedRows() : QModelIndexList();
                             if (!item || !playlist || playlist->getPlaylist() != item->data(decklab::history::PlaylistRole).toInt())
                                 failures->append(QStringLiteral("History table is not the set's playlist"));
                             else if (rows.size() != 1 || playlist->getTrackId(rows.first()).toVariant().toInt() != item->data(decklab::history::TrackIdRole).toInt())
                                 failures->append(QStringLiteral("History selection is not the highlighted track"));
                             auto* tile = static_cast<DeckLabDockTile*>(surface->findChild<QPushButton*>(QStringLiteral("decklab-load-3")));
                             if (!tile || !tile->loadReady()) failures->append(QStringLiteral("History track not loadable from the dock"));
                             // r5-fix-history: a controller's LOAD on this list level
                             // ([DeckLabNative] track_level) loads exactly this row.
                             // The same LoadSelectedTrack path, into the preview deck
                             // (loaded, never played), so no deck layer changes.
                             if (ControlObject::get(ConfigKey(QStringLiteral("[DeckLabNative]"), QStringLiteral("track_level"))) != 1)
                                 failures->append(QStringLiteral("History track row does not report track_level"));
                             const int wanted = item ? item->data(decklab::history::TrackIdRole).toInt() : -1;
                             ControlObject::set(ConfigKey(QStringLiteral("[PreviewDeck1]"), QStringLiteral("LoadSelectedTrack")), 1);
                             ControlObject::set(ConfigKey(QStringLiteral("[PreviewDeck1]"), QStringLiteral("LoadSelectedTrack")), 0);
                             QTimer::singleShot(700, surface, [surface, failures, wanted] {
                                 const TrackPointer loaded = PlayerInfo::instance().getTrackInfo(QStringLiteral("[PreviewDeck1]"));
                                 if (!loaded || loaded->getId().toVariant().toInt() != wanted)
                                     failures->append(QStringLiteral("Controller LOAD from History loaded another track"));
                                 ControlObject::set(ConfigKey(QStringLiteral("[PreviewDeck1]"), QStringLiteral("eject")), 1);
                                 ControlObject::set(ConfigKey(QStringLiteral("[PreviewDeck1]"), QStringLiteral("eject")), 0);
                                 if (surface->currentPage() != 1) surface->selectPage(1);
                             });
                         });
                         return true;
                     }},
                    {"history-marks", 900, [surface] {
                         // Played marks in a track list: this set's rows dim with
                         // a check, last night's carry a quieter ring.
                         surface->browseBack();
                         surface->browseBack();
                         if (!selectListItem(surface, QStringLiteral("Library"))) return false;
                         surface->browsePress();
                         if (!selectListItem(surface, decklab::categoryName(decklab::Category::AllTracks))) return false;
                         surface->browsePress();
                         return selectTrack(surface, "Late Checkout");
                     }},
                    {"history-mixes", 900, [surface] {
                         // The marks reach "Mixes with deck 1" too. Browse ends
                         // on the Library, as before, for the Sampler captures.
                         surface->browseBack();
                         if (!selectListItem(surface, decklab::mixesTitle(1))) return false;
                         surface->browsePress();
                         auto* list = surface->findChild<QListWidget*>(QStringLiteral("decklab-browse-categories"));
                         if (!list || !list->count() || !list->item(0)->data(DeckLabListDelegate::MixRole).toBool()) return false;
                         list->setCurrentRow(0);
                         return true;
                     }},
            });
            auto index = std::make_shared<int>(0);
            auto next = std::make_shared<std::function<void()>>();
            *next = [=] {
                if (*index >= captures->size()) {
                    *browseDone = true;
                    timer->start(350);
                    return;
                }
                const BrowseCapture capture = captures->at(*index);
                if (!capture.action()) failures->append(QString(QStringLiteral("Browse capture could not reach: ") + capture.name));
                QTimer::singleShot(capture.waitMs, surface, [=] {
                    check();
                    // Browse touch targets keep 44 px in the real surface style.
                    if (auto* browse = surface->findChild<QWidget*>(QStringLiteral("decklab-browse"))) {
                        for (auto* button : browse->findChildren<QPushButton*>()) {
                            if (button->isVisible() && button->height() < 44) {
                                failures->append(QStringLiteral("Small Browse touch target %1 (%2 px)")
                                                         .arg(button->objectName())
                                                         .arg(button->height()));
                            }
                        }
                    }
                    const QString file = QStringLiteral("app-browse-%1-%2.png").arg(*index + 1, 2, 10, QLatin1Char('0')).arg(capture.name);
                    if (!surface->grab().save(QDir(output).filePath(file))) failures->append("Screenshot failed");
                    ++*index;
                    (*next)();
                });
            };
            (*next)();
            return;
        }
        else if (!*samplerDone) {
            // r4-sampler: Sampler captures after Browse (uncounted).
            timer->stop();
            captureSampler(surface, output, check, failures, [=] { *samplerDone = true; timer->start(350); });
            return;
        }
        else if (!*decksDone) {
            // r5-decks: key sheet and GRID mode captures (uncounted).
            timer->stop();
            captureDeckTools(surface, output, check, failures, [=] { *decksDone = true; timer->start(350); });
            return;
        }
        else {
            timer->stop();
            QFile file(QDir(output).filePath("native-smoke.json"));
            const bool opened = file.open(QIODevice::WriteOnly);
            int processors = 0;
            for (int unit = 1; unit <= 2; ++unit)
                processors += ControlObject::get(ConfigKey(QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit), "loaded")) == 1;
            if (opened) file.write(QJsonDocument(QJsonObject{{"screens", *step}, {"failures", *failures},
                    {"audio_started", false}, {"native_rmx_units_loaded", processors}}).toJson());
            qApp->exit(opened && failures->isEmpty() ? 0 : 71); return;
        }
        const bool large = *step >= kLargeFirst && *step < kLargeFirst + kLargeCount;
        surface->setFixedSize(large ? QSize(1024, 600) : QSize(800, 480));
        *capturing = true;
        QTimer::singleShot(*step >= kSystemFirst ? 600 : 100, surface, [=] {
            if (!large) check();
            if ((*step >= 10 && *step < 15) || *step >= kSystemFirst) {
                checkSystemTouchTargets(surface, *failures);
                checkBarlow(surface, *failures);
            }
            if (*step == 15 || *step == 16) {
                for (int deck = 1; deck <= 2; ++deck)
                    if (surface->findChild<QPushButton*>(QStringLiteral("decklab-load-%1").arg(deck))->isEnabled())
                        failures->append("Empty collection offers Load without a track selection");
            }
            // Sheets hosted on the main window are captured over the surface.
            const bool sheet = *step >= kSheetFirst && *step < kSheetFirst + kSheetCount;
            const QPixmap screen = sheet
                    ? surface->window()->grab(QRect(surface->mapTo(surface->window(), QPoint()), surface->size()))
                    : surface->grab();
            bool statusSheet = false;
            for (auto* status : surface->findChildren<DeckLabStatusSheet*>()) statusSheet |= status->isVisible();
            if (sheet && !surface->window()->findChild<decklab::ui::Confirm*>() && !statusSheet)
                failures->append(QStringLiteral("Deck Lab sheet missing on screen %1").arg(*step));
            if (!screen.save(QDir(output).filePath(QStringLiteral("app-screen-%1.png").arg(*step, 2, 10, QLatin1Char('0'))))) failures->append("Screenshot failed");
            if (*step == 18) {
                // Wider HDMI touch panels must scale cleanly, not break.
                surface->setFixedSize(1024, 600);
                for (int pass = 0; pass < 3; ++pass) QCoreApplication::processEvents();
                if (!surface->grab().save(QDir(output).filePath(QStringLiteral("app-decks-1024x600.png")))) failures->append("Screenshot failed");
                surface->setFixedSize(800, 480);
                for (int pass = 0; pass < 3; ++pass) QCoreApplication::processEvents();
            }
            ++*step;
            *capturing = false;
        });
    });
    timer->start(350);
}
