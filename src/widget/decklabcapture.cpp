// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabcapture.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QStyle>
#include <QPushButton>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QThread>
#include <QVBoxLayout>
#include <QtDebug>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "decklab/decklabdownloads.h"
#include "decklab/decklaboverlay.h"
#include "library/decklabknob.h"
#include "sources/audiosourcestereoproxy.h"
#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/track.h"
#include "util/fpclassify.h"
#include "util/samplebuffer.h"

#include "moc_decklabcapture.cpp"

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabCapture", text);
}

// Frames decoded and written per step; the cancel flag is read between them.
constexpr SINT kChunkFrames = 8192;
// Shorter loops are refused (a few milliseconds).
constexpr qint64 kMinFrames = 64;
constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * 2; // 16-bit PCM

// Sample Maker's strip(' .').
QString stripSpacesAndDots(const QString& text) {
    qsizetype first = 0;
    qsizetype last = text.size();
    const auto edge = [](QChar c) {
        return c == QLatin1Char(' ') || c == QLatin1Char('.');
    };
    while (first < last && edge(text[first])) {
        ++first;
    }
    while (last > first && edge(text[last - 1])) {
        --last;
    }
    return text.mid(first, last - first);
}

// A whole number, or the given fractions of a beat, when the loop is within
// a hair of it (the engine rounds loop ends to frames).
double snapBeats(double raw) {
    if (!util_isfinite(raw) || raw <= 0) {
        return 0;
    }
    if (raw >= 0.75) {
        const double whole = std::round(raw);
        return std::abs(raw - whole) <= 0.02 * whole ? whole : raw;
    }
    for (double fraction = 0.5; fraction >= 1.0 / 32; fraction /= 2) {
        if (std::abs(raw - fraction) <= 0.02 * fraction) {
            return fraction;
        }
    }
    return raw;
}

// A 16-bit PCM WAV header: fmt, a RIFF INFO list with the title (INAM, so
// the library shows the whole name instead of splitting it at " - "), and
// the data chunk last. Sizes are filled in once the data is written.
QByteArray wavHeader(int sampleRate, const QString& title, qint64 dataBytes, int* dataSizeOffset) {
    QByteArray header;
    const auto u32 = [&header](quint32 value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        header.append(bytes, 4);
    };
    const auto u16 = [&header](quint16 value) {
        char bytes[2];
        qToLittleEndian(value, bytes);
        header.append(bytes, 2);
    };
    QByteArray name = title.toUtf8();
    name.append('\0');
    if (name.size() % 2) {
        name.append('\0');
    }
    header.append("RIFF");
    u32(0); // RIFF size, below
    header.append("WAVE");
    header.append("fmt ");
    u32(16);
    u16(1); // PCM
    u16(kChannels);
    u32(static_cast<quint32>(sampleRate));
    u32(static_cast<quint32>(sampleRate * kBytesPerFrame));
    u16(kBytesPerFrame);
    u16(16);
    header.append("LIST");
    u32(static_cast<quint32>(4 + 8 + name.size()));
    header.append("INFO");
    header.append("INAM");
    u32(static_cast<quint32>(title.toUtf8().size() + 1));
    header.append(name);
    header.append("data");
    *dataSizeOffset = static_cast<int>(header.size());
    u32(static_cast<quint32>(dataBytes));
    qToLittleEndian(static_cast<quint32>(header.size() - 8 + dataBytes), header.data() + 4);
    return header;
}

void setRiffSizes(QByteArray* header, int dataSizeOffset, qint64 dataBytes) {
    qToLittleEndian(static_cast<quint32>(header->size() - 8 + dataBytes), header->data() + 4);
    qToLittleEndian(static_cast<quint32>(dataBytes), header->data() + dataSizeOffset);
}

decklab::capture::Result failed(decklab::capture::Result::Status status) {
    decklab::capture::Result result;
    result.status = status;
    return result;
}

decklab::capture::Result::Status writeError(const QFileDevice& file) {
    // Qt reports ENOSPC as a resource error.
    return file.error() == QFileDevice::ResourceError ? decklab::capture::Result::Status::NoSpace
                                                      : decklab::capture::Result::Status::NotWritable;
}

} // namespace

namespace decklab::capture {

QString cleanName(const QString& name) {
    static const QString kForbidden = QStringLiteral("<>:\"/\\|?*");
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        out.append(c.unicode() < 0x20 || kForbidden.contains(c) ? QChar(QLatin1Char(' ')) : c);
    }
    out = stripSpacesAndDots(out);
    static const QRegularExpression kSpaces(QStringLiteral("\\s+"),
            QRegularExpression::UseUnicodePropertiesOption);
    out.replace(kSpaces, QStringLiteral(" "));
    if (out.size() > kMaxNameLength) {
        // Never half a surrogate pair.
        out.truncate(out.at(kMaxNameLength - 1).isHighSurrogate() ? kMaxNameLength - 1 : kMaxNameLength);
    }
    out = stripSpacesAndDots(out);
    return out == QStringLiteral(".") || out == QStringLiteral("..") ? QString() : out;
}

QString lengthText(double beats) {
    if (!util_isfinite(beats) || beats <= 0) {
        return QString();
    }
    if (beats == std::round(beats)) {
        const auto whole = static_cast<qint64>(beats);
        if (whole >= 4 && whole % 4 == 0) {
            const qint64 bars = whole / 4;
            return bars == 1 ? tr("1 bar") : tr("%1 bars").arg(bars);
        }
        return whole == 1 ? tr("1 beat") : tr("%1 beats").arg(whole);
    }
    return tr("%1 beats").arg(QString::number(beats, 'g', 3));
}

QString sampleName(const QString& title, double beats, double seconds) {
    const QString length = lengthText(beats);
    const QString suffix = QStringLiteral(" - ") + tr("Loop") + QLatin1Char(' ') +
            (length.isEmpty() ? tr("%1 s").arg(QString::number(std::max(0.0, seconds), 'f', 1)) : length);
    QString name = cleanName(title);
    if (name.isEmpty()) {
        name = tr("Untitled");
    }
    const int room = kMaxNameLength - static_cast<int>(suffix.size());
    if (name.size() > room) {
        name = stripSpacesAndDots(name.left(std::max(1, room)));
    }
    return cleanName(name + suffix);
}

QString candidatePath(const QString& folder, const QString& name, int number) {
    const QString label = number <= 1 ? name : QStringLiteral("%1 %2").arg(name).arg(number);
    return QDir(folder).filePath(label + QStringLiteral(".wav"));
}

QString uniquePath(const QString& folder, const QString& name) {
    for (int number = 1; number < 1000; ++number) {
        const QString path = candidatePath(folder, name, number);
        if (!QFileInfo::exists(path)) {
            return path;
        }
    }
    return QString();
}

Plan plan(bool enabled,
        double loopStart,
        double loopEnd,
        double sampleRate,
        const std::shared_ptr<const mixxx::Beats>& beats) {
    Plan result;
    if (!util_isfinite(sampleRate) || sampleRate <= 0) {
        result.refusal = Refusal::NoTrack;
        return result;
    }
    if (!enabled || !util_isfinite(loopStart) || !util_isfinite(loopEnd) || loopStart < 0 ||
            loopEnd <= loopStart) {
        result.refusal = Refusal::NoLoop;
        return result;
    }
    // Engine sample positions are two per frame.
    result.startFrame = std::llround(loopStart / 2);
    const qint64 endFrame = std::llround(loopEnd / 2);
    result.frames = endFrame - result.startFrame;
    result.seconds = result.frames / sampleRate;
    if (beats) {
        const mixxx::Bpm bpm = beats->getBpmInRange(
                mixxx::audio::FramePos(static_cast<double>(result.startFrame)),
                mixxx::audio::FramePos(static_cast<double>(endFrame)));
        if (bpm.isValid() && bpm.value() > 0) {
            result.beats = snapBeats(result.seconds * bpm.value() / 60);
        }
    }
    if (result.frames < kMinFrames) {
        result.refusal = Refusal::TooShort;
    } else if (result.beats > kMaxBeats + 0.01 || result.seconds > kMaxSeconds + 0.01) {
        result.refusal = Refusal::TooLong;
    } else {
        result.refusal = Refusal::None;
    }
    return result;
}

QString refusalText(Refusal refusal) {
    switch (refusal) {
    case Refusal::NoTrack:
        return tr("Load a track first");
    case Refusal::NoLoop:
        return tr("Set a loop first");
    case Refusal::TooShort:
        return tr("This loop is too short for a pad");
    case Refusal::TooLong:
        return tr("Too long for a pad: up to 32 bars or 60 s");
    case Refusal::Busy:
        return tr("Still saving the last loop");
    case Refusal::None:
        break;
    }
    return QString();
}

QString errorText(Result::Status status) {
    switch (status) {
    case Result::Status::Unreadable:
        return tr("Not saved: this track can't be read");
    case Result::Status::NoSpace:
        return tr("Not saved: the disk is full");
    case Result::Status::NotWritable:
        return tr("Not saved: can't write to Music/Deck Lab Samples");
    case Result::Status::Cancelled:
        return tr("Not saved");
    case Result::Status::Saved:
        break;
    }
    return QString();
}

Result write(const Request& request, const std::atomic<bool>* cancel) {
    using Status = Result::Status;
    if (request.frames < kMinFrames || request.startFrame < 0 || request.source.isEmpty()) {
        return failed(Status::Unreadable);
    }
    if (!QDir().mkpath(request.folder) || !QFileInfo(request.folder).isWritable()) {
        return failed(Status::NotWritable);
    }
    // Mixxx's decoder, on this (worker) thread, through a track object of
    // its own: the deck's track is never touched.
    const TrackPointer track = Track::newTemporary(request.source);
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());
    mixxx::AudioSourcePointer source = SoundSourceProxy(track).openAudioSource(params);
    if (!source) {
        return failed(Status::Unreadable);
    }
    if (source->getSignalInfo().getChannelCount() != mixxx::audio::ChannelCount::stereo()) {
        // Mono (or more channels): as the engine hears it, two channels.
        source = mixxx::AudioSourceStereoProxy::create(source, kChunkFrames);
    }
    const int sampleRate = static_cast<int>(source->getSignalInfo().getSampleRate());
    // Engine frame 0 is the source's first frame.
    const mixxx::IndexRange wanted = mixxx::IndexRange::forward(
            source->frameIndexMin() + static_cast<SINT>(request.startFrame),
            static_cast<SINT>(request.frames));
    mixxx::IndexRange remaining = mixxx::intersect(wanted, source->frameIndexRange());
    if (sampleRate <= 0 || remaining.empty()) {
        source->close();
        return failed(Status::Unreadable);
    }
    const qint64 expectedBytes = static_cast<qint64>(remaining.length()) * kBytesPerFrame;
    const QStorageInfo storage(request.folder);
    if (storage.isValid() && storage.isReady() &&
            storage.bytesAvailable() < expectedBytes + 4096 + request.reserveBytes) {
        source->close();
        return failed(Status::NoSpace);
    }
    const QString name = QFileInfo(uniquePath(request.folder, sampleName(request.title, request.beats,
                                                                      request.frames / double(sampleRate))))
                                 .completeBaseName();
    if (name.isEmpty()) {
        source->close();
        return failed(Status::NotWritable);
    }
    // A hidden temporary file in the folder (as Sample Maker's ".sample-"),
    // removed unless it is renamed into place.
    QTemporaryFile file(QDir(request.folder).filePath(QStringLiteral(".sample-XXXXXX.wav")));
    if (!file.open()) {
        source->close();
        return failed(Status::NotWritable);
    }
    int dataSizeOffset = 0;
    QByteArray header = wavHeader(sampleRate, name, expectedBytes, &dataSizeOffset);
    if (file.write(header) != header.size()) {
        source->close();
        return failed(writeError(file));
    }
    mixxx::SampleBuffer buffer(kChunkFrames * kChannels);
    std::vector<qint16> pcm(static_cast<size_t>(kChunkFrames * kChannels));
    qint64 written = 0;
    const auto writeFrames = [&](const CSAMPLE* samples, SINT frames) {
        for (SINT index = 0; index < frames * kChannels; ++index) {
            // Float to 16 bits on the decoder's own scale (1.0 = 32768),
            // so 16-bit sources come back sample for sample.
            const float value = samples ? samples[index] : 0.0f;
            const long scaled = std::lround(std::clamp(value * 32768.0f, -32768.0f, 32767.0f));
            pcm[static_cast<size_t>(index)] = qToLittleEndian(static_cast<qint16>(scaled));
        }
        const qint64 bytes = static_cast<qint64>(frames) * kBytesPerFrame;
        if (file.write(reinterpret_cast<const char*>(pcm.data()), bytes) != bytes) {
            return false;
        }
        written += frames;
        return true;
    };
    while (!remaining.empty()) {
        if (cancel && cancel->load()) {
            source->close();
            return failed(Status::Cancelled);
        }
        const mixxx::IndexRange chunk = remaining.splitAndShrinkFront(
                std::min(kChunkFrames, remaining.length()));
        const mixxx::ReadableSampleFrames readable = source->readSampleFrames(
                mixxx::WritableSampleFrames(chunk, mixxx::SampleBuffer::WritableSlice(buffer)));
        const mixxx::IndexRange got = readable.frameIndexRange();
        bool ok = true;
        if (got.empty()) {
            // A gap the decoder could not fill: silence keeps the length.
            ok = writeFrames(nullptr, chunk.length());
        } else {
            ok = writeFrames(nullptr, got.start() - chunk.start()) &&
                    writeFrames(readable.readableData(), got.length()) &&
                    writeFrames(nullptr, chunk.end() - got.end());
        }
        if (!ok) {
            source->close();
            return failed(writeError(file));
        }
        // The decoder may find the stream shorter than it said.
        remaining = mixxx::intersect(remaining, source->frameIndexRange());
    }
    source->close();
    const qint64 dataBytes = written * kBytesPerFrame;
    if (dataBytes != expectedBytes) {
        setRiffSizes(&header, dataSizeOffset, dataBytes);
        if (!file.seek(0) || file.write(header) != header.size()) {
            return failed(writeError(file));
        }
    }
    if (!file.flush()) {
        return failed(writeError(file));
    }
    // Into place under the first free name (never over another sample).
    for (int number = 1; number < 1000; ++number) {
        const QString path = candidatePath(request.folder, sampleName(request.title, request.beats,
                                                                   request.frames / double(sampleRate)),
                number);
        if (QFileInfo::exists(path)) {
            continue;
        }
        if (file.rename(path)) {
            file.setAutoRemove(false);
            Result result;
            result.status = Status::Saved;
            result.path = path;
            result.frames = written;
            result.sampleRate = sampleRate;
            result.bpm = request.beats > 0 && written > 0 ? request.beats * 60.0 * sampleRate / written : 0;
            return result;
        }
        if (!QFileInfo::exists(path)) {
            break;
        }
    }
    return failed(Status::NotWritable);
}

TrackPointer padTrack(const Result& result) {
    TrackPointer track = Track::newTemporary(result.path);
    track->setTitle(QFileInfo(result.path).completeBaseName()); // the file's own title
    if (result.sampleRate > 0) {
        track->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(static_cast<mixxx::audio::SampleRate::value_t>(result.sampleRate)),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(double(result.frames) / result.sampleRate));
    }
    applyGrid(track, result);
    return track;
}

bool applyGrid(const TrackPointer& track, const Result& result) {
    if (!track || !(result.bpm > 0) || result.sampleRate <= 0 || track->isBpmLocked()) {
        return false;
    }
    if (!track->trySetBeats(mixxx::Beats::fromConstTempo(
                mixxx::audio::SampleRate(static_cast<mixxx::audio::SampleRate::value_t>(result.sampleRate)),
                mixxx::audio::kStartFramePos,
                mixxx::Bpm(result.bpm)))) {
        return false;
    }
    track->setBpmLocked(true);
    return true;
}

} // namespace decklab::capture

// ---------------------------------------------------------------------------
// The "which pad?" sheet

namespace {
const QString kChooserStyle = QStringLiteral(R"(
 QWidget#decklab-pad-chooser {background:#16191E;border:1px solid #9AA3AF;border-radius:3px;}
 QWidget#decklab-pad-chooser QLabel {background:transparent;font-family:"Barlow";font-size:16px;font-weight:500;color:#F2F4F7;}
 QWidget#decklab-pad-chooser QLabel[heading="true"] {font-size:18px;font-weight:600;}
 QWidget#decklab-pad-chooser QLabel[muted="true"] {color:#9AA3AF;}
 QWidget#decklab-pad-chooser QPushButton {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;border-radius:3px;
     padding:0px 8px;min-height:42px;max-height:42px;font-family:"Barlow";font-size:16px;font-weight:600;text-align:left;}
 QWidget#decklab-pad-chooser QPushButton:pressed {background:#20242B;}
 QWidget#decklab-pad-chooser QPushButton:disabled {background:#111418;color:#4B525C;border-color:#1E2228;}
 QWidget#decklab-pad-chooser QPushButton#decklab-pad-chooser-cancel {text-align:center;}
 QWidget#decklab-pad-chooser QPushButton[knobCursor="true"] {border:2px solid #F2F4F7;}
 )");
} // namespace

DeckLabPadChooser::DeckLabPadChooser(QWidget* parent)
        : QWidget(parent) {
    setObjectName(QStringLiteral("decklab-pad-chooser"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(kChooserStyle);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(12, 10, 12, 12);
    column->setSpacing(6);
    auto* heading = new QLabel(tr("NO EMPTY PAD · REPLACE WHICH?"), this);
    heading->setProperty("heading", true);
    heading->setObjectName(QStringLiteral("decklab-pad-chooser-heading"));
    column->addWidget(heading);
    m_sample = new QLabel(this);
    m_sample->setProperty("muted", true);
    m_sample->setObjectName(QStringLiteral("decklab-pad-chooser-sample"));
    m_sample->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    column->addWidget(m_sample);
    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    for (int index = 0; index < kPads; ++index) {
        auto* b = new QPushButton(this);
        b->setObjectName(QStringLiteral("decklab-pad-chooser-%1").arg(index + 1));
        b->setFocusPolicy(Qt::NoFocus);
        b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        // Replacing is destructive: on release.
        connect(b, &QPushButton::clicked, this, [this, index] {
            const int sampler = m_samplers[index];
            if (!m_asking || sampler <= 0) {
                return;
            }
            m_asking = false;
            hide();
            emit chosen(sampler);
        });
        grid->addWidget(b, index / 4, index % 4);
        m_pads[index] = b;
    }
    column->addLayout(grid);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(new QLabel(tr("The sample stays in Deck Lab Samples."), this), 1);
    m_cancel = new QPushButton(tr("CANCEL"), this);
    m_cancel->setObjectName(QStringLiteral("decklab-pad-chooser-cancel"));
    m_cancel->setFocusPolicy(Qt::NoFocus);
    m_cancel->setMinimumWidth(120);
    connect(m_cancel, &QPushButton::clicked, this, [this] { cancel(); });
    bottom->addWidget(m_cancel);
    column->addLayout(bottom);
    hide();
    // It opens over the Decks page, waveforms included, without hiding them
    // mid-set.
    decklab::overlay::keepAboveWaveforms(this);
}

DeckLabPadChooser::~DeckLabPadChooser() {
    decklab::releaseKnob(this);
}

void DeckLabPadChooser::ask(const QString& sample, const QList<Pad>& pads) {
    m_sample->setText(QFontMetrics(m_sample->font()).elidedText(sample, Qt::ElideMiddle, 520));
    for (int index = 0; index < kPads; ++index) {
        auto* b = m_pads[index];
        if (index >= pads.size()) {
            m_samplers[index] = 0;
            b->setText(QString());
            b->setEnabled(false);
            b->setVisible(false);
            continue;
        }
        const Pad& pad = pads[index];
        m_samplers[index] = pad.sampler;
        const QString title = pad.title.trimmed().isEmpty() ? tr("Untitled") : pad.title.trimmed();
        b->setVisible(true);
        b->setText(QFontMetrics(b->font()).elidedText(
                QStringLiteral("%1  %2").arg(pad.sampler).arg(title), Qt::ElideRight, 108));
        b->setEnabled(!pad.playing);
        b->setAccessibleName(pad.playing ? tr("Pad %1, %2, playing: stop it to replace it").arg(pad.sampler).arg(title)
                                         : tr("Replace pad %1, %2").arg(pad.sampler).arg(title));
    }
    m_asking = true;
    showCursor(nullptr);
    place();
    show();
    decklab::overlay::raiseAboveWaveforms(this);
    decklab::claimKnob(this);
}

QList<QPushButton*> DeckLabPadChooser::cursorTargets() const {
    QList<QPushButton*> targets;
    for (auto* b : m_pads) {
        if (b->isVisible() && b->isEnabled()) {
            targets.append(b);
        }
    }
    targets.append(m_cancel);
    return targets;
}

QPushButton* DeckLabPadChooser::cursorButton() const {
    return m_cursor;
}

void DeckLabPadChooser::showCursor(QPushButton* button) {
    if (m_cursor == button) {
        return;
    }
    for (auto* b : {m_cursor, button}) {
        if (!b) {
            continue;
        }
        b->setProperty("knobCursor", b == button);
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
    m_cursor = button;
}

void DeckLabPadChooser::keyPressEvent(QKeyEvent* event) {
    // The knob claim sends Tab/Backtab (or Up/Down) for a turn, Space or
    // Return for a press and Escape for back (decklab::forwardKnob).
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Left:
    case Qt::Key_Backtab:
    case Qt::Key_Down:
    case Qt::Key_Right:
    case Qt::Key_Tab: {
        const QList<QPushButton*> targets = cursorTargets();
        if (!m_cursor || !targets.contains(m_cursor)) {
            // The first turn only shows the cursor, on CANCEL.
            showCursor(m_cancel);
        } else {
            const bool back = event->key() == Qt::Key_Up || event->key() == Qt::Key_Left ||
                    event->key() == Qt::Key_Backtab;
            const auto count = targets.size();
            const auto index = (targets.indexOf(m_cursor) + (back ? count - 1 : 1)) % count;
            showCursor(targets.at(index));
        }
        event->accept();
        return;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (m_cursor && m_cursor->isVisible() && m_cursor->isEnabled()) {
            m_cursor->click();
        } else if (!m_cursor) {
            showCursor(m_cancel); // a press before any turn shows where it is
        }
        event->accept();
        return;
    case Qt::Key_Escape:
        cancel();
        event->accept();
        return;
    case Qt::Key_Select:
        event->accept(); // the knob held down: nothing until it turns or is let go
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void DeckLabPadChooser::place() {
    QWidget* host = parentWidget();
    const int width = 560;
    const int height = std::max(sizeHint().height(), 300);
    if (!host) {
        resize(width, height);
        return;
    }
    const int w = std::min(width, host->width() - 16);
    const int h = std::min(height, host->height() - 16);
    setGeometry((host->width() - w) / 2, std::max(8, (host->height() - h) / 2), w, h);
}

void DeckLabPadChooser::cancel() {
    if (!m_asking) {
        return;
    }
    m_asking = false;
    hide();
    emit cancelled();
}

QPushButton* DeckLabPadChooser::padButton(int sampler) const {
    for (int index = 0; index < kPads; ++index) {
        if (m_samplers[index] == sampler && m_pads[index]->isVisible()) {
            return m_pads[index];
        }
    }
    return nullptr;
}

void DeckLabPadChooser::hideEvent(QHideEvent* event) {
    // A page change hides it under a finger: no choice is made then.
    QWidget::hideEvent(event);
    decklab::releaseKnob(this);
    showCursor(nullptr);
    if (m_asking) {
        cancel();
    }
}

// ---------------------------------------------------------------------------
// TO PAD

DeckLabLoopCapture::DeckLabLoopCapture(Host host, QObject* parent)
        : QObject(parent),
          m_host(std::move(host)),
          m_cancel(std::make_shared<std::atomic<bool>>(false)) {
    setObjectName(QStringLiteral("decklab-loop-capture"));
}

DeckLabLoopCapture::~DeckLabLoopCapture() {
    if (m_worker) {
        m_cancel->store(true);
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }
}

decklab::capture::Refusal DeckLabLoopCapture::capture(int deck) {
    using decklab::capture::Refusal;
    const auto refuse = [this, deck](Refusal refusal) {
        if (m_host.tell) {
            m_host.tell(deck, decklab::capture::refusalText(refusal), refusal != Refusal::Busy);
        }
        return refusal;
    };
    if (m_worker) {
        return refuse(Refusal::Busy);
    }
    const TrackPointer track = m_host.track ? m_host.track(deck) : TrackPointer();
    if (!track || track->getLocation().isEmpty()) {
        return refuse(Refusal::NoTrack);
    }
    const auto read = [this, deck](const char* item) {
        return m_host.read ? m_host.read(deck, QString::fromLatin1(item)) : 0.0;
    };
    const decklab::capture::Plan plan = decklab::capture::plan(read("loop_enabled") > 0,
            read("loop_start_position"),
            read("loop_end_position"),
            static_cast<double>(track->getSampleRate().value()),
            track->getBeats());
    if (plan.refusal != Refusal::None) {
        return refuse(plan.refusal);
    }
    decklab::capture::Request request;
    request.source = track->getLocation();
    request.title = track->getTitle().trimmed();
    if (request.title.isEmpty()) {
        request.title = QFileInfo(request.source).completeBaseName();
    }
    request.startFrame = plan.startFrame;
    request.frames = plan.frames;
    request.beats = plan.beats;
    request.folder = decklab::samplesFolder();
    m_cancel->store(false);
    auto result = std::make_shared<decklab::capture::Result>();
    auto cancel = m_cancel;
    QThread* worker = QThread::create([request, result, cancel] {
        *result = decklab::capture::write(request, cancel.get());
    });
    worker->setObjectName(QStringLiteral("DeckLabCapture"));
    connect(worker, &QThread::finished, this, [this, worker, result, deck] {
        if (m_worker == worker) {
            m_worker = nullptr;
        }
        worker->deleteLater();
        written(deck, *result);
    });
    m_worker = worker;
    // Below everything else: the audio, the GUI and the waveforms come first.
    worker->start(QThread::IdlePriority);
    return Refusal::None;
}

void DeckLabLoopCapture::written(int deck, const decklab::capture::Result& result) {
    if (result.status != decklab::capture::Result::Status::Saved) {
        if (result.status != decklab::capture::Result::Status::Cancelled && m_host.tell) {
            m_host.tell(deck, decklab::capture::errorText(result.status), true);
        }
        emit finished(deck, 0, result);
        return;
    }
    const int pads = m_host.pads ? m_host.pads() : 0;
    if (m_host.emptyPad) {
        if (const int sampler = m_host.emptyPad(); sampler > 0) {
            place(deck, sampler, result);
            return;
        }
    } else {
        for (int sampler = 1; sampler <= pads; ++sampler) {
            if (m_host.pad && !m_host.pad(sampler).loaded) {
                place(deck, sampler, result);
                return;
            }
        }
    }
    // Every pad is taken: ask which to replace (the first bank's pads).
    QList<DeckLabPadChooser::Pad> shown;
    for (int sampler = 1; sampler <= std::min(pads, DeckLabPadChooser::kPads); ++sampler) {
        const Pad pad = m_host.pad ? m_host.pad(sampler) : Pad();
        shown.append({sampler, pad.title, pad.playing});
    }
    QWidget* parent = m_host.sheetParent ? m_host.sheetParent() : nullptr;
    if (shown.isEmpty() || !parent) {
        if (m_host.tell) {
            m_host.tell(deck, tr("Saved in Deck Lab Samples"), false);
        }
        emit finished(deck, 0, result);
        return;
    }
    if (!m_chooser || m_chooser->parentWidget() != parent) {
        delete m_chooser;
        m_chooser = new DeckLabPadChooser(parent);
        connect(m_chooser, &DeckLabPadChooser::chosen, this, [this](int sampler) {
            const int deckNow = std::exchange(m_pendingDeck, 0);
            if (deckNow > 0) {
                place(deckNow, sampler, m_pending);
            }
        });
        connect(m_chooser, &DeckLabPadChooser::cancelled, this, [this] {
            const int deckNow = std::exchange(m_pendingDeck, 0);
            if (deckNow > 0) {
                if (m_host.tell) {
                    m_host.tell(deckNow, tr("Saved in Deck Lab Samples, not on a pad"), false);
                }
                emit finished(deckNow, 0, m_pending);
            }
        });
    }
    // A sample still waiting from before stays in the folder.
    m_chooser->cancel();
    m_pendingDeck = deck;
    m_pending = result;
    m_chooser->ask(QFileInfo(result.path).completeBaseName(), shown);
}

void DeckLabLoopCapture::place(int deck, int sampler, const decklab::capture::Result& result) {
    const bool loaded = m_host.load && m_host.load(sampler, result);
    if (m_host.tell) {
        m_host.tell(deck,
                loaded ? tr("Saved to pad %1").arg(sampler)
                       : tr("Saved in Deck Lab Samples: pad %1 can't take it now").arg(sampler),
                !loaded);
    }
    emit finished(deck, loaded ? sampler : 0, result);
}

void DeckLabLoopCapture::cancelChoice() {
    if (m_chooser) {
        m_chooser->cancel();
    }
}

DeckLabCaptureLibrary::DeckLabCaptureLibrary(Hooks hooks, QObject* parent)
        : QObject(parent),
          m_hooks(std::move(hooks)) {
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &DeckLabCaptureLibrary::flush);
}

void DeckLabCaptureLibrary::add(const decklab::capture::Result& result) {
    if (result.path.isEmpty()) {
        return;
    }
    m_pending.append(result);
    flush();
}

void DeckLabCaptureLibrary::flush() {
    if (m_pending.isEmpty()) {
        return;
    }
    if (!m_hooks.add || (m_hooks.quiet && !m_hooks.quiet())) {
        m_retry.start();
        return;
    }
    addPending();
}

void DeckLabCaptureLibrary::flushNow() {
    if (m_pending.isEmpty() || !m_hooks.add || (m_hooks.scanning && m_hooks.scanning())) {
        return;
    }
    m_retry.stop();
    addPending();
}

void DeckLabCaptureLibrary::addPending() {
    const QList<decklab::capture::Result> pending = std::exchange(m_pending, {});
    for (const auto& result : pending) {
        // Its tags are read and its row written now; the grid is saved with
        // the row when the last reference goes (a moment later).
        const TrackPointer track = m_hooks.add(result.path);
        if (track) {
            decklab::capture::applyGrid(track, result);
        } else {
            qWarning() << "Deck Lab TO PAD: the sample could not be added to the library; the next scan adds it";
        }
    }
}
