// SPDX-License-Identifier: GPL-2.0-or-later
// r7-sampler: TO PAD (save a deck's loop to a sampler pad) against generated
// WAV files in a temporary folder: Mixxx's decoder, the written sample, its
// name, the length cap and where it lands. No engine, no audio.
// NativeDeckLabTest covers the key in the Decks page's MORE sheet.
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cmath>
#include <memory>
#include <vector>

#include "decklab/decklabdownloads.h"
#include "library/decklabknob.h"
#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/track.h"
#include "track/trackmetadata.h"
#include "widget/decklabcapture.h"

namespace {

using decklab::capture::Refusal;
using Status = decklab::capture::Result::Status;

constexpr int kRate = 44100;
constexpr qint64 kSourceFrames = kRate * 6;

// The generated source: a different value in every frame, so the test can
// tell exactly which frames were written.
qint16 left(qint64 frame) {
    return static_cast<qint16>((frame * 7) % 20000 - 10000);
}
qint16 right(qint64 frame) {
    return static_cast<qint16>(-left(frame));
}

// A plain 16-bit stereo WAV, written here (not with the code under test).
bool writeSource(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const quint32 bytes = static_cast<quint32>(kSourceFrames * 4);
    QByteArray data(44, '\0');
    memcpy(data.data(), "RIFF", 4);
    qToLittleEndian<quint32>(36 + bytes, data.data() + 4);
    memcpy(data.data() + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, data.data() + 16);
    qToLittleEndian<quint16>(1, data.data() + 20);
    qToLittleEndian<quint16>(2, data.data() + 22);
    qToLittleEndian<quint32>(kRate, data.data() + 24);
    qToLittleEndian<quint32>(kRate * 4, data.data() + 28);
    qToLittleEndian<quint16>(4, data.data() + 32);
    qToLittleEndian<quint16>(16, data.data() + 34);
    memcpy(data.data() + 36, "data", 4);
    qToLittleEndian<quint32>(bytes, data.data() + 40);
    std::vector<qint16> samples(static_cast<size_t>(kSourceFrames * 2));
    for (qint64 frame = 0; frame < kSourceFrames; ++frame) {
        samples[static_cast<size_t>(frame * 2)] = qToLittleEndian(left(frame));
        samples[static_cast<size_t>(frame * 2 + 1)] = qToLittleEndian(right(frame));
    }
    data.append(reinterpret_cast<const char*>(samples.data()), bytes);
    return file.write(data) == data.size();
}

// What a written sample holds, read back as bytes.
struct Wav {
    int format = 0;
    int channels = 0;
    int rate = 0;
    int bits = 0;
    QString title;
    QByteArray data;
};

Wav readWav(const QString& path) {
    Wav wav;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return wav;
    }
    const QByteArray all = file.readAll();
    if (all.size() < 12 || !all.startsWith("RIFF") || all.mid(8, 4) != "WAVE") {
        return wav;
    }
    if (qFromLittleEndian<quint32>(all.constData() + 4) != static_cast<quint32>(all.size() - 8)) {
        return wav;
    }
    qsizetype at = 12;
    while (at + 8 <= all.size()) {
        const QByteArray id = all.mid(at, 4);
        const auto size = static_cast<qsizetype>(qFromLittleEndian<quint32>(all.constData() + at + 4));
        const char* body = all.constData() + at + 8;
        if (id == "fmt ") {
            wav.format = qFromLittleEndian<quint16>(body);
            wav.channels = qFromLittleEndian<quint16>(body + 2);
            wav.rate = static_cast<int>(qFromLittleEndian<quint32>(body + 4));
            wav.bits = qFromLittleEndian<quint16>(body + 14);
        } else if (id == "LIST" && all.mid(at + 8, 4) == "INFO" && all.mid(at + 12, 4) == "INAM") {
            wav.title = QString::fromUtf8(all.constData() + at + 20);
        } else if (id == "data") {
            wav.data = all.mid(at + 8, size);
        }
        at += 8 + size + (size % 2);
    }
    return wav;
}

mixxx::BeatsPointer grid(double bpm) {
    return mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kRate), mixxx::audio::kStartFramePos, mixxx::Bpm(bpm));
}

// Engine samples (two per frame) for a loop of `beats` at `bpm` from `start`.
double samples(qint64 frames) {
    return 2.0 * static_cast<double>(frames);
}
qint64 framesFor(double beats, double bpm) {
    return std::llround(beats * 60.0 * kRate / bpm);
}

class DeckLabCaptureTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(SoundSourceProxy::isFileTypeSupported(QStringLiteral("wav")) ||
                SoundSourceProxy::registerProviders());
        ASSERT_TRUE(dir.isValid());
        source = dir.filePath(QStringLiteral("Generated Tone.wav"));
        ASSERT_TRUE(writeSource(source));
        // r7: one samples folder for TO PAD, PUT ON PAD, SAMPLES and the
        // song lists' filter: decklab::samplesFolder(), ~/Music/Deck Lab Samples.
        decklab::setMusicFolderForTesting(dir.filePath(QStringLiteral("Music")));
        folder = dir.filePath(QStringLiteral("Music/Deck Lab Samples"));
    }
    void TearDown() override {
        decklab::setMusicFolderForTesting(QString());
    }
    decklab::capture::Request request(qint64 start, qint64 frames, double beats) const {
        decklab::capture::Request r;
        r.source = source;
        r.title = QStringLiteral("Generated Tone");
        r.startFrame = start;
        r.frames = frames;
        r.beats = beats;
        r.folder = folder;
        return r;
    }
    QStringList saved() const {
        return QDir(folder).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    }
    void screenshot(QWidget* widget, const QString& name) const {
        const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
        if (!output.isEmpty()) {
            EXPECT_TRUE(widget->grab().save(QDir(output).filePath(name))) << name.toStdString();
        }
    }
    QTemporaryDir dir;
    QString source;
    QString folder;
};

// A deck with its loop, a fake set of pads and what the capture told.
struct FakeDecks {
    TrackPointer track;
    bool looping = true;
    double start = 0;
    double end = 0;
    std::vector<DeckLabLoopCapture::Pad> pads = std::vector<DeckLabLoopCapture::Pad>(16);
    QList<std::pair<int, decklab::capture::Result>> loads;
    QStringList told;
    QList<bool> errors;
    QWidget* parent = nullptr;

    DeckLabLoopCapture::Host host() {
        DeckLabLoopCapture::Host h;
        h.read = [this](int deck, const QString& item) {
            if (deck != 1) {
                return 0.0;
            }
            if (item == QStringLiteral("loop_enabled")) {
                return looping ? 1.0 : 0.0;
            }
            if (item == QStringLiteral("loop_start_position")) {
                return start;
            }
            if (item == QStringLiteral("loop_end_position")) {
                return end;
            }
            return 0.0;
        };
        h.track = [this](int deck) { return deck == 1 ? track : TrackPointer(); };
        h.pads = [this] { return static_cast<int>(pads.size()); };
        h.pad = [this](int sampler) { return pads[static_cast<size_t>(sampler - 1)]; };
        h.load = [this](int sampler, const decklab::capture::Result& result) {
            if (pads[static_cast<size_t>(sampler - 1)].playing) {
                return false;
            }
            loads.append({sampler, result});
            return true;
        };
        h.tell = [this](int, const QString& text, bool error) {
            told.append(text);
            errors.append(error);
        };
        h.sheetParent = [this] { return parent; };
        return h;
    }
};

TEST_F(DeckLabCaptureTest, WritesExactlyTheLoopFramesAsA16BitStereoWav) {
    // Four beats at 120 BPM from half a second in.
    const qint64 start = kRate / 2;
    const qint64 frames = framesFor(4, 120);
    const auto result = decklab::capture::write(request(start, frames, 4));
    ASSERT_EQ(Status::Saved, result.status);
    EXPECT_EQ(frames, result.frames);
    EXPECT_EQ(kRate, result.sampleRate) << "the track's own rate, never resampled";
    EXPECT_NEAR(120.0, result.bpm, 1e-9) << "whole beats over the sample's length";
    EXPECT_EQ(QDir(folder).filePath(QStringLiteral("Generated Tone - Loop 1 bar.wav")), result.path);
    EXPECT_TRUE(QDir(folder).exists()) << "the folder is made when it is missing";
    const Wav wav = readWav(result.path);
    EXPECT_EQ(1, wav.format) << "PCM";
    EXPECT_EQ(2, wav.channels);
    EXPECT_EQ(kRate, wav.rate);
    EXPECT_EQ(16, wav.bits);
    EXPECT_EQ(QStringLiteral("Generated Tone - Loop 1 bar"), wav.title) << "the whole name is the title";
    ASSERT_EQ(frames * 4, wav.data.size());
    // Exactly the loop's frames, sample for sample.
    int wrong = 0;
    for (qint64 frame = 0; frame < frames; ++frame) {
        const qint16 l = qFromLittleEndian<qint16>(wav.data.constData() + frame * 4);
        const qint16 r = qFromLittleEndian<qint16>(wav.data.constData() + frame * 4 + 2);
        if (std::abs(l - left(start + frame)) > 1 || std::abs(r - right(start + frame)) > 1) {
            ++wrong;
        }
    }
    EXPECT_EQ(0, wrong);
    // Mixxx reads it back with the same length, and the title from the file
    // whole (an untagged file's name would be split at " - ").
    auto readBack = SoundSourceProxy(Track::newTemporary(result.path)).openAudioSource();
    ASSERT_TRUE(readBack);
    EXPECT_EQ(frames, readBack->frameLength());
    readBack->close();
    mixxx::TrackMetadata metadata;
    SoundSourceProxy(Track::newTemporary(result.path)).importTrackMetadataAndCoverImage(&metadata, nullptr, false);
    EXPECT_EQ(QStringLiteral("Generated Tone - Loop 1 bar"), metadata.getTrackInfo().getTitle());
    // The same loop again gets the next free name; nothing half-written stays.
    const auto again = decklab::capture::write(request(start, frames, 4));
    ASSERT_EQ(Status::Saved, again.status);
    EXPECT_EQ(QDir(folder).filePath(QStringLiteral("Generated Tone - Loop 1 bar 2.wav")), again.path);
    const QStringList files = saved();
    EXPECT_EQ(2, files.size()) << files.join(QLatin1Char('|')).toStdString();
    EXPECT_TRUE(files.contains(QStringLiteral("Generated Tone - Loop 1 bar.wav")));
    EXPECT_TRUE(files.contains(QStringLiteral("Generated Tone - Loop 1 bar 2.wav")));
}

TEST_F(DeckLabCaptureTest, PlanReadsTheDeckLoopAndCapsTheLength) {
    // The deck's loop controls are engine samples: two per frame.
    const qint64 start = kRate;
    const qint64 frames = framesFor(4, 120);
    auto plan = decklab::capture::plan(true, samples(start), samples(start + frames), kRate, grid(120));
    EXPECT_EQ(Refusal::None, plan.refusal);
    EXPECT_EQ(start, plan.startFrame);
    EXPECT_EQ(frames, plan.frames);
    EXPECT_DOUBLE_EQ(4, plan.beats);
    EXPECT_DOUBLE_EQ(2, plan.seconds);
    // The engine rounds loop ends to frames: still four whole beats.
    plan = decklab::capture::plan(true, samples(start), samples(start + frames + 3), kRate, grid(120));
    EXPECT_DOUBLE_EQ(4, plan.beats);
    plan = decklab::capture::plan(true, samples(start), samples(start + framesFor(0.5, 120)), kRate, grid(120));
    EXPECT_DOUBLE_EQ(0.5, plan.beats);
    // No loop, or none that makes sense.
    EXPECT_EQ(Refusal::NoLoop, decklab::capture::plan(false, 0, samples(frames), kRate, grid(120)).refusal);
    EXPECT_EQ(Refusal::NoLoop, decklab::capture::plan(true, samples(frames), samples(start), kRate, grid(120)).refusal);
    EXPECT_EQ(Refusal::NoTrack, decklab::capture::plan(true, 0, samples(frames), 0, grid(120)).refusal);
    EXPECT_EQ(Refusal::TooShort, decklab::capture::plan(true, 0, samples(10), kRate, grid(120)).refusal);
    // The cap: 32 bars, and 60 s.
    EXPECT_EQ(Refusal::None, decklab::capture::plan(true, 0, samples(framesFor(128, 140)), kRate, grid(140)).refusal)
            << "32 bars at 140 BPM is 55 s";
    EXPECT_EQ(Refusal::TooLong, decklab::capture::plan(true, 0, samples(framesFor(132, 140)), kRate, grid(140)).refusal)
            << "33 bars";
    EXPECT_EQ(Refusal::TooLong, decklab::capture::plan(true, 0, samples(framesFor(128, 120)), kRate, grid(120)).refusal)
            << "32 bars at 120 BPM is 64 s";
    EXPECT_EQ(Refusal::None, decklab::capture::plan(true, 0, samples(framesFor(60, 60)), kRate, grid(60)).refusal)
            << "60 s exactly";
    EXPECT_EQ(Refusal::TooLong, decklab::capture::plan(true, 0, samples(framesFor(64, 60)), kRate, grid(60)).refusal);
    // Without a grid: the length in seconds, no beats.
    plan = decklab::capture::plan(true, 0, samples(kRate * 5), kRate, nullptr);
    EXPECT_EQ(Refusal::None, plan.refusal);
    EXPECT_DOUBLE_EQ(0, plan.beats);
    EXPECT_DOUBLE_EQ(5, plan.seconds);
    // The screen says the limit.
    const QString tooLong = decklab::capture::refusalText(Refusal::TooLong);
    EXPECT_TRUE(tooLong.contains(QStringLiteral("32 bars")) && tooLong.contains(QStringLiteral("60 s")))
            << tooLong.toStdString();
}

TEST_F(DeckLabCaptureTest, NamesAreCleanedLikeSampleMakerAndMadeUnique) {
    using decklab::capture::cleanName;
    using decklab::capture::lengthText;
    using decklab::capture::sampleName;
    EXPECT_EQ(QStringLiteral("My Track Name"), cleanName(QStringLiteral("  My:Track/Name?  ")));
    EXPECT_EQ(QStringLiteral("a b c"), cleanName(QStringLiteral("a\tb\n  c")));
    EXPECT_EQ(QStringLiteral("hidden"), cleanName(QStringLiteral("..hidden. .")));
    EXPECT_EQ(QStringLiteral("Mr. Generated"), cleanName(QStringLiteral("Mr. Generated")));
    EXPECT_EQ(QString(), cleanName(QStringLiteral("...")));
    EXPECT_EQ(QString(), cleanName(QStringLiteral("<>|")));
    EXPECT_EQ(70, cleanName(QString(100, QLatin1Char('x'))).size());
    EXPECT_EQ(QStringLiteral("1 bar"), lengthText(4));
    EXPECT_EQ(QStringLiteral("2 bars"), lengthText(8));
    EXPECT_EQ(QStringLiteral("32 bars"), lengthText(128));
    EXPECT_EQ(QStringLiteral("1 beat"), lengthText(1));
    EXPECT_EQ(QStringLiteral("2 beats"), lengthText(2));
    EXPECT_EQ(QStringLiteral("6 beats"), lengthText(6));
    EXPECT_EQ(QStringLiteral("0.5 beats"), lengthText(0.5));
    EXPECT_EQ(QStringLiteral("0.25 beats"), lengthText(0.25));
    EXPECT_EQ(QString(), lengthText(0));
    EXPECT_EQ(QStringLiteral("Signal Fire - Loop 2 bars"), sampleName(QStringLiteral("Signal Fire"), 8, 0));
    EXPECT_EQ(QStringLiteral("Untitled - Loop 4.2 s"), sampleName(QString(), 0, 4.24));
    const QString longName = sampleName(QString(100, QLatin1Char('W')), 8, 0);
    EXPECT_LE(longName.size(), 70);
    EXPECT_TRUE(longName.endsWith(QStringLiteral("W - Loop 2 bars"))) << "a long title gives way, never the length";
    // " 2", " 3" ... after the names taken.
    ASSERT_TRUE(QDir().mkpath(folder));
    for (const char* name : {"Generated Tone - Loop 1 bar.wav", "Generated Tone - Loop 1 bar 2.wav"}) {
        QFile file(QDir(folder).filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    EXPECT_EQ(QDir(folder).filePath(QStringLiteral("Generated Tone - Loop 1 bar 3.wav")),
            decklab::capture::uniquePath(folder, QStringLiteral("Generated Tone - Loop 1 bar")));
    const auto third = decklab::capture::write(request(0, framesFor(4, 120), 4));
    ASSERT_EQ(Status::Saved, third.status);
    EXPECT_EQ(QDir(folder).filePath(QStringLiteral("Generated Tone - Loop 1 bar 3.wav")), third.path);
}

TEST_F(DeckLabCaptureTest, AFullDiskAndAFolderThatCannotBeWrittenAreClearErrors) {
    auto full = request(0, framesFor(4, 120), 4);
    full.reserveBytes = qint64(1) << 60; // more than any disk has free
    EXPECT_EQ(Status::NoSpace, decklab::capture::write(full).status);
    EXPECT_EQ(QStringLiteral("Not saved: the disk is full"), decklab::capture::errorText(Status::NoSpace));
    EXPECT_TRUE(saved().isEmpty()) << "nothing half-written is left";
    // A folder that cannot be made (a file is in the way).
    QFile blocker(dir.filePath(QStringLiteral("blocked")));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    auto blocked = request(0, framesFor(4, 120), 4);
    blocked.folder = dir.filePath(QStringLiteral("blocked/Deck Lab Samples"));
    EXPECT_EQ(Status::NotWritable, decklab::capture::write(blocked).status);
    EXPECT_TRUE(decklab::capture::errorText(Status::NotWritable).contains(QStringLiteral("Deck Lab Samples")));
    // A file Mixxx cannot read.
    auto missing = request(0, framesFor(4, 120), 4);
    missing.source = dir.filePath(QStringLiteral("missing.wav"));
    EXPECT_EQ(Status::Unreadable, decklab::capture::write(missing).status);
    // A cancelled write leaves nothing.
    std::atomic<bool> cancel(true);
    EXPECT_EQ(Status::Cancelled, decklab::capture::write(request(0, framesFor(4, 120), 4), &cancel).status);
    EXPECT_TRUE(saved().isEmpty());
}

TEST_F(DeckLabCaptureTest, TheLoopGoesOnTheFirstEmptyPadOffTheGuiThread) {
    FakeDecks decks;
    decks.track = Track::newTemporary(source);
    decks.track->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
            mixxx::audio::SampleRate(kRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(6));
    ASSERT_TRUE(decks.track->trySetBeats(grid(120)));
    decks.start = samples(kRate / 2);
    decks.end = samples(kRate / 2 + framesFor(8, 120));
    decks.pads[0].loaded = true;
    decks.pads[1].loaded = true;
    DeckLabLoopCapture capture(decks.host());
    int doneSampler = -1;
    decklab::capture::Result done;
    QObject::connect(&capture, &DeckLabLoopCapture::finished, &capture,
            [&](int deck, int sampler, const decklab::capture::Result& result) {
                EXPECT_EQ(1, deck);
                doneSampler = sampler;
                done = result;
            });
    EXPECT_EQ(Refusal::None, capture.capture(1));
    EXPECT_TRUE(capture.busy()) << "the write runs on its own thread";
    EXPECT_EQ(Refusal::Busy, capture.capture(1)) << "one capture at a time";
    EXPECT_EQ(QStringLiteral("Still saving the last loop"), decks.told.value(0));
    ASSERT_TRUE(QTest::qWaitFor([&] { return doneSampler >= 0; }, 10000));
    EXPECT_FALSE(capture.busy());
    EXPECT_EQ(3, doneSampler) << "the first empty pad";
    ASSERT_EQ(1, decks.loads.size());
    EXPECT_EQ(3, decks.loads[0].first);
    EXPECT_EQ(framesFor(8, 120), decks.loads[0].second.frames);
    EXPECT_NEAR(120.0, decks.loads[0].second.bpm, 1e-9) << "the pad gets the track's tempo as its grid";
    EXPECT_EQ(QDir(folder).filePath(QStringLiteral("Generated Tone - Loop 2 bars.wav")), done.path);
    EXPECT_TRUE(QFile::exists(done.path));
    EXPECT_EQ(QStringLiteral("Saved to pad 3"), decks.told.last());
    EXPECT_FALSE(decks.errors.last());
    // No loop, or one too long: refused on the screen, nothing written.
    decks.looping = false;
    EXPECT_EQ(Refusal::NoLoop, capture.capture(1));
    decks.looping = true;
    decks.end = decks.start + samples(framesFor(132, 120));
    EXPECT_EQ(Refusal::TooLong, capture.capture(1));
    EXPECT_FALSE(capture.busy());
    EXPECT_TRUE(decks.told.last().contains(QStringLiteral("32 bars")));
    EXPECT_TRUE(decks.errors.last());
    EXPECT_EQ(Refusal::NoTrack, capture.capture(2));
    EXPECT_EQ(1, saved().size());
}

// r7: the pad comes from the one empty-pad rule PUT ON PAD uses too
// (DeckLabSystem::emptyPad): a pad that is loading, or that a Load pad trip
// is filling, is not empty even while nothing is loaded on it.
TEST_F(DeckLabCaptureTest, TheLoopGoesOnThePadTheSharedEmptyPadRuleNames) {
    FakeDecks decks;
    decks.track = Track::newTemporary(source);
    decks.track->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
            mixxx::audio::SampleRate(kRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(6));
    ASSERT_TRUE(decks.track->trySetBeats(grid(120)));
    decks.start = 0;
    decks.end = samples(framesFor(4, 120));
    DeckLabLoopCapture::Host host = decks.host();
    int empty = 5; // pads 1 to 4 show nothing loaded, but are loading or a trip's target
    host.emptyPad = [&empty] { return empty; };
    DeckLabLoopCapture capture(host);
    int doneSampler = -1;
    QObject::connect(&capture, &DeckLabLoopCapture::finished, &capture, [&](int, int sampler, const decklab::capture::Result&) {
        doneSampler = sampler;
    });
    ASSERT_EQ(Refusal::None, capture.capture(1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return doneSampler >= 0; }, 10000));
    EXPECT_EQ(5, doneSampler);
    ASSERT_EQ(1, decks.loads.size());
    EXPECT_EQ(5, decks.loads[0].first);
    // No pad free by that rule: it asks (no sheet here: saved, on no pad).
    empty = 0;
    doneSampler = -1;
    ASSERT_EQ(Refusal::None, capture.capture(1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return doneSampler >= 0; }, 10000));
    EXPECT_EQ(0, doneSampler);
    EXPECT_EQ(1, decks.loads.size()) << "pad 1 shows nothing loaded, yet the rule keeps it";
}

// r7: TO PAD never writes the library database while the deck plays. The
// pad gets a temporary track with the loop's grid at once; the sample joins
// the library, with that grid, once it is quiet.
TEST_F(DeckLabCaptureTest, CapturedSamplesJoinTheLibraryOnlyOnceItIsQuiet) {
    decklab::capture::Result result;
    result.status = decklab::capture::Result::Status::Saved;
    result.path = QDir(folder).filePath(QStringLiteral("Generated Tone - Loop 1 bar.wav"));
    result.frames = framesFor(4, 120);
    result.sampleRate = kRate;
    result.bpm = 120;
    const TrackPointer pad = decklab::capture::padTrack(result);
    ASSERT_TRUE(pad);
    EXPECT_FALSE(pad->getId().isValid()) << "not a library track";
    EXPECT_EQ(result.path, pad->getLocation());
    EXPECT_EQ(QStringLiteral("Generated Tone - Loop 1 bar"), pad->getTitle());
    EXPECT_TRUE(pad->isBpmLocked());
    EXPECT_NEAR(120.0, pad->getBpm(), 0.01);
    EXPECT_NEAR(2.0, pad->getDuration(), 0.01);

    bool quiet = false;
    QStringList added;
    TrackPointer row;
    DeckLabCaptureLibrary::Hooks hooks;
    hooks.quiet = [&quiet] { return quiet; };
    hooks.add = [&](const QString& path) {
        added.append(path);
        // The library reads the file's header as it adds it.
        row = Track::newTemporary(path);
        row->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(kRate),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(2));
        return row;
    };
    DeckLabCaptureLibrary library(hooks);
    library.add(result);
    EXPECT_TRUE(added.isEmpty()) << "nothing touches the library while a deck plays";
    EXPECT_EQ(1, library.pending());
    library.flush();
    EXPECT_TRUE(added.isEmpty());
    quiet = true;
    ASSERT_TRUE(QTest::qWaitFor([&] { return !added.isEmpty(); }, DeckLabCaptureLibrary::kRetryMs + 2000));
    EXPECT_EQ(QStringList{result.path}, added);
    EXPECT_EQ(0, library.pending());
    ASSERT_TRUE(row);
    EXPECT_TRUE(row->isBpmLocked()) << "the library row keeps the loop's grid";
    EXPECT_NEAR(120.0, row->getBpm(), 0.01);
    // A quiet capture joins at once; a track whose tempo is locked keeps it.
    library.add(result);
    EXPECT_EQ(2, added.size());
    decklab::capture::Result other = result;
    other.bpm = 90;
    EXPECT_FALSE(decklab::capture::applyGrid(row, other));
    EXPECT_NEAR(120.0, row->getBpm(), 0.01);

    // r7 fix: the pad's SAMPLES list asks for what waits (flush) while a
    // deck plays: nothing is added mid-set; the list says how many wait.
    quiet = false;
    DeckLabCaptureLibrary listed(hooks);
    listed.add(result);
    listed.flush();
    EXPECT_EQ(2, added.size()) << "no library write mid-set";
    EXPECT_EQ(1, listed.pending());
    quiet = true;
    listed.flush();
    EXPECT_EQ(3, added.size());
    EXPECT_EQ(0, listed.pending());
}

TEST_F(DeckLabCaptureTest, WithNoEmptyPadASheetAsksWhichPadToReplace) {
    QWidget surface;
    surface.resize(800, 480);
    surface.setStyleSheet(QStringLiteral("background:#0D0F12;"));
    surface.show();
    FakeDecks decks;
    decks.parent = &surface;
    decks.track = Track::newTemporary(source);
    decks.track->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
            mixxx::audio::SampleRate(kRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(6));
    decks.track->setTitle(QStringLiteral("Generated Tone"));
    ASSERT_TRUE(decks.track->trySetBeats(grid(120)));
    decks.start = 0;
    decks.end = samples(framesFor(4, 120));
    for (int index = 0; index < 16; ++index) {
        decks.pads[static_cast<size_t>(index)] = {true, false, QStringLiteral("Generated Pad %1").arg(index + 1)};
    }
    decks.pads[4].playing = true;
    DeckLabLoopCapture capture(decks.host());
    int doneSampler = -1;
    QObject::connect(&capture, &DeckLabLoopCapture::finished, &capture, [&](int, int sampler, const decklab::capture::Result&) {
        doneSampler = sampler;
    });
    ASSERT_EQ(Refusal::None, capture.capture(1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return capture.chooser() && capture.chooser()->isVisible(); }, 10000));
    EXPECT_EQ(-1, doneSampler) << "it waits for the DJ";
    EXPECT_TRUE(decks.loads.isEmpty());
    auto* chooser = capture.chooser();
    EXPECT_TRUE(surface.rect().contains(chooser->geometry()));
    ASSERT_NE(nullptr, chooser->padButton(5));
    EXPECT_FALSE(chooser->padButton(5)->isEnabled()) << "a playing pad is never replaced";
    for (int sampler = 1; sampler <= 16; ++sampler) {
        auto* b = chooser->padButton(sampler);
        ASSERT_NE(nullptr, b) << sampler;
        EXPECT_GE(b->height(), 44) << sampler;
        EXPECT_GE(b->width(), 44) << sampler;
    }
    EXPECT_GE(chooser->cancelButton()->height(), 44);
    screenshot(&surface, QStringLiteral("capture-choose-pad.png"));
    // Replacing acts on release.
    QTest::mousePress(chooser->padButton(7), Qt::LeftButton);
    EXPECT_TRUE(decks.loads.isEmpty());
    QTest::mouseRelease(chooser->padButton(7), Qt::LeftButton);
    EXPECT_EQ(7, doneSampler);
    ASSERT_EQ(1, decks.loads.size());
    EXPECT_EQ(7, decks.loads[0].first);
    EXPECT_FALSE(chooser->isVisible());
    EXPECT_EQ(QStringLiteral("Saved to pad 7"), decks.told.last());
    // CANCEL: the sample stays in the folder, on no pad.
    doneSampler = -1;
    ASSERT_EQ(Refusal::None, capture.capture(1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return chooser->isVisible(); }, 10000));
    QTest::mouseClick(chooser->cancelButton(), Qt::LeftButton);
    EXPECT_EQ(0, doneSampler);
    EXPECT_EQ(1, decks.loads.size());
    EXPECT_EQ(QStringLiteral("Saved in Deck Lab Samples, not on a pad"), decks.told.last());
    EXPECT_EQ(2, saved().size());
    // r7 fix: the browse knob drives the sheet (it never leaves for Browse):
    // the first turn shows the cursor on CANCEL, turns move it over the pads
    // that can be replaced, a press picks.
    doneSampler = -1;
    ASSERT_EQ(Refusal::None, capture.capture(1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return chooser->isVisible(); }, 10000));
    EXPECT_FALSE(decklab::knobOwner().browse()) << "the sheet owns the knob";
    EXPECT_EQ(chooser, decklab::knobOwner().blocker);
    EXPECT_EQ(nullptr, chooser->cursorButton());
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Move, 1);
    EXPECT_EQ(chooser->cancelButton(), chooser->cursorButton());
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Move, 1);
    EXPECT_EQ(chooser->padButton(1), chooser->cursorButton());
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Move, 4);
    EXPECT_EQ(chooser->padButton(6), chooser->cursorButton()) << "pad 5 plays: skipped";
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Move, -2);
    EXPECT_EQ(chooser->padButton(3), chooser->cursorButton());
    screenshot(&surface, QStringLiteral("capture-choose-pad-knob.png"));
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Press, 0);
    EXPECT_EQ(3, doneSampler);
    EXPECT_FALSE(chooser->isVisible());
    EXPECT_TRUE(decklab::knobOwner().browse()) << "Browse has the knob back";
    // Back on the knob cancels.
    doneSampler = -1;
    ASSERT_EQ(Refusal::None, capture.capture(1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return chooser->isVisible(); }, 10000));
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Back, 0);
    EXPECT_EQ(0, doneSampler);
    EXPECT_FALSE(chooser->isVisible());
    // A page change (the sheet hides with its page) is a cancel too.
    doneSampler = -1;
    ASSERT_EQ(Refusal::None, capture.capture(1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return chooser->isVisible(); }, 10000));
    surface.hide();
    EXPECT_EQ(0, doneSampler);
    EXPECT_EQ(2, decks.loads.size());
    EXPECT_TRUE(decklab::knobOwner().browse());
}

} // namespace
