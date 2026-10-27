// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// r7-sampler: save a deck's loop to a sampler pad (rekordbox calls it
// Capture). TO PAD, in the deck's MORE sheet beside the track keys, saves
// exactly the deck's active loop as a sample and puts it on the first empty
// pad, as a LOOP pad whose beat grid is the track's tempo (the loop is a
// whole number of beats), so a SYNC'd loop pad stays in time. With no empty
// pad a small sheet asks which pad to replace, or cancel.
//
// - Mixxx's own decoder (SoundSourceProxy) reads the loop on a worker thread
//   at idle priority: never on the audio or GUI thread. Allowed while the
//   deck plays (DJs capture live); the engine is never touched, the range
//   comes from the deck's loop controls when TO PAD is tapped.
// - The file: WAV at the track's sample rate, stereo, 16-bit PCM. It is what
//   Sample Maker writes to the same folder, half the SD-card writes of float,
//   plays on CDJs and in rekordbox from an export, and most DJ files carry
//   16 bits anyway. The source rate is kept, so the sample has exactly the
//   loop's frames and its grid needs no resampling.
// - ~/Music/Deck Lab Samples (decklab::samplesFolder, the folder Sample
//   Maker uses; created if missing), named
//   "<Title> - Loop <n> bars" (or beats) as Sample Maker's clean_name cleans
//   it, with " 2", " 3" ... when taken. Written to a hidden temporary file in
//   the folder and renamed, so a half-written sample never shows in Browse.
//   The title is also in the file (RIFF INFO), so the pad shows it whole.
// - At most kMaxBeats (32 bars) and kMaxSeconds; longer loops are refused
//   with a message that says the limit.
// - The pad plays the sample at once from a temporary track with the loop's
//   grid; nothing touches the library database then. The sample joins the
//   library (with that grid) once it is quiet (DeckLabCaptureLibrary).
//
// The writing and naming are free functions, tested on generated files;
// DeckLabLoopCapture reaches the surface only through its Host.

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include "track/track_decl.h"

class QGridLayout;
class QLabel;
class QPushButton;
class QThread;
namespace mixxx {
class Beats;
}

namespace decklab::capture {

constexpr int kMaxBeats = 128; // 32 bars
constexpr double kMaxSeconds = 60.0;
constexpr int kMaxNameLength = 70; // Sample Maker's clean_name

// Sample Maker's clean_name: the characters a file name cannot carry become
// spaces, runs of white space one space, at most 70 characters, no leading
// or trailing spaces or dots. Empty when nothing is left.
QString cleanName(const QString& name);
// "1 bar", "2 bars" (whole bars), "1 beat", "3 beats", "0.5 beats"; empty
// without a length in beats.
QString lengthText(double beats);
// "<Title> - Loop 2 bars" (or "- Loop 4.2 s" without a grid), cleaned; a
// long title is shortened, never the length.
QString sampleName(const QString& title, double beats, double seconds);
// "<folder>/<name>.wav", or "<name> 2.wav", "<name> 3.wav" ... while taken;
// empty after 999.
QString uniquePath(const QString& folder, const QString& name);
QString candidatePath(const QString& folder, const QString& name, int number);
// The folder is decklab::samplesFolder() (decklab/decklabdownloads.h).

// What TO PAD found on the deck.
enum class Refusal {
    None,
    NoTrack,  // nothing loaded, or no file behind it
    NoLoop,   // no active loop
    TooShort, // under a few milliseconds
    TooLong,  // over kMaxBeats or kMaxSeconds
    Busy,     // the last capture is still being written
};
struct Plan {
    Refusal refusal = Refusal::NoLoop;
    qint64 startFrame = 0;
    qint64 frames = 0;
    double beats = 0; // the loop's length in beats (snapped when whole); 0 without a grid
    double seconds = 0;
};
// enabled: loop_enabled; loopStart, loopEnd: loop_start_position and
// loop_end_position (engine samples, two per frame); the track's rate and
// beat grid.
Plan plan(bool enabled,
        double loopStart,
        double loopEnd,
        double sampleRate,
        const std::shared_ptr<const mixxx::Beats>& beats);
QString refusalText(Refusal refusal);

struct Request {
    QString source; // the deck's file
    QString title;
    qint64 startFrame = 0;
    qint64 frames = 0;
    double beats = 0;
    QString folder;
    // Left free on the disk after the sample (1 MB by default).
    qint64 reserveBytes = 1 << 20;
};
struct Result {
    enum class Status {
        Saved,
        Unreadable,
        NoSpace,
        NotWritable,
        Cancelled,
    };
    Status status = Status::Unreadable;
    QString path;      // the saved sample
    qint64 frames = 0; // written
    int sampleRate = 0;
    double bpm = 0; // the sample's tempo: its beats over its length; 0 without a grid
};
QString errorText(Result::Status status);
// Decodes the range and writes the sample. Blocking: a worker thread runs
// it. `cancel` is checked between chunks; a cancelled or failed write leaves
// nothing behind.
Result write(const Request& request, const std::atomic<bool>* cancel = nullptr);
// The saved sample as a temporary track for a pad, without the library: the
// file's name as its title, its length, and the loop's grid (whole beats
// over its length), locked so a pad's analysis keeps it.
TrackPointer padTrack(const Result& result);
// The loop's grid on a track (unless its tempo is locked already); false
// without a tempo.
bool applyGrid(const TrackPointer& track, const Result& result);

} // namespace decklab::capture

// "No empty pad: replace which?" Every pad with its number and title; a
// playing pad cannot be replaced. A pad acts on release. CANCEL keeps the
// sample in the folder. A hide (page change) cancels.
class DeckLabPadChooser final : public QWidget {
    Q_OBJECT
  public:
    struct Pad {
        int sampler = 0;
        QString title;
        bool playing = false;
    };
    static constexpr int kPads = 16;
    explicit DeckLabPadChooser(QWidget* parent = nullptr);
    ~DeckLabPadChooser() override;
    // Shows the sheet for this sample over its parent. r7 fix: the browse
    // knob drives it while it is shown (a turn moves a cursor over the pads
    // that can be replaced and CANCEL, the first turn only shows it; a
    // press picks, back cancels), so a turn never leaves for Browse.
    void ask(const QString& sample, const QList<Pad>& pads);
    void cancel();
    QPushButton* padButton(int sampler) const;
    QPushButton* cancelButton() const {
        return m_cancel;
    }
    // The button under the knob's cursor; null while the cursor is hidden.
    QPushButton* cursorButton() const;

  signals:
    void chosen(int sampler);
    void cancelled();

  protected:
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void place();
    // The pads that can be replaced, then CANCEL.
    QList<QPushButton*> cursorTargets() const;
    void showCursor(QPushButton* button);
    QLabel* m_sample = nullptr;
    std::array<QPushButton*, kPads> m_pads{};
    std::array<int, kPads> m_samplers{};
    QPushButton* m_cancel = nullptr;
    QPushButton* m_cursor = nullptr; // shown after the first knob turn
    bool m_asking = false;
};

// TO PAD for every deck: checks the loop, writes it on a worker thread, then
// puts it on the first empty pad or asks.
class DeckLabLoopCapture final : public QObject {
    Q_OBJECT
  public:
    struct Pad {
        bool loaded = false;
        bool playing = false;
        QString title;
    };
    struct Host {
        // The deck's controls ([ChannelN]).
        std::function<double(int deck, const QString& item)> read;
        std::function<TrackPointer(int deck)> track;
        // Samplers that can take a sample, numbered from 1.
        std::function<int()> pads;
        std::function<Pad(int sampler)> pad;
        // r7: the pad a new sample takes (DeckLabSystem::emptyPad: nothing
        // loaded or loading, not the pad a Load pad trip fills), 0 when none.
        // Unset: the first pad that is not loaded.
        std::function<int()> emptyPad;
        // Loads the saved sample onto the pad (as a LOOP pad with the beat
        // grid); false when the pad cannot take it now.
        std::function<bool(int sampler, const decklab::capture::Result& result)> load;
        // Feedback on the deck ("Saved to pad 3"), or an error.
        std::function<void(int deck, const QString& text, bool error)> tell;
        // Where the "which pad?" sheet opens.
        std::function<QWidget*()> sheetParent;
    };
    explicit DeckLabLoopCapture(Host host, QObject* parent = nullptr);
    // A running write is cancelled and waited for (one chunk).
    ~DeckLabLoopCapture() override;
    // TO PAD on this deck: refused (and told why) or started.
    decklab::capture::Refusal capture(int deck);
    bool busy() const {
        return m_worker != nullptr;
    }
    // The "which pad?" sheet, once it was needed.
    DeckLabPadChooser* chooser() const {
        return m_chooser;
    }
    // A page change: an open "which pad?" sheet is cancelled.
    void cancelChoice();

  signals:
    // The capture ended: saved and on this pad, saved but on no pad
    // (sampler 0), or failed.
    void finished(int deck, int sampler, const decklab::capture::Result& result);

  private:
    void written(int deck, const decklab::capture::Result& result);
    void place(int deck, int sampler, const decklab::capture::Result& result);
    Host m_host;
    QThread* m_worker = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    DeckLabPadChooser* m_chooser = nullptr;
    // The saved sample waiting for a pad while the sheet asks.
    int m_pendingDeck = 0;
    decklab::capture::Result m_pending;
};

// r7: TO PAD's samples join the library once it is quiet
// (decklab/decklabquiet.h). Adding one reads its tags and writes the
// database on the GUI thread, which must never happen while anything plays
// or beside a library scan; meanwhile the pad plays it from a temporary
// track. The library row gets the loop's grid. A sample still waiting when
// Mixxx quits is found by the next library scan (its folder is a library
// folder). r7 fix: when the DJ opens the pad's SAMPLES list, what waits
// joins at once (flushNow: a few small files on the card), so a sample saved
// mid-set is in that list; never beside a library scan.
class DeckLabCaptureLibrary final : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<bool()> quiet;
        // r7 fix: Mixxx's library scan runs (flushNow waits for it).
        std::function<bool()> scanning;
        // The file's library track, added if it is new; null when it can't be.
        std::function<TrackPointer(const QString& path)> add;
    };
    static constexpr int kRetryMs = 2000;
    explicit DeckLabCaptureLibrary(Hooks hooks, QObject* parent = nullptr);
    // Adds the sample now if it is quiet, else once it is.
    void add(const decklab::capture::Result& result);
    // Adds what waits, if it is quiet now.
    void flush();
    // r7 fix: adds what waits now, quiet or not, unless a library scan runs.
    void flushNow();
    int pending() const {
        return static_cast<int>(m_pending.size());
    }

  private:
    void addPending();
    Hooks m_hooks;
    QList<decklab::capture::Result> m_pending;
    QTimer m_retry;
};
