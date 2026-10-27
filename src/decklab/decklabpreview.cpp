// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabpreview.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonValue>
#include <QRegularExpression>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <utility>

#include "moc_decklabpreview.cpp"

namespace {
QString stateName(DeckLabPreview::State state) {
    switch (state) {
    case DeckLabPreview::State::Loading:
        return QStringLiteral("loading");
    case DeckLabPreview::State::Playing:
        return QStringLiteral("playing");
    case DeckLabPreview::State::Stopped:
        return QStringLiteral("stopped");
    case DeckLabPreview::State::Idle:
        break;
    }
    return QStringLiteral("idle");
}

// A whole, positive library id, else 0.
int libraryId(const QJsonValue& value) {
    const double number = value.toDouble(0);
    if (!value.isDouble() || !std::isfinite(number) || number < 1 || number > 2147483647.0 ||
            std::floor(number) != number) {
        return 0;
    }
    return static_cast<int>(number);
}

bool seconds(const QJsonValue& value, double* out) {
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > DeckLabPreview::kMaxSeconds) {
        return false;
    }
    *out = number;
    return true;
}

// Positions closer than half a millisecond are the same place.
constexpr double kSamePlace = 0.0005;

// decklabd accepts 0..7200 s only; a missing or odd engine value reads as 0.
double bounded(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, DeckLabPreview::kMaxSeconds) : 0.0;
}

// Missing hooks fail safe: unknown deck state counts as playing, nothing loads.
DeckLabPreview::Hooks withDefaults(DeckLabPreview::Hooks hooks) {
    if (!hooks.decksPlaying) {
        hooks.decksPlaying = [] { return true; };
    }
    if (!hooks.utilityOpen) {
        hooks.utilityOpen = [] { return false; };
    }
    if (!hooks.loadedTrack) {
        hooks.loadedTrack = [] { return 0; };
    }
    if (!hooks.load) {
        hooks.load = [](int, QString*) { return false; };
    }
    if (!hooks.duration) {
        hooks.duration = [] { return 0.0; };
    }
    if (!hooks.position) {
        hooks.position = [] { return 0.0; };
    }
    if (!hooks.playing) {
        hooks.playing = [] { return false; };
    }
    if (!hooks.play) {
        hooks.play = [](double, double, bool, double) {};
    }
    if (!hooks.pause) {
        hooks.pause = [] {};
    }
    if (!hooks.eject) {
        hooks.eject = [] {};
    }
    if (!hooks.samplesFolder) {
        hooks.samplesFolder = [] { return QString(); };
    }
    if (!hooks.emptyPad) {
        hooks.emptyPad = [] { return 0; };
    }
    if (!hooks.loadPad) {
        hooks.loadPad = [](int, const QString&, QString*) { return false; };
    }
    return hooks;
}
} // namespace

DeckLabPreview::DeckLabPreview(Hooks hooks, Send send, QObject* parent)
        : QObject(parent),
          m_hooks(withDefaults(std::move(hooks))),
          m_send(std::move(send)) {
    m_watch.setInterval(kWatchIntervalMs);
    m_watch.setTimerType(Qt::PreciseTimer);
    connect(&m_watch, &QTimer::timeout, this, &DeckLabPreview::watch);
    m_report.setInterval(kReportIntervalMs);
    connect(&m_report, &QTimer::timeout, this, &DeckLabPreview::tick);
}

void DeckLabPreview::setHooks(Hooks hooks) {
    m_hooks = withDefaults(std::move(hooks));
}

bool DeckLabPreview::onEvent(const QString& topic, const QJsonObject& data) {
    if (topic != QStringLiteral("preview.play") && topic != QStringLiteral("preview.stop") &&
            topic != QStringLiteral("sampler.load")) {
        return false;
    }
    const QJsonValue raw = data.value(QStringLiteral("serial"));
    const double number = raw.toDouble(0);
    if (!raw.isDouble() || number < 1 || number > 9007199254740991.0 || std::floor(number) != number) {
        qWarning() << "decklabd relayed" << topic << "without a serial";
        return true;
    }
    const auto serial = static_cast<qint64>(number);
    if (topic == QStringLiteral("preview.play")) {
        play(serial, data);
    } else if (topic == QStringLiteral("preview.stop")) {
        stop(serial);
    } else {
        loadSample(serial, data);
    }
    return true;
}

bool DeckLabPreview::parsePlay(const QJsonObject& data, Request* request, QString* error) {
    request->track = libraryId(data.value(QStringLiteral("track_id")));
    if (!request->track) {
        *error = tr("That isn't a library track.");
        return false;
    }
    if (!seconds(data.value(QStringLiteral("start")), &request->start) ||
            !seconds(data.value(QStringLiteral("end")), &request->end) ||
            request->end - request->start < kMinPreviewSeconds) {
        *error = tr("Choose valid IN and OUT points.");
        return false;
    }
    const QJsonValue loop = data.value(QStringLiteral("loop"));
    if (!loop.isUndefined() && !loop.isNull() && !loop.isBool()) {
        *error = tr("Loop must be on or off.");
        return false;
    }
    request->loop = loop.toBool(false);
    request->from = -1;
    const QJsonValue from = data.value(QStringLiteral("from"));
    if (!from.isUndefined() && !from.isNull() &&
            (!seconds(from, &request->from) || request->from < request->start ||
                    request->from >= request->end)) {
        *error = tr("The preview must start inside the cut.");
        return false;
    }
    return true;
}

void DeckLabPreview::play(qint64 serial, const QJsonObject& data) {
    Request request;
    QString error;
    if (!parsePlay(data, &request, &error)) {
        refuse(serial, QStringLiteral("bad_request"), error);
        return;
    }
    // Only for an open utility (Sample Maker), and never over a playing deck or pad.
    if (!m_hooks.utilityOpen()) {
        refuse(serial, QStringLiteral("denied"), tr("Open Sample Maker from Deck Lab to preview."));
        return;
    }
    if (m_hooks.decksPlaying()) {
        refuse(serial, QStringLiteral("busy"), tr("Stop the decks and samplers to preview."));
        return;
    }
    m_reason.clear();
    m_unloadWhenLoaded = false;
    if (m_state == State::Loading && m_current.track == request.track &&
            m_hooks.loadedTrack() != request.track) {
        // IN or OUT moved while the track loads: it will play the new cut.
        m_current = request;
    } else if (m_hooks.loadedTrack() == request.track && m_hooks.duration() > 0) {
        if (!begin(request, &error)) {
            refuse(serial, QStringLiteral("bad_request"), error);
            return;
        }
    } else {
        // Another track, or a Browse preview: pause it, then load ours.
        if (m_hooks.playing()) {
            m_hooks.pause();
            if (m_state == State::Playing) {
                m_stoppedAt = bounded(playhead());
                setState(State::Stopped);
            }
        }
        m_ownLoad = true;
        const bool loading = m_hooks.load(request.track, &error);
        m_ownLoad = false;
        if (!loading) {
            refuse(serial,
                    QStringLiteral("failed"),
                    error.isEmpty() ? tr("That track could not be loaded.") : error);
            return;
        }
        m_current = request;
        m_loading.start();
        setState(State::Loading);
    }
    sendReport();
    reply(serial, report());
}

bool DeckLabPreview::begin(const Request& wanted, QString* error) {
    Request request = wanted;
    const double length = m_hooks.duration();
    if (!(length > 0)) {
        *error = tr("The track is not loaded.");
        return false;
    }
    request.end = std::min(request.end, length);
    if (request.end - request.start < kMinPreviewSeconds) {
        *error = tr("IN is past the end of the track.");
        return false;
    }
    if (request.from >= request.end) {
        request.from = -1;
    }
    const double now = playhead();
    const bool carryOn = m_state == State::Playing && m_current.track == request.track &&
            m_hooks.playing() && request.from >= 0 && std::abs(now - request.from) < kSeamlessSeconds &&
            now >= request.start && now < request.end;
    const double from = carryOn ? -1.0 : (request.from >= 0 ? request.from : request.start);
    m_hooks.play(request.start, request.end, request.loop, from);
    if (from >= 0) {
        // The engine seeks on its next audio callback. Until then it reports
        // where it was, which can be at or past OUT (PLAY again after OUT, OUT
        // moved before the playhead), and watch() must not stop on that.
        m_seen = m_hooks.position();
        m_seekTo = std::abs(m_seen - from) < kSamePlace ? -1.0 : from;
        m_changesToSee = 2;
    }
    m_current = request;
    setState(State::Playing);
    return true;
}

void DeckLabPreview::stop(qint64 serial) {
    if (m_state == State::Playing || m_state == State::Loading) {
        halt(State::Stopped, QString());
    } else {
        sendReport();
    }
    reply(serial, report());
}

void DeckLabPreview::loadSample(qint64 serial, const QJsonObject& data) {
    QString error;
    const QString path = samplePath(m_hooks.samplesFolder(), data.value(QStringLiteral("name")), &error);
    if (path.isEmpty()) {
        refuse(serial, QStringLiteral("bad_request"), error);
        return;
    }
    if (!m_hooks.utilityOpen()) {
        refuse(serial, QStringLiteral("denied"), tr("Open Sample Maker from Deck Lab to load a pad."));
        return;
    }
    // Loading reads the file: never while anything plays.
    if (m_hooks.decksPlaying() || m_hooks.playing()) {
        refuse(serial, QStringLiteral("busy"), tr("Stop the decks, samplers and preview to load a pad."));
        return;
    }
    // r7 fix: nor beside a library scan (the library add would wait for it).
    if (m_hooks.scanning && m_hooks.scanning()) {
        refuse(serial, QStringLiteral("busy"), tr("Deck Lab is looking for new tracks. Try again in a moment."));
        return;
    }
    const int pad = m_hooks.emptyPad();
    if (pad < 1) {
        refuse(serial,
                QStringLiteral("busy"),
                tr("Every pad is in use. Empty one on the Sampler page first."));
        return;
    }
    if (!m_hooks.loadPad(pad, path, &error)) {
        refuse(serial,
                QStringLiteral("failed"),
                error.isEmpty() ? tr("The sample could not be loaded.") : error);
        return;
    }
    reply(serial, QJsonObject{{QStringLiteral("pad"), pad}, {QStringLiteral("name"), QFileInfo(path).fileName()}});
}

QString DeckLabPreview::samplePath(const QString& folder, const QJsonValue& value, QString* error) {
    // A basename with an audio extension: no separators, no leading dot, no control characters.
    static const QRegularExpression kName(QStringLiteral(
                                                  "^[^./\\\\\\x{0}-\\x{1f}\\x{7f}][^/\\\\\\x{0}-\\x{1f}\\x{7f}]{0,249}"
                                                  "\\.(?:wav|aiff?|flac|mp3|ogg|opus|m4a)$"),
            QRegularExpression::CaseInsensitiveOption);
    const QString name = value.toString();
    if (!value.isString() || name != name.trimmed() || !kName.match(name).hasMatch()) {
        *error = tr("That isn't the file name of a sample.");
        return QString();
    }
    // The file itself, not a link, and still inside the folder once resolved.
    const QString root = folder.isEmpty() ? QString() : QFileInfo(folder).canonicalFilePath();
    const QFileInfo info(QDir(folder).filePath(name));
    if (root.isEmpty() || !info.exists() || info.isSymLink() || !info.isFile() ||
            QFileInfo(info.canonicalFilePath()).absolutePath() != root) {
        *error = tr("That sample is no longer in Deck Lab Samples.");
        return QString();
    }
    return info.canonicalFilePath();
}

void DeckLabPreview::onPreviewLoaded(int id) {
    if (std::exchange(m_unloadWhenLoaded, false) && id == m_current.track && id > 0) {
        // Sample Maker closed while our track was still loading. Any other
        // outcome (a failed load, another track) leaves the deck alone.
        m_hooks.eject();
        return;
    }
    if (m_state == State::Loading) {
        if (id != m_current.track) {
            m_reason = id ? QString() : tr("The track could not be loaded.");
            setState(State::Idle);
        } else if (m_hooks.decksPlaying()) {
            m_reason = tr("A deck or sampler is playing, so the preview did not start.");
            m_stoppedAt = m_current.start;
            setState(State::Stopped);
        } else {
            QString error;
            if (!begin(m_current, &error)) {
                m_reason = error;
                m_stoppedAt = m_current.start;
                setState(State::Stopped);
            }
        }
        sendReport();
        return;
    }
    if (m_state != State::Idle && id != m_current.track) {
        // Browse previewed another track, or the preview deck was emptied.
        m_reason.clear();
        setState(State::Idle);
        sendReport();
    }
}

void DeckLabPreview::onPreviewLoading() {
    if (m_ownLoad) {
        return;
    }
    // Browse (or anything but us) loads the preview deck: what Sample Maker
    // asked for is no longer coming, and nothing of ours is there to unload.
    // Browse previewing the very track we load makes it Browse's too.
    m_unloadWhenLoaded = false;
    if (m_state == State::Loading) {
        m_reason.clear();
        setState(State::Idle);
        sendReport();
    }
}

void DeckLabPreview::onConnectionLost() {
    if (m_state == State::Playing || m_state == State::Loading) {
        halt(State::Stopped, tr("Deck Lab lost its link to Sample Maker, so the preview stopped."));
    }
}

void DeckLabPreview::onUtilityChanged(const QString& app) {
    if (app == kSampleMaker || m_state == State::Idle) {
        return;
    }
    // Sample Maker is gone: leave the preview deck as Browse expects it.
    if (m_state == State::Loading) {
        m_unloadWhenLoaded = true;
    } else if (m_hooks.loadedTrack() == m_current.track) {
        if (m_hooks.playing()) {
            m_hooks.pause();
        }
        m_hooks.eject();
    }
    m_reason.clear();
    setState(State::Idle);
    sendReport();
}

void DeckLabPreview::halt(State state, const QString& reason) {
    m_stoppedAt = m_state == State::Playing ? bounded(playhead()) : m_current.start;
    if (m_hooks.playing() && m_hooks.loadedTrack() == m_current.track) {
        m_hooks.pause();
    }
    m_reason = reason;
    setState(state);
    sendReport();
}

void DeckLabPreview::setState(State state) {
    m_state = state;
    if (state != State::Playing) {
        m_seekTo = -1;
    }
    // A cut without loop is watched for OUT, a looped one until the engine
    // has taken its seek.
    if (state == State::Playing && (!m_current.loop || m_seekTo >= 0)) {
        m_watch.start();
    } else {
        m_watch.stop();
    }
    if (state == State::Playing || state == State::Loading) {
        if (!m_report.isActive()) {
            m_report.start();
        }
    } else {
        m_report.stop();
    }
    emit playingChanged();
}

void DeckLabPreview::watch() {
    // A cut without loop stops at OUT; Mixxx's own loop wraps a looped one.
    if (m_state != State::Playing) {
        m_watch.stop();
        return;
    }
    if (!m_hooks.playing()) {
        m_stoppedAt = bounded(playhead());
        setState(State::Stopped);
        sendReport();
        return;
    }
    const double position = m_hooks.position();
    if (!seekTaken(position)) {
        return; // still where it was before our seek
    }
    if (m_current.loop) {
        m_watch.stop();
    } else if (position >= m_current.end - kSamePlace) {
        m_hooks.pause();
        m_stoppedAt = m_current.end;
        m_reason.clear();
        setState(State::Stopped);
        sendReport();
    }
}

bool DeckLabPreview::seekTaken(double position) {
    // The first new position can still come from the audio callback that was
    // running when the seek was asked for; every later one is from after it.
    // A first one inside the cut is not past OUT either, so it counts too.
    if (m_seekTo >= 0 && position != m_seen) {
        m_seen = position;
        if (--m_changesToSee <= 0 || (position >= m_current.start && position < m_current.end)) {
            m_seekTo = -1;
        }
    }
    return m_seekTo < 0;
}

double DeckLabPreview::playhead() const {
    // Our seek's target until the engine shows it.
    return m_seekTo >= 0 ? m_seekTo : m_hooks.position();
}

void DeckLabPreview::tick() {
    if (m_state == State::Loading && m_loading.elapsed() > kLoadTimeoutMs) {
        m_reason = tr("The track did not load.");
        setState(State::Idle);
        sendReport();
        return;
    }
    if (m_hooks.decksPlaying()) {
        halt(State::Stopped, tr("A deck or sampler started, so the preview stopped."));
        return;
    }
    if (m_state == State::Playing && !m_hooks.playing()) {
        m_stoppedAt = bounded(playhead());
        setState(State::Stopped);
    }
    sendReport();
}

QJsonObject DeckLabPreview::report() const {
    double position = 0;
    switch (m_state) {
    case State::Playing:
        position = playhead();
        break;
    case State::Stopped:
        position = m_stoppedAt;
        break;
    case State::Loading:
        position = m_current.from >= 0 ? m_current.from : m_current.start;
        break;
    case State::Idle:
        break;
    }
    QJsonObject result{{QStringLiteral("state"), stateName(m_state)},
            {QStringLiteral("track_id"), m_state == State::Idle ? 0 : m_current.track},
            {QStringLiteral("position"), bounded(position)},
            {QStringLiteral("start"), bounded(m_current.start)},
            {QStringLiteral("end"), bounded(m_current.end)},
            {QStringLiteral("duration"), bounded(m_hooks.duration())},
            {QStringLiteral("loop"), m_current.loop}};
    if (!m_reason.isEmpty()) {
        result.insert(QStringLiteral("reason"), m_reason.left(300));
    }
    return result;
}

void DeckLabPreview::sendReport() {
    if (m_send) {
        m_send(QStringLiteral("preview.report"), report());
    }
}

void DeckLabPreview::reply(qint64 serial, const QJsonObject& result) {
    if (m_send) {
        m_send(QStringLiteral("relay.reply"),
                QJsonObject{{QStringLiteral("serial"), serial}, {QStringLiteral("result"), result}});
    }
}

void DeckLabPreview::refuse(qint64 serial, const QString& code, const QString& message) {
    if (m_send) {
        m_send(QStringLiteral("relay.reply"),
                QJsonObject{{QStringLiteral("serial"), serial},
                        {QStringLiteral("error"),
                                QJsonObject{{QStringLiteral("code"), code},
                                        {QStringLiteral("message"), message}}}});
    }
}
