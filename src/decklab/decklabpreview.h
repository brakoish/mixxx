// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>

/// Sample Maker's headphone preview and PUT ON PAD (decklabd protocol v1.2).
///
/// Mixxx owns the sound card, so Sample Maker cannot play audio itself.
/// decklabd relays the utility's `preview.play`, `preview.stop` and
/// `sampler.load` to Mixxx as events with a serial (CONTRACT-decklabd.md).
/// This class carries them out on [PreviewDeck1], which plays only in the
/// headphones, and on the first empty sampler pad; it answers each one with
/// `relay.reply` and pushes `preview.report` so the page can draw a playhead.
///
/// It never changes what the decks hold, refuses while a deck or sampler
/// plays, stops the preview when one starts and unloads it when the utility
/// closes. Every Mixxx effect goes through Hooks, so the policy is testable
/// without players, a library or an audio engine.
class DeckLabPreview : public QObject {
    Q_OBJECT
  public:
    enum class State {
        Idle,    // nothing of ours in the preview deck
        Loading, // waiting for our track to load
        Playing,
        Stopped, // our track is loaded and paused
    };
    struct Hooks {
        std::function<bool()> decksPlaying;   // any deck or sampler playing
        std::function<bool()> utilityOpen;    // a utility window is open (apps.changed)
        std::function<int()> loadedTrack;     // library id in [PreviewDeck1], 0 when none
        /// Starts loading library track `id` into [PreviewDeck1]; false and a reason when it can't.
        std::function<bool(int id, QString* error)> load;
        std::function<double()> duration;     // seconds of the loaded track, 0 while none
        std::function<double()> position;     // seconds, at audio callback precision
        std::function<bool()> playing;        // [PreviewDeck1] plays
        /// Loops start..end (the loop on only for `loop`), seeks to `from`
        /// unless it is negative, then plays.
        std::function<void(double start, double end, bool loop, double from)> play;
        std::function<void()> pause;          // pauses and turns the loop off
        std::function<void()> eject;
        std::function<QString()> samplesFolder; // ~/Music/Deck Lab Samples
        /// First empty pad (1-based), 0 when every pad holds a track.
        std::function<int()> emptyPad;
        std::function<bool(int pad, const QString& path, QString* error)> loadPad;
    };
    /// Sends a request to decklabd without waiting for its answer.
    using Send = std::function<void(const QString& method, const QJsonObject& params)>;

    static constexpr int kReportIntervalMs = 250;
    static constexpr int kWatchIntervalMs = 10;
    static constexpr int kLoadTimeoutMs = 15000;
    static constexpr double kMaxSeconds = 7200.0;
    static constexpr double kMinPreviewSeconds = 0.01;
    /// A `from` this close to where the same preview already plays carries on
    /// without a seek, so moving IN or OUT during a preview never stutters.
    static constexpr double kSeamlessSeconds = 0.25;

    DeckLabPreview(Hooks hooks, Send send, QObject* parent = nullptr);

    void setHooks(Hooks hooks);
    /// preview.play, preview.stop and sampler.load; false for other events.
    bool onEvent(const QString& topic, const QJsonObject& data);
    /// [PreviewDeck1] finished loading library track `id`; 0 when it emptied or failed.
    void onPreviewLoaded(int id);
    /// apps.changed: a utility window opened or closed.
    void onUtilityChanged(bool open);

    State state() const {
        return m_state;
    }
    /// What preview.report carries.
    QJsonObject report() const;
    /// The canonical path of a sample named by its basename only, inside
    /// `folder`; empty with a reason for anything else (paths, links, other folders).
    static QString samplePath(const QString& folder, const QJsonValue& name, QString* error);

  signals:
    /// The preview deck started or stopped for us: deck-state.json is due.
    void playingChanged();

  private:
    struct Request {
        int track = 0;
        double start = 0;
        double end = 0;
        bool loop = false;
        double from = -1;
    };
    static bool parsePlay(const QJsonObject& data, Request* request, QString* error);
    void play(qint64 serial, const QJsonObject& data);
    void stop(qint64 serial);
    void loadSample(qint64 serial, const QJsonObject& data);
    bool begin(const Request& request, QString* error);
    void halt(State state, const QString& reason);
    void setState(State state);
    void watch();
    void tick();
    void sendReport();
    void reply(qint64 serial, const QJsonObject& result);
    void refuse(qint64 serial, const QString& code, const QString& message);

    Hooks m_hooks;
    Send m_send;
    State m_state = State::Idle;
    Request m_current;
    double m_stoppedAt = 0;
    bool m_unloadWhenLoaded = false;
    QString m_reason;
    QTimer m_watch;
    QTimer m_report;
    QElapsedTimer m_loading;
};
