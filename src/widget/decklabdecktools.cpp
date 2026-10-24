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
    // Key shift, key, key lock and grid lock changes refresh the readouts.
    // Change driven: nothing here runs on the display tick.
    if (m_host.watch) {
        for (int deck = 1; deck <= 4; ++deck) {
            for (const auto& item : {QStringLiteral("pitch_adjust"), QStringLiteral("bpmlock")}) {
                m_host.watch(deck, item, [this, deck](double) {
                    if (m_host.refresh) m_host.refresh(deck);
                });
            }
            m_host.watch(deck, QStringLiteral("pitch"), [this, deck](double) {
                if (!m_host.refresh) return;
                m_host.refresh(deck);
                // A deck matched to this one checks whether it drifted.
                for (int other = 1; other <= 4; ++other) {
                    if (m_matches[other - 1].target == deck) m_host.refresh(other);
                }
            });
            m_host.watch(deck, QStringLiteral("keylock"), [this, deck](double on) {
                // Key lock turned off by anything: it is no longer the sheet's.
                if (on <= 0) {
                    m_keylockBySheet[deck - 1] = false;
                    m_keylockTrack[deck - 1].reset();
                }
            });
            // Another deck's key or pitch decides whether this one still
            // matches it.
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
        return QString(sign + QString::number(static_cast<int>(std::abs(whole))));
    }
    return QString(sign + QString::number(std::abs(semitones), 'f', 1));
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

bool DeckLabDeckTools::ensureKeylock(int deck) {
    // A key set by hand stays put when the tempo moves. The sheet remembers
    // that it turned key lock on, so RESET (or the next track) gives the
    // deck back as it was: Mixxx saves key lock.
    if (m_host.read(deck, QStringLiteral("keylock")) > 0) {
        return false;
    }
    m_host.write(deck, QStringLiteral("keylock"), 1);
    m_keylockBySheet[deck - 1] = true;
    m_keylockTrack[deck - 1] = m_host.track(deck);
    return true;
}

void DeckLabDeckTools::restoreKeylock(int deck) {
    if (!m_keylockBySheet[deck - 1]) {
        return;
    }
    m_keylockBySheet[deck - 1] = false;
    m_keylockTrack[deck - 1].reset();
    if (m_host.read(deck, QStringLiteral("keylock")) > 0) {
        m_host.write(deck, QStringLiteral("keylock"), 0);
    }
}

bool DeckLabDeckTools::keylockBySheet(int deck) const {
    return deck >= 1 && deck <= 4 && m_keylockBySheet[deck - 1];
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
        // The KEY readout opens the sheet (touch-down while paused, a short
        // hold while playing); holding a paused deck's BPM opens GRID mode.
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
    if (deck < 1 || deck > 4 || (m_host.onDecks && !m_host.onDecks()) || !current(deck)) {
        return;
    }
    if (m_grid.deck) {
        // One knob owner at a time: the sheet takes over from GRID mode.
        leaveGrid(Relayout::Now);
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
    case KeyAction::Up: {
        // A semitone from what plays now. With key lock off the heard pitch
        // includes the tempo: turning key lock on would snap it back to the
        // track's key, so the new pitch (heard ± 1) is written right after
        // key lock in one step, with nothing audible in between. With key
        // lock on, Mixxx's own semitone step (the pitch knob's 12 steps).
        const double step = action == KeyAction::Up ? 1 : -1;
        const double heard = m_host.read(deck, QStringLiteral("pitch"));
        if (ensureKeylock(deck)) {
            m_host.write(deck, QStringLiteral("pitch"), std::clamp((util_isfinite(heard) ? heard : 0) + step, -6.0, 6.0));
        } else {
            pulse(deck, action == KeyAction::Up ? QStringLiteral("pitch_up") : QStringLiteral("pitch_down"));
        }
        m_matches[deck - 1] = Match();
        break;
    }
    case KeyAction::Match: {
        // Mixxx's KEY SYNC: the nearest key that mixes with the target's,
        // from the track's own key (the heard pitch does not change it).
        ensureKeylock(deck);
        const int target = keySyncTarget(deck);
        pulse(deck, QStringLiteral("sync_key"));
        m_matches[deck - 1] = Match();
        if (target > 0 && keysMatch(deck, target)) {
            m_matches[deck - 1] = {target, m_host.read(deck, QStringLiteral("pitch")),
                    m_host.read(target, QStringLiteral("pitch")), m_host.track(deck)};
        }
        break;
    }
    case KeyAction::Reset:
        // The track's own key, then key lock as it was before the sheet
        // (in this order, a deck that had key lock off plays exactly as it
        // did: Mixxx's unlock gives the pitch back to the tempo).
        pulse(deck, QStringLiteral("reset_key"));
        restoreKeylock(deck);
        m_matches[deck - 1] = Match();
        break;
    case KeyAction::KeyLock:
        // A deliberate choice: no longer the sheet's to undo.
        m_keylockBySheet[deck - 1] = false;
        m_keylockTrack[deck - 1].reset();
        m_host.write(deck, QStringLiteral("keylock"), m_host.read(deck, QStringLiteral("keylock")) > 0 ? 0 : 1);
        break;
    case KeyAction::Done:
        break;
    }
    if (m_host.refresh) {
        m_host.refresh(deck);
    }
}

bool DeckLabDeckTools::matchHolds(int deck) const {
    // Still the matched key: the same track, the shift MATCH set, and the
    // matched deck still loaded, at the pitch it had, in a key that mixes.
    const Match& match = m_matches[deck - 1];
    if (match.target <= 0) {
        return false;
    }
    const TrackPointer matched = match.track.lock();
    const double pitch = m_host.read(deck, QStringLiteral("pitch"));
    const double targetPitch = m_host.read(match.target, QStringLiteral("pitch"));
    return matched && matched == m_host.track(deck) && util_isfinite(pitch) && util_isfinite(targetPitch) &&
            std::abs(pitch - match.pitch) < 0.02 && std::abs(targetPitch - match.targetPitch) <= kMatchDrift &&
            m_host.read(match.target, QStringLiteral("track_loaded")) > 0 && keysMatch(deck, match.target);
}

void DeckLabDeckTools::fillKey(int deck, DeckLabStripState& state) {
    if (deck < 1 || deck > 4) {
        return;
    }
    state.keyShift.clear();
    state.keyMatched = false;
    // A new track gives the deck its key lock back if the sheet turned it on.
    if (m_keylockBySheet[deck - 1]) {
        const TrackPointer owner = m_keylockTrack[deck - 1].lock();
        if (!owner || owner != m_host.track(deck)) {
            restoreKeylock(deck);
        }
    }
    if (!state.loaded) {
        m_matches[deck - 1] = Match();
        return;
    }
    // The deliberate shift only: from the track's key while key lock holds
    // it (Mixxx's pitch), and the tweak over the tempo while it does not
    // (pitch_adjust), so riding the tempo fader never reads as a key shift.
    const bool keylock = m_host.read(deck, QStringLiteral("keylock")) > 0;
    state.keyShift = shiftText(m_host.read(deck, keylock ? QStringLiteral("pitch") : QStringLiteral("pitch_adjust")));
    if (matchHolds(deck)) {
        state.keyMatched = true;
    } else {
        m_matches[deck - 1] = Match();
    }
}

DeckLabKeySheetState DeckLabDeckTools::keyState(int deck) const {
    const DeckLabStripState& strip = this->strip(deck);
    DeckLabKeySheetState state;
    state.deck = deck;
    state.loaded = strip.loaded && current(deck);
    if (!state.loaded) {
        return state;
    }
    const auto keyText = [this](int which) {
        const auto key = chromatic(m_host.read(which, QStringLiteral("key")));
        return validKey(key) ? KeyUtils::keyToString(key) : QString();
    };
    state.hasKey = validKey(chromatic(m_host.read(deck, QStringLiteral("file_key"))));
    state.keylock = m_host.read(deck, QStringLiteral("keylock")) > 0;
    state.target = keySyncTarget(deck);
    if (state.target > 0) {
        state.targetKey = keyText(state.target);
    }
    state.key = strip.key;
    state.shift = strip.keyShift;
    // MATCHED names the deck that was matched, which today's target may not be.
    state.matched = strip.keyMatched && m_matches[deck - 1].target > 0;
    if (state.matched) {
        state.matchedDeck = m_matches[deck - 1].target;
        state.matchedKey = keyText(state.matchedDeck);
    }
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
    // Zoom: an active session already zoomed in; its zoom-out comes back
    // when this one ends.
    double baseline = m_host.read(deck, QStringLiteral("waveform_zoom"));
    if (m_grid.deck) {
        baseline = m_grid.zoomBefore;
        endSession(false);
    }
    if (!m_gridPanel) {
        DeckLabGridPanel::Host host;
        host.current = [this](int which) { return which == m_grid.deck && current(which); };
        host.act = [this](int which, GridAction action) { gridAction(which, action); };
        m_gridPanel = new DeckLabGridPanel(QStringLiteral("decklab-grid-mode"), host,
                m_deckGrid ? m_deckGrid->parentWidget() : nullptr);
        m_gridPanel->hide();
    }
    // One knob owner at a time.
    closeKeySheets();
    m_grid.deck = deck;
    m_grid.track = m_host.track(deck);
    m_grid.four = m_host.fourDecks && m_host.fourDecks();
    // A closer look at the grid while editing: Mixxx's zoom control only.
    m_grid.zoomBefore = util_isfinite(baseline) && baseline > kGridZoom ? baseline : 0;
    if (m_grid.zoomBefore > 0) {
        m_host.write(deck, QStringLiteral("waveform_zoom"), kGridZoom);
    }
    m_gridPanel->setKnob(DeckLabGridPanel::Knob::Nudge);
    m_gridPanel->setState(gridState(deck));
    if (m_host.relayout) {
        m_host.relayout();
    }
}

void DeckLabDeckTools::endSession(bool restoreZoom) {
    const GridSession session = m_grid;
    m_grid = GridSession();
    if (m_gridPanel) {
        if (m_deckGrid) {
            m_deckGrid->removeWidget(m_gridPanel);
        }
        m_gridPanel->hide();
    }
    // The zoom comes back unless it was changed meanwhile.
    if (restoreZoom && session.zoomBefore > 0 &&
            std::abs(m_host.read(session.deck, QStringLiteral("waveform_zoom")) - kGridZoom) < 1e-6) {
        m_host.write(session.deck, QStringLiteral("waveform_zoom"), session.zoomBefore);
    }
}

void DeckLabDeckTools::leaveGrid(Relayout relayout) {
    if (!m_grid.deck) {
        return;
    }
    endSession(true);
    if (!m_host.relayout) {
        return;
    }
    switch (relayout) {
    case Relayout::Now:
        m_host.relayout();
        break;
    case Relayout::Queued:
        QMetaObject::invokeMethod(
                this, [this] { if (m_host.relayout) m_host.relayout(); }, Qt::QueuedConnection);
        break;
    case Relayout::None:
        break;
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

bool DeckLabDeckTools::canHalve(const mixxx::BeatsPointer& beats) const {
    // Mixxx refuses to halve some marker grids (odd beat counts between
    // markers). Ask it once per grid object, not on every refresh.
    if (!beats) {
        return false;
    }
    if (m_halveBeats.lock() != beats) {
        m_halveBeats = beats;
        m_halveOk = static_cast<bool>(beats->tryScale(mixxx::Beats::BpmScale::Halve));
    }
    return m_halveOk;
}

void DeckLabDeckTools::gridAction(int deck, GridAction action) {
    if (action == GridAction::Done) {
        leaveGrid(Relayout::Now);
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
        // constant-tempo moves) are Mixxx's. The live checks above run at
        // touch-down, ahead of the panel's last refresh.
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
            // beat 1 of the count the phase meter shows (constant-tempo
            // grids only: the panel offers DOWNBEAT nowhere else).
            const auto before = track->getBeats();
            if (!before || !before->hasConstantTempo()) {
                break;
            }
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
    state.canHalve = canHalve(grid);
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

const DeckLabStripState& DeckLabDeckTools::strip(int deck) const {
    static const DeckLabStripState empty;
    return m_host.stripState && deck >= 1 && deck <= 4 ? m_host.stripState(deck) : empty;
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
            // A new track on the edited deck ends GRID mode (this runs in a
            // deck refresh: the relayout follows it).
            leaveGrid(Relayout::Queued);
        } else {
            m_gridPanel->setState(gridState(deck));
            m_gridPanel->refreshKnob();
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
        // Called before the surface's relayout: that one relayout covers it.
        leaveGrid(Relayout::None);
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
        // Half the deck row. With the lanes side by side (1 2 over 3 4) the
        // half under the deck's lane column, never under another deck's.
        const bool columns = m_host.sideBySide && m_host.sideBySide();
        const int first = columns ? (deck - 1) % 2 * 2 : (deck - 1) / 2 * 2;
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
