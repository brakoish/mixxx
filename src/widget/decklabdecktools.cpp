// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabdecktools.h"

#include <QApplication>
#include <QBoxLayout>
#include <QGridLayout>
#include <QMetaObject>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "audio/frame.h"
#include "moc_decklabdecktools.cpp"
#include "track/beats.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "util/fpclassify.h"
#include "widget/decklabpads.h"

namespace {
using KeyAction = DeckLabKeySheet::Action;
using GridAction = DeckLabGridPanel::Action;

mixxx::track::io::key::ChromaticKey chromatic(double value) {
    return KeyUtils::keyFromNumericValue(value);
}

bool validKey(mixxx::track::io::key::ChromaticKey key) {
    return key != mixxx::track::io::key::INVALID;
}

// Hides a widget GRID mode covers. A touch on the readout gave it focus;
// hiding a focused widget would hand focus on with a keyboard focus frame
// (the other deck's readout lit up), so the focus is dropped first.
void cover(QWidget* widget) {
    if (!widget) {
        return;
    }
    if (QWidget* focus = QApplication::focusWidget(); focus && (focus == widget || widget->isAncestorOf(focus))) {
        focus->clearFocus();
    }
    widget->hide();
}
} // namespace

DeckLabDeckTools::DeckLabDeckTools(Host host, QGridLayout* deckGrid, QObject* parent)
        : QObject(parent),
          m_host(std::move(host)),
          m_deckGrid(deckGrid) {
    setObjectName(QStringLiteral("decklab-deck-tools"));
    // Key shift, key and grid lock changes refresh the readouts. Change
    // driven: nothing here runs on the display tick.
    if (m_host.watch) {
        for (int deck = 1; deck <= 4; ++deck) {
            m_host.watch(deck, QStringLiteral("pitch"), [this, deck](double) {
                if (m_host.refresh) m_host.refresh(deck);
            });
            m_host.watch(deck, QStringLiteral("bpmlock"), [this, deck](double) {
                if (m_host.refresh) m_host.refresh(deck);
            });
            // Another deck's key decides whether this one still matches it.
            m_host.watch(deck, QStringLiteral("key"), [this](double) {
                if (!m_host.refresh) return;
                for (int other = 1; other <= 4; ++other) m_host.refresh(other);
            });
        }
    }
}

DeckLabDeckTools::~DeckLabDeckTools() = default;

QString DeckLabDeckTools::shiftText(double semitones) {
    if (!util_isfinite(semitones) || std::abs(semitones) < 0.05) {
        return QString();
    }
    const QString sign = semitones > 0 ? QStringLiteral("+") : QString::fromUtf8("−");
    const double whole = std::round(semitones);
    if (std::abs(semitones - whole) < 0.05) {
        return sign + QString::number(static_cast<int>(std::abs(whole)));
    }
    return sign + QString::number(std::abs(semitones), 'f', 1);
}

bool DeckLabDeckTools::current(int deck) const {
    if (deck < 1 || deck > 4 || !m_host.track) {
        return false;
    }
    const TrackPointer track = m_host.track(deck);
    return track && (!m_host.shownTrack || track == m_host.shownTrack(deck)) &&
            m_host.read(deck, QStringLiteral("track_loaded")) > 0;
}

bool DeckLabDeckTools::sameTrack(int deck, const TrackWeakPointer& opened) const {
    const TrackPointer track = opened.lock();
    return track && current(deck) && track == m_host.track(deck);
}

void DeckLabDeckTools::pulse(int deck, const QString& item) {
    m_host.write(deck, item, 1);
    m_host.write(deck, item, 0);
}

int DeckLabDeckTools::keySyncTarget(int deck) const {
    const auto loaded = [this](int other) {
        return m_host.read(other, QStringLiteral("track_loaded")) > 0;
    };
    const auto tempo = [this, &loaded](int other) {
        return loaded(other) && m_host.read(other, QStringLiteral("bpm")) > 0;
    };
    const auto playing = [this](int other) {
        return m_host.read(other, QStringLiteral("play")) > 0;
    };
    for (int other = 1; other <= 4; ++other) {
        if (other != deck && loaded(other) && m_host.read(other, QStringLiteral("sync_leader")) > 0) {
            return other;
        }
    }
    for (int other = 1; other <= 4; ++other) {
        if (other != deck && tempo(other) && playing(other) && m_host.read(other, QStringLiteral("sync_enabled")) > 0) {
            return other;
        }
    }
    for (int other = 1; other <= 4; ++other) {
        if (other != deck && tempo(other) && playing(other)) {
            return other;
        }
    }
    for (int other = 1; other <= 4; ++other) {
        if (other != deck && tempo(other)) {
            return other;
        }
    }
    return 0;
}

bool DeckLabDeckTools::keysMatch(int deck, int target) const {
    // Mixxx's compatibility rule (the same, relative, a fourth or a fifth),
    // read from the engine's keys, which change at once.
    const auto own = chromatic(m_host.read(deck, QStringLiteral("key")));
    const auto other = chromatic(m_host.read(target, QStringLiteral("key")));
    return validKey(own) && validKey(other) && KeyUtils::getCompatibleKeys(other).contains(own);
}

void DeckLabDeckTools::ensureKeylock(int deck) {
    // A key set by hand stays put when the tempo moves.
    if (m_host.read(deck, QStringLiteral("keylock")) <= 0) {
        m_host.write(deck, QStringLiteral("keylock"), 1);
    }
}

// ----------------------------------------------------------------- key sheet

void DeckLabDeckTools::attachSide(int side, DeckLabPadBlock* pads) {
    if (side < 0 || side > 1 || !pads || m_sideSheets[side]) {
        return;
    }
    m_sidePads[side] = pads;
    DeckLabKeySheet::Host host;
    host.deck = [this, side] { return m_sideSheetDeck[side] && m_host.displayedDeck ? m_host.displayedDeck(side) : 0; };
    host.current = [this](int deck) { return current(deck); };
    host.act = [this](int deck, KeyAction action) { keyAction(deck, action); };
    auto* sheet = new DeckLabKeySheet(QStringLiteral("decklab-key-%1").arg(side + 1),
            DeckLabKeySheet::Shape::Pads,
            host);
    pads->setPanel(sheet);
    m_sideSheets[side] = sheet;
}

void DeckLabDeckTools::attachCard(int deck, QWidget* card, DeckLabDeckStrip* strip) {
    if (deck < 1 || deck > 4 || !card || m_cardSheets[deck - 1]) {
        return;
    }
    m_cards[deck - 1] = card;
    DeckLabKeySheet::Host host;
    host.deck = [this, deck] { return m_cardSheetOpen[deck - 1] ? deck : 0; };
    host.current = [this](int which) { return current(which); };
    host.act = [this](int which, KeyAction action) { keyAction(which, action); };
    auto* sheet = new DeckLabKeySheet(QStringLiteral("decklab-card-key-%1").arg(deck),
            DeckLabKeySheet::Shape::Card,
            host,
            card);
    // The card's lower rows: the overview (44) and transport (48) give way.
    sheet->setContentsMargins(4, 0, 4, 0);
    sheet->hide();
    if (auto* rows = qobject_cast<QBoxLayout*>(card->layout())) {
        rows->addWidget(sheet);
    }
    m_cardSheets[deck - 1] = sheet;
    if (strip) {
        // Touch-down on the KEY readout opens the sheet; holding the BPM
        // readout opens GRID mode.
        connect(strip, &DeckLabDeckStrip::keyTouched, this, [this](int which) { openKeySheet(which); });
        connect(strip, &DeckLabDeckStrip::bpmHeld, this, [this](int which) { enterGrid(which); });
    }
}

bool DeckLabDeckTools::keySheetOpen(int deck) const {
    if (deck < 1 || deck > 4) {
        return false;
    }
    return m_cardSheetOpen[deck - 1] || m_sideSheetDeck[0] == deck || m_sideSheetDeck[1] == deck;
}

void DeckLabDeckTools::openKeySheet(int deck) {
    if (deck < 1 || deck > 4 || (m_host.onDecks && !m_host.onDecks()) || !current(deck) || m_grid.deck == deck) {
        return;
    }
    const bool four = m_host.fourDecks && m_host.fourDecks();
    if (four) {
        auto* sheet = m_cardSheets[deck - 1];
        if (!sheet) {
            return;
        }
        if (m_cardSheetOpen[deck - 1]) {
            // A second touch on the KEY readout closes it, like a toggle.
            closeCardSheet(deck);
            return;
        }
        m_cardSheetOpen[deck - 1] = true;
        m_cardSheetTrack[deck - 1] = m_host.track(deck);
        sheet->setState(keyState(deck));
        if (QWidget* overview = m_host.overview ? m_host.overview(deck) : nullptr) {
            overview->hide();
        }
        if (QWidget* transport = m_host.cardTransport ? m_host.cardTransport(deck) : nullptr) {
            transport->hide();
        }
        sheet->show();
        return;
    }
    int side = -1;
    for (int which = 0; which < 2; ++which) {
        if (m_host.displayedDeck && m_host.displayedDeck(which) == deck) {
            side = which;
        }
    }
    if (side < 0 || !m_sidePads[side] || !m_sideSheets[side]) {
        return;
    }
    if (m_sideSheetDeck[side] == deck) {
        closeSideSheet(side);
        return;
    }
    m_sideSheetDeck[side] = deck;
    m_sideSheetTrack[side] = m_host.track(deck);
    m_sideSheets[side]->setState(keyState(deck));
    m_sidePads[side]->showPanel(true);
}

void DeckLabDeckTools::closeSideSheet(int side) {
    if (side < 0 || side > 1) {
        return;
    }
    m_sideSheetDeck[side] = 0;
    m_sideSheetTrack[side].reset();
    if (m_sidePads[side]) {
        m_sidePads[side]->showPanel(false);
    }
}

void DeckLabDeckTools::closeCardSheet(int deck) {
    if (deck < 1 || deck > 4 || !m_cardSheetOpen[deck - 1]) {
        return;
    }
    m_cardSheetOpen[deck - 1] = false;
    m_cardSheetTrack[deck - 1].reset();
    if (auto* sheet = m_cardSheets[deck - 1]) {
        sheet->hide();
    }
    if (QWidget* overview = m_host.overview ? m_host.overview(deck) : nullptr) {
        overview->show();
    }
    if (QWidget* transport = m_host.cardTransport ? m_host.cardTransport(deck) : nullptr) {
        transport->setVisible(m_host.fourDecks && m_host.fourDecks());
    }
}

void DeckLabDeckTools::closeKeySheets() {
    closeSideSheet(0);
    closeSideSheet(1);
    for (int deck = 1; deck <= 4; ++deck) {
        closeCardSheet(deck);
    }
}

void DeckLabDeckTools::keyAction(int deck, KeyAction action) {
    if (action == KeyAction::Done) {
        for (int side = 0; side < 2; ++side) {
            if (m_sideSheetDeck[side] == deck) {
                closeSideSheet(side);
            }
        }
        closeCardSheet(deck);
        return;
    }
    if (!current(deck)) {
        return;
    }
    switch (action) {
    case KeyAction::Down:
    case KeyAction::Up:
        // Mixxx's semitone steps (the pitch knob's step count is 12).
        ensureKeylock(deck);
        pulse(deck, action == KeyAction::Up ? QStringLiteral("pitch_up") : QStringLiteral("pitch_down"));
        m_matches[deck - 1] = Match();
        break;
    case KeyAction::Match: {
        // Mixxx's KEY SYNC: the nearest key that mixes with the target's.
        ensureKeylock(deck);
        const int target = keySyncTarget(deck);
        pulse(deck, QStringLiteral("sync_key"));
        m_matches[deck - 1] = Match();
        if (target > 0 && keysMatch(deck, target)) {
            m_matches[deck - 1] = {target, m_host.read(deck, QStringLiteral("pitch")), m_host.track(deck)};
        }
        break;
    }
    case KeyAction::Reset:
        pulse(deck, QStringLiteral("reset_key"));
        m_matches[deck - 1] = Match();
        break;
    case KeyAction::KeyLock:
        m_host.write(deck, QStringLiteral("keylock"), m_host.read(deck, QStringLiteral("keylock")) > 0 ? 0 : 1);
        break;
    case KeyAction::Done:
        break;
    }
    if (m_host.refresh) {
        m_host.refresh(deck);
    }
}

void DeckLabDeckTools::fillKey(int deck, DeckLabStripState& state) {
    if (deck < 1 || deck > 4) {
        return;
    }
    state.keyShift.clear();
    state.keyMatched = false;
    Match& match = m_matches[deck - 1];
    if (!state.loaded) {
        match = Match();
        return;
    }
    const double pitch = m_host.read(deck, QStringLiteral("pitch"));
    state.keyShift = shiftText(pitch);
    if (match.target > 0) {
        // Still the matched key: the same track, the shift MATCH set, and
        // the target still loaded in a key that mixes with this one.
        const TrackPointer matched = match.track.lock();
        const bool holds = matched && matched == m_host.track(deck) && util_isfinite(pitch) &&
                std::abs(pitch - match.pitch) < 0.02 &&
                m_host.read(match.target, QStringLiteral("track_loaded")) > 0 && keysMatch(deck, match.target);
        if (holds) {
            state.keyMatched = true;
        } else {
            match = Match();
        }
    }
}

const DeckLabStripState& DeckLabDeckTools::strip(int deck) const {
    static const DeckLabStripState empty;
    return m_host.stripState && deck >= 1 && deck <= 4 ? m_host.stripState(deck) : empty;
}

DeckLabKeySheetState DeckLabDeckTools::keyState(int deck) const {
    const DeckLabStripState& strip = this->strip(deck);
    DeckLabKeySheetState state;
    state.deck = deck;
    state.loaded = strip.loaded && current(deck);
    if (!state.loaded) {
        return state;
    }
    state.hasKey = validKey(chromatic(m_host.read(deck, QStringLiteral("file_key"))));
    state.keylock = m_host.read(deck, QStringLiteral("keylock")) > 0;
    state.target = keySyncTarget(deck);
    if (state.target > 0) {
        const auto key = chromatic(m_host.read(state.target, QStringLiteral("key")));
        if (validKey(key)) {
            state.targetKey = KeyUtils::keyToString(key);
        }
    }
    state.key = strip.key;
    state.shift = strip.keyShift;
    state.matched = strip.keyMatched;
    return state;
}

// ----------------------------------------------------------------- GRID mode

quint64 DeckLabDeckTools::layoutSignature() const {
    return quint64(m_grid.deck & 7) << 41 | quint64(m_grid.four) << 44;
}

void DeckLabDeckTools::enterGrid(int deck) {
    if (deck < 1 || deck > 4 || (m_host.onDecks && !m_host.onDecks()) || !current(deck)) {
        return;
    }
    if (m_grid.deck == deck) {
        return;
    }
    if (m_grid.deck) {
        leaveGrid(false);
    }
    if (!m_gridPanel) {
        DeckLabGridPanel::Host host;
        host.current = [this](int which) { return which == m_grid.deck && current(which); };
        host.act = [this](int which, GridAction action) { gridAction(which, action); };
        m_gridPanel = new DeckLabGridPanel(QStringLiteral("decklab-grid-mode"), host,
                m_deckGrid ? m_deckGrid->parentWidget() : nullptr);
        m_gridPanel->hide();
    }
    m_grid.deck = deck;
    m_grid.track = m_host.track(deck);
    m_grid.four = m_host.fourDecks && m_host.fourDecks();
    // A closer look at the grid while editing: Mixxx's zoom control only.
    m_grid.zoomBefore = m_host.read(deck, QStringLiteral("waveform_zoom"));
    if (util_isfinite(m_grid.zoomBefore) && m_grid.zoomBefore > kGridZoom) {
        m_host.write(deck, QStringLiteral("waveform_zoom"), kGridZoom);
    } else {
        m_grid.zoomBefore = 0;
    }
    // The deck's own key sheet gives way to GRID mode.
    for (int side = 0; side < 2; ++side) {
        if (m_sideSheetDeck[side] == deck) {
            closeSideSheet(side);
        }
    }
    closeCardSheet(deck);
    m_gridPanel->setKnob(DeckLabGridPanel::Knob::Nudge);
    m_gridPanel->setState(gridState(deck));
    if (m_host.relayout) {
        m_host.relayout();
    }
}

void DeckLabDeckTools::leaveGrid(bool relayoutNow) {
    if (!m_grid.deck) {
        return;
    }
    const GridSession session = m_grid;
    m_grid = GridSession();
    if (m_gridPanel) {
        if (m_deckGrid) {
            m_deckGrid->removeWidget(m_gridPanel);
        }
        m_gridPanel->hide();
    }
    // The zoom comes back unless it was changed meanwhile.
    if (session.zoomBefore > 0 &&
            std::abs(m_host.read(session.deck, QStringLiteral("waveform_zoom")) - kGridZoom) < 1e-6) {
        m_host.write(session.deck, QStringLiteral("waveform_zoom"), session.zoomBefore);
    }
    if (!m_host.relayout) {
        return;
    }
    if (relayoutNow) {
        m_host.relayout();
    } else {
        QMetaObject::invokeMethod(
                this, [this] { if (m_host.relayout) m_host.relayout(); }, Qt::QueuedConnection);
    }
}

int DeckLabDeckTools::sharedBusyDeck(int deck) const {
    const TrackPointer track = m_host.track(deck);
    if (!track) {
        return 0;
    }
    for (int other = 1; other <= 4; ++other) {
        if (other == deck || m_host.track(other) != track) {
            continue;
        }
        if (m_host.read(other, QStringLiteral("play")) > 0 || m_host.read(other, QStringLiteral("sync_enabled")) > 0 ||
                m_host.read(other, QStringLiteral("sync_leader")) > 0) {
            return other;
        }
    }
    return 0;
}

bool DeckLabDeckTools::gridEditable(int deck) const {
    const TrackPointer track = m_host.track(deck);
    return track && track->getBeats() && !track->isBpmLocked() &&
            m_host.read(deck, QStringLiteral("sync_enabled")) <= 0 &&
            m_host.read(deck, QStringLiteral("sync_leader")) <= 0 && sharedBusyDeck(deck) == 0;
}

void DeckLabDeckTools::gridAction(int deck, GridAction action) {
    if (action == GridAction::Done) {
        leaveGrid(true);
        return;
    }
    if (deck != m_grid.deck || !current(deck)) {
        return;
    }
    const TrackPointer track = m_host.track(deck);
    if (action == GridAction::Lock) {
        // bpmlock requests toggle the track's lock (BpmControl).
        if (sharedBusyDeck(deck) == 0) {
            m_host.write(deck, QStringLiteral("bpmlock"), m_host.read(deck, QStringLiteral("bpmlock")) > 0 ? 0 : 1);
        }
    } else if (gridEditable(deck)) {
        // Mixxx's beatgrid controls; their rules (BPM lock, rounding,
        // constant-tempo moves) are Mixxx's.
        const auto slipBeats = [&track](double beats) {
            if (const auto grid = track->getBeats()) {
                if (const auto moved = grid->tryTranslateBeats(beats)) {
                    track->trySetBeats(*moved);
                }
            }
        };
        switch (action) {
        case GridAction::NudgeEarlier:
            pulse(deck, QStringLiteral("beats_translate_earlier"));
            break;
        case GridAction::NudgeLater:
            pulse(deck, QStringLiteral("beats_translate_later"));
            break;
        case GridAction::BeatEarlier:
            slipBeats(-1);
            break;
        case GridAction::BeatLater:
            slipBeats(1);
            break;
        case GridAction::BpmDownCoarse:
        case GridAction::BpmUpCoarse:
            for (int step = 0; step < 10; ++step) {
                pulse(deck,
                        action == GridAction::BpmUpCoarse ? QStringLiteral("beats_adjust_faster")
                                                          : QStringLiteral("beats_adjust_slower"));
            }
            break;
        case GridAction::BpmDownFine:
            pulse(deck, QStringLiteral("beats_adjust_slower"));
            break;
        case GridAction::BpmUpFine:
            pulse(deck, QStringLiteral("beats_adjust_faster"));
            break;
        case GridAction::Halve:
            pulse(deck, QStringLiteral("beats_set_halve"));
            break;
        case GridAction::Double:
            pulse(deck, QStringLiteral("beats_set_double"));
            break;
        case GridAction::Tap:
            pulse(deck, QStringLiteral("bpm_tap"));
            break;
        case GridAction::Downbeat: {
            // A beat at the playhead, then whole-beat slips so that beat is
            // beat 1 of the count the phase meter shows.
            pulse(deck, QStringLiteral("beats_translate_curpos"));
            const auto grid = track->getBeats();
            const double samples = m_host.read(deck, QStringLiteral("track_samples"));
            const double position = m_host.read(deck, QStringLiteral("playposition"));
            if (grid && grid->hasConstantTempo() && util_isfinite(position) && samples > 0) {
                const auto frame = mixxx::audio::FramePos::fromEngineSamplePos(position * samples);
                const auto beat = grid->findClosestBeat(frame);
                if (frame.isValid() && beat.isValid()) {
                    const long long index = grid->iteratorFrom(beat) - grid->cfirstmarker();
                    const int offset = static_cast<int>(((index % 4) + 4) % 4);
                    if (offset != 0) {
                        slipBeats(offset == 3 ? -1 : offset);
                    }
                }
            }
            break;
        }
        case GridAction::Lock:
        case GridAction::Done:
        case GridAction::Count:
            break;
        }
    }
    if (m_host.refresh) {
        m_host.refresh(deck);
    }
}

DeckLabGridState DeckLabDeckTools::gridState(int deck) const {
    const DeckLabStripState& strip = this->strip(deck);
    DeckLabGridState state;
    state.deck = deck;
    const TrackPointer track = m_host.track(deck);
    state.loaded = track && m_host.read(deck, QStringLiteral("track_loaded")) > 0;
    if (!state.loaded) {
        return state;
    }
    state.playing = m_host.read(deck, QStringLiteral("play")) > 0;
    const auto grid = track->getBeats();
    state.hasBeats = static_cast<bool>(grid);
    state.constantTempo = grid && grid->hasConstantTempo();
    state.locked = track->isBpmLocked();
    state.synced = m_host.read(deck, QStringLiteral("sync_enabled")) > 0 ||
            m_host.read(deck, QStringLiteral("sync_leader")) > 0;
    state.sharedDeck = sharedBusyDeck(deck);
    const double bpm = m_host.read(deck, QStringLiteral("local_bpm"));
    if (util_isfinite(bpm) && bpm > 0) {
        state.bpm = QString::number(bpm, 'f', 2);
    }
    state.beat = strip.loaded ? strip.phase : -1;
    return state;
}

// --------------------------------------------------------- surface hooks

void DeckLabDeckTools::showState(int deck) {
    if (deck < 1 || deck > 4) {
        return;
    }
    if (!m_grid.deck && !m_sideSheetDeck[0] && !m_sideSheetDeck[1] &&
            std::none_of(m_cardSheetOpen.cbegin(), m_cardSheetOpen.cend(), [](bool open) { return open; })) {
        return;
    }
    // A sheet's target and GRID mode's busy alias depend on the other decks
    // too: any deck's refresh updates what is open (at most three widgets,
    // each repainting only what changed).
    refreshOpen();
}

void DeckLabDeckTools::refreshOpen() {
    for (int side = 0; side < 2; ++side) {
        const int deck = m_sideSheetDeck[side];
        if (!deck || !m_sideSheets[side]) {
            continue;
        }
        if (!sameTrack(deck, m_sideSheetTrack[side])) {
            // A new track (or none) closes the sheet: no stale touch.
            closeSideSheet(side);
        } else {
            m_sideSheets[side]->setState(keyState(deck));
        }
    }
    for (int deck = 1; deck <= 4; ++deck) {
        if (!m_cardSheetOpen[deck - 1]) {
            continue;
        }
        if (!sameTrack(deck, m_cardSheetTrack[deck - 1])) {
            closeCardSheet(deck);
        } else {
            m_cardSheets[deck - 1]->setState(keyState(deck));
        }
    }
    if (const int deck = m_grid.deck; deck && m_gridPanel) {
        if (!sameTrack(deck, m_grid.track)) {
            // A new track on the edited deck ends GRID mode.
            leaveGrid(false);
        } else {
            m_gridPanel->setState(gridState(deck));
        }
    }
}

bool DeckLabDeckTools::validate() {
    const bool decks = !m_host.onDecks || m_host.onDecks();
    const bool four = m_host.fourDecks && m_host.fourDecks();
    for (int side = 0; side < 2; ++side) {
        const int deck = m_sideSheetDeck[side];
        if (deck && (!decks || four || !m_host.displayedDeck || m_host.displayedDeck(side) != deck ||
                            !sameTrack(deck, m_sideSheetTrack[side]))) {
            closeSideSheet(side);
        }
    }
    for (int deck = 1; deck <= 4; ++deck) {
        if (m_cardSheetOpen[deck - 1] && (!decks || !four || !sameTrack(deck, m_cardSheetTrack[deck - 1]))) {
            closeCardSheet(deck);
        }
    }
    if (!m_grid.deck) {
        return false;
    }
    const int deck = m_grid.deck;
    bool shown = decks && four == m_grid.four;
    if (shown && !four) {
        shown = m_host.displayedDeck && (m_host.displayedDeck(0) == deck || m_host.displayedDeck(1) == deck);
    }
    if (!shown || !sameTrack(deck, m_grid.track)) {
        leaveGrid(false);
        return false;
    }
    return true;
}

void DeckLabDeckTools::applyLayout() {
    if (m_gridPanel && m_deckGrid) {
        m_deckGrid->removeWidget(m_gridPanel);
    }
    const bool gridOn = validate();
    const bool four = m_host.fourDecks && m_host.fourDecks();
    // Open card sheets keep their place over the overview and transport,
    // which the relayout has just shown again.
    for (int deck = 1; deck <= 4; ++deck) {
        if (!m_cardSheetOpen[deck - 1]) {
            continue;
        }
        if (QWidget* overview = m_host.overview ? m_host.overview(deck) : nullptr) {
            overview->hide();
        }
        if (QWidget* transport = m_host.cardTransport ? m_host.cardTransport(deck) : nullptr) {
            transport->hide();
        }
    }
    if (!gridOn || !m_gridPanel || !m_deckGrid) {
        if (m_gridPanel) {
            m_gridPanel->hide();
        }
        return;
    }
    const int deck = m_grid.deck;
    if (four) {
        // The half of the deck row that holds the deck.
        const int first = (deck - 1) / 2 * 2;
        for (int column = first; column < first + 2; ++column) {
            cover(m_cards[column]);
            closeCardSheet(column + 1);
        }
        m_deckGrid->addWidget(m_gridPanel, 0, first, 1, 2);
    } else {
        // The deck's column: its readout, overview and pads.
        const int side = m_host.displayedDeck(0) == deck ? 0 : 1;
        cover(m_cards[deck - 1]);
        closeSideSheet(side);
        cover(m_sidePads[side]);
        m_deckGrid->addWidget(m_gridPanel, 0, side, 2, 1);
    }
    m_gridPanel->setState(gridState(deck));
    m_gridPanel->show();
    m_gridPanel->raise();
}
