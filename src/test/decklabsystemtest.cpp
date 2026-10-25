// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab system integration: decklabd client protocol, controller mapping
// matching, Deck Setup policy and persistence, eject safety, System pages and
// the first-run flow. Only generated fixtures and bundled mappings are used:
// no hardware, host services, personal media or network.
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPushButton>
#include <QElapsedTimer>
#include <QSlider>
#include <QComboBox>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <csignal>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif
#include <memory>

#include "controllers/controllermappinginfo.h"
#include "controllers/controllermappinginfoenumerator.h"
#include "controllers/controllermappingmatcher.h"
#include "decklab/decklabdclient.h"
#include "decklab/decklabfakeservice.h"
#include "decklab/decklabinstruments.h"
#include "decklab/decklabsetupmodel.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsignals.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabrail.h"
#include "decklab/decklabsystempages.h"
#include "decklab/decklabsystemui.h"
#include "decklab/decklabusbname.h"
#include "decklab/decklabui.h"
#include "decklab/decklabui.h"
#include "test/mixxxtest.h"
#include "widget/decklabdeckwidgets.h"

namespace {

QString mappingPath(const UserSettingsPointer& config, const QString& fileName) {
    return QDir(config->getResourcePath()).filePath(QStringLiteral("controllers/") + fileName);
}

int scoreFor(const UserSettingsPointer& config, const QString& device, const QString& fileName) {
    ControllerMappingMatcher::Device input;
    input.name = device;
    return ControllerMappingMatcher::score(input, MappingInfo(mappingPath(config, fileName)));
}

QString bestMidiMapping(const UserSettingsPointer& config, const QString& device) {
    static QList<MappingInfo> mappings;
    if (mappings.isEmpty()) {
        MappingInfoEnumerator enumerator(QDir(config->getResourcePath()).filePath(QStringLiteral("controllers")));
        mappings = enumerator.getMappingsByExtension(QStringLiteral(".midi.xml"));
    }
    ControllerMappingMatcher::Device input;
    input.name = device;
    const auto ranked = ControllerMappingMatcher::rank(input, mappings);
    return ranked.isEmpty() ? QString() : QFileInfo(ranked.first().mapping.getPath()).fileName();
}

decklab::AudioDevice audioDevice(const QString& id, int outputs, decklab::AudioDevice::Kind kind, bool controller = false) {
    decklab::AudioDevice device;
    device.id = id;
    device.name = id;
    device.outputs = outputs;
    device.kind = kind;
    device.controllerCard = controller;
    return device;
}

QPushButton* visibleButton(QWidget* root, const QString& name, int index = 0) {
    int seen = 0;
    for (auto* button : root->findChildren<QPushButton*>(name)) {
        if (button->isVisibleTo(root) && seen++ == index) {
            return button;
        }
    }
    return nullptr;
}

int visibleCount(QWidget* root, const QString& name) {
    int count = 0;
    for (auto* button : root->findChildren<QPushButton*>(name)) {
        count += button->isVisibleTo(root);
    }
    return count;
}

// The Deck Lab sheet currently shown over `host`, if any.
decklab::ui::Confirm* openSheet(QWidget* host) {
    for (auto* sheet : host->findChildren<decklab::ui::Confirm*>()) {
        if (sheet->isVisibleTo(host)) {
            return sheet;
        }
    }
    return nullptr;
}

QString sheetText(QWidget* sheet) {
    QStringList texts;
    for (auto* label : sheet->findChildren<QLabel*>()) {
        texts.append(label->text());
    }
    return texts.join(QLatin1Char('\n'));
}

} // namespace

class DeckLabSystemTest : public MixxxTest {
  protected:
    QString socketPath() {
        return m_dir.filePath(QStringLiteral("decklabd.sock"));
    }
    QTemporaryDir m_dir;
};

// --------------------------------------------------------- decklabd client --

TEST_F(DeckLabSystemTest, ClientHandshakesSubscribesAndParsesSplitAndCoalescedFrames) {
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabdClient client(nullptr, socketPath());
    QSignalSpy ready(&client, &DeckLabdClient::readyChanged);
    client.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return client.isReady(); }, 3000));
    ASSERT_GE(fake.calls.size(), 2);
    EXPECT_EQ(QStringLiteral("hello"), fake.calls.at(0));
    EXPECT_EQ(QStringLiteral("events.subscribe"), fake.calls.at(1));
    EXPECT_EQ(1, fake.requests.at(0).value("params").toObject().value("protocol").toInt());
    EXPECT_EQ(QStringLiteral("mixxx"), fake.requests.at(0).value("params").toObject().value("client").toString());
    EXPECT_TRUE(client.hasFeature(QStringLiteral("usb")));
    EXPECT_EQ(QStringLiteral("fake-1"), client.serverVersion());

    QStringList topics;
    QObject::connect(&client, &DeckLabdClient::eventReceived, [&topics](const QString& topic, const QJsonObject& data) {
        topics.append(topic + QLatin1Char('=') + data.value("n").toString());
    });
    // Two events in one write, then one split across three writes.
    fake.sendRaw(QByteArray("{\"event\":\"usb.changed\",\"data\":{\"n\":\"1\"}}\n"
                            "{\"event\":\"thermal.changed\",\"data\":{\"n\":\"2\"}}\n"));
    fake.sendRaw(QByteArray("{\"event\":\"wifi.cha"));
    QTest::qWait(30);
    fake.sendRaw(QByteArray("nged\",\"data\":{\"n\""));
    QTest::qWait(30);
    fake.sendRaw(QByteArray(":\"3\"}}\n"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return topics.size() == 3; }, 2000));
    EXPECT_EQ(QStringList({QStringLiteral("usb.changed=1"), QStringLiteral("thermal.changed=2"),
                      QStringLiteral("wifi.changed=3")}),
            topics);

    // Replies reach the right callback with result or error.
    DeckLabdReply info;
    DeckLabdReply refused;
    bool gotInfo = false;
    bool gotRefused = false;
    fake.failures.insert(QStringLiteral("display.set"), QStringLiteral("denied"));
    client.call(QStringLiteral("system.info"), {}, [&](const DeckLabdReply& reply) {
        info = reply;
        gotInfo = true;
    });
    client.call(QStringLiteral("display.set"), QJsonObject{{"brightness", 50}}, [&](const DeckLabdReply& reply) {
        refused = reply;
        gotRefused = true;
    });
    ASSERT_TRUE(QTest::qWaitFor([&] { return gotInfo && gotRefused; }, 2000));
    EXPECT_TRUE(info.ok);
    EXPECT_EQ(QStringLiteral("decklab"), info.result.value("hostname").toString());
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(QStringLiteral("denied"), refused.errorCode);
}

TEST_F(DeckLabSystemTest, ClientDropsOverlongLinesAndReconnectsWithBackoff) {
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabdClient client(nullptr, socketPath());
    client.setReconnectBackoff(20, 80);
    client.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return client.isReady(); }, 3000));
    QSignalSpy ready(&client, &DeckLabdClient::readyChanged);

    // A line beyond the 64 KiB contract limit is a protocol violation.
    fake.sendRaw(QByteArray(DeckLabdClient::kMaxLineBytes + 10, 'x'));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !client.isReady(); }, 2000));
    ASSERT_TRUE(QTest::qWaitFor([&] { return client.isReady(); }, 3000));
    EXPECT_GE(fake.calls.count(QStringLiteral("hello")), 2);

    // A server-side disconnect fails pending work and reconnects.
    fake.respond = false;
    DeckLabdReply pending;
    bool done = false;
    client.call(QStringLiteral("wifi.status"), {}, [&](const DeckLabdReply& reply) {
        pending = reply;
        done = true;
    });
    ASSERT_TRUE(QTest::qWaitFor([&] { return fake.calls.last() == QStringLiteral("wifi.status"); }, 2000));
    fake.respond = true;
    fake.disconnectClients();
    ASSERT_TRUE(QTest::qWaitFor([&] { return done; }, 2000));
    EXPECT_FALSE(pending.ok);
    EXPECT_EQ(QStringLiteral("not_available"), pending.errorCode);
    ASSERT_TRUE(QTest::qWaitFor([&] { return client.isReady(); }, 3000));
    EXPECT_GE(fake.calls.count(QStringLiteral("hello")), 3);
    EXPECT_GE(ready.count(), 4);
}

TEST_F(DeckLabSystemTest, ClientTimesOutUnansweredRequestsAndFailsFastWhenOffline) {
    // Offline: no socket at all. The call fails asynchronously, never inline.
    DeckLabdClient offline(nullptr, m_dir.filePath(QStringLiteral("missing.sock")));
    offline.setReconnectBackoff(20, 40);
    offline.start();
    bool inline_ = true;
    bool offlineDone = false;
    DeckLabdReply offlineReply;
    offline.call(QStringLiteral("usb.list"), {}, [&](const DeckLabdReply& reply) {
        offlineReply = reply;
        offlineDone = true;
    });
    inline_ = offlineDone;
    EXPECT_FALSE(inline_);
    ASSERT_TRUE(QTest::qWaitFor([&] { return offlineDone; }, 1000));
    EXPECT_EQ(QStringLiteral("not_available"), offlineReply.errorCode);
    EXPECT_FALSE(offline.isReady());

    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabdClient client(nullptr, socketPath());
    client.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return client.isReady(); }, 3000));
    fake.respond = false;
    QElapsedTimer clock;
    clock.start();
    DeckLabdReply reply;
    bool done = false;
    client.call(QStringLiteral("time.status"), {}, [&](const DeckLabdReply& answer) {
        reply = answer;
        done = true;
    }, 250);
    ASSERT_TRUE(QTest::qWaitFor([&] { return done; }, 2000));
    EXPECT_FALSE(reply.ok);
    EXPECT_EQ(QStringLiteral("timeout"), reply.errorCode);
    EXPECT_GE(clock.elapsed(), 200);
    EXPECT_TRUE(client.isReady()); // a slow method does not drop the session
}

TEST_F(DeckLabSystemTest, SocketPathComesFromEnvironmentThenRuntimeDirectory) {
    const QByteArray oldSocket = qgetenv("DECKLAB_SOCKET");
    const QByteArray oldRuntime = qgetenv("XDG_RUNTIME_DIR");
    qputenv("DECKLAB_SOCKET", "/tmp/custom/decklabd.sock");
    EXPECT_EQ(QStringLiteral("/tmp/custom/decklabd.sock"), DeckLabdClient::defaultSocketPath());
    qunsetenv("DECKLAB_SOCKET");
    qputenv("XDG_RUNTIME_DIR", "/run/user/4242");
    EXPECT_EQ(QStringLiteral("/run/user/4242/decklab/decklabd.sock"), DeckLabdClient::defaultSocketPath());
    if (oldSocket.isNull()) {
        qunsetenv("DECKLAB_SOCKET");
    } else {
        qputenv("DECKLAB_SOCKET", oldSocket);
    }
    if (oldRuntime.isNull()) {
        qunsetenv("XDG_RUNTIME_DIR");
    } else {
        qputenv("XDG_RUNTIME_DIR", oldRuntime);
    }
}

// ------------------------------------------------------- mapping matching --

TEST_F(DeckLabSystemTest, MidiPortNamesMatchTheirBundledMappings) {
    const auto config = this->config();
    EXPECT_EQ(QStringLiteral("Pioneer-DDJ-FLX4.midi.xml"), bestMidiMapping(config, QStringLiteral("DDJ-FLX4 MIDI 1")));
    EXPECT_EQ(QStringLiteral("Numark-Mixtrack-3.midi.xml"), bestMidiMapping(config, QStringLiteral("MixTrack Pro 3 MIDI 1")));
    EXPECT_EQ(QStringLiteral("Hercules_DJControl_Inpulse_500.midi.xml"),
            bestMidiMapping(config, QStringLiteral("DJControl Inpulse 500 MIDI 1")));
    EXPECT_EQ(QStringLiteral("Denon-MC7000.midi.xml"), bestMidiMapping(config, QStringLiteral("DENON DJ MC7000 MIDI 1")));
    EXPECT_EQ(QStringLiteral("Pioneer-DDJ-SB2.midi.xml"), bestMidiMapping(config, QStringLiteral("DDJ-SB2 MIDI 1")));
    // ALSA "client:port address" decoration is ignored.
    EXPECT_EQ(QStringLiteral("Pioneer-DDJ-SB2.midi.xml"),
            bestMidiMapping(config, QStringLiteral("DDJ-SB2:DDJ-SB2 MIDI 1 20:0")));
    // Exact name matches count as matchMapping() hits; neighbours never do.
    EXPECT_GE(scoreFor(config, QStringLiteral("DDJ-FLX4 MIDI 1"), QStringLiteral("Pioneer-DDJ-FLX4.midi.xml")),
            ControllerMappingMatcher::kExactScore);
    EXPECT_GE(scoreFor(config, QStringLiteral("MixTrack Pro 3 MIDI 1"), QStringLiteral("Numark-Mixtrack-3.midi.xml")),
            ControllerMappingMatcher::kExactScore);
    EXPECT_EQ(0, scoreFor(config, QStringLiteral("DJControl Inpulse 500 MIDI 1"),
                         QStringLiteral("Hercules_DJControl_Inpulse_200.midi.xml")));
    EXPECT_EQ(0, scoreFor(config, QStringLiteral("DDJ-SB2 MIDI 1"), QStringLiteral("Pioneer-DDJ-SB3.midi.xml")));
    EXPECT_LT(scoreFor(config, QStringLiteral("DDJ-SB2 MIDI 1"), QStringLiteral("Pioneer-DDJ-SB.midi.xml")),
            ControllerMappingMatcher::kSuggestScore);
    EXPECT_LT(scoreFor(config, QStringLiteral("MixTrack Pro 3 MIDI 1"), QStringLiteral("Numark Mixtrack Pro.midi.xml")),
            ControllerMappingMatcher::kSuggestScore);
    EXPECT_LT(scoreFor(config, QStringLiteral("MixTrack Pro 3 MIDI 1"), QStringLiteral("Numark Mixtrack Pro FX.midi.xml")),
            ControllerMappingMatcher::kSuggestScore);
    // The MIDI loopback is not a controller.
    EXPECT_TRUE(bestMidiMapping(config, QStringLiteral("Midi Through Port-0")).isEmpty());
    EXPECT_EQ(QStringLiteral("DDJ-FLX4"), ControllerMappingMatcher::modelName(QStringLiteral("DDJ-FLX4 MIDI 1")));
}

TEST_F(DeckLabSystemTest, HidControllersMatchByProductIdsAndUsbOverridesApply) {
    const auto config = this->config();
    ControllerMappingMatcher::Device traktor;
    traktor.name = QStringLiteral("Traktor Kontrol S2 MK3");
    traktor.vendorId = 0x17cc;
    traktor.productId = 0x1710;
    traktor.interfaceNumber = 3;
    traktor.hid = true;
    EXPECT_EQ(100, ControllerMappingMatcher::score(traktor, MappingInfo(mappingPath(config, QStringLiteral("Traktor Kontrol S2 MK3.hid.xml")))));
    EXPECT_LT(ControllerMappingMatcher::score(traktor, MappingInfo(mappingPath(config, QStringLiteral("Traktor Kontrol S2 Mk2.hid.xml")))),
            ControllerMappingMatcher::kSuggestScore);
    EXPECT_LT(ControllerMappingMatcher::score(traktor, MappingInfo(mappingPath(config, QStringLiteral("Traktor Kontrol S4 MK3.hid.xml")))),
            ControllerMappingMatcher::kSuggestScore);
    // A host-reported USB id selects a mapping even with an unhelpful name.
    ControllerMappingMatcher::Device usb;
    usb.name = QStringLiteral("USB MIDI Interface");
    usb.vendorId = 0x15e4;
    usb.productId = 0x0035;
    EXPECT_EQ(110, ControllerMappingMatcher::score(usb, MappingInfo(mappingPath(config, QStringLiteral("Numark-Mixtrack-3.midi.xml")))));
    // User override files take precedence and are merged with the builtins.
    const auto overrides = ControllerMappingMatcher::parseOverrides(QByteArray(
            "{\"overrides\":[{\"name\":\"^Studio Box\",\"mapping\":\"Pioneer-DDJ-400.midi.xml\"}]}"));
    ControllerMappingMatcher::Device custom;
    custom.name = QStringLiteral("Studio Box MIDI 1");
    EXPECT_EQ(110, ControllerMappingMatcher::score(custom, MappingInfo(mappingPath(config, QStringLiteral("Pioneer-DDJ-400.midi.xml"))), overrides));
    // Deck count suggestion follows [Channel3]/[Channel4] usage.
    EXPECT_EQ(2, ControllerMappingMatcher::suggestedDeckCount(MappingInfo(mappingPath(config, QStringLiteral("Pioneer-DDJ-FLX4.midi.xml"))),
                         QDir(config->getResourcePath()).filePath(QStringLiteral("controllers"))));
    EXPECT_EQ(4, ControllerMappingMatcher::suggestedDeckCount(MappingInfo(mappingPath(config, QStringLiteral("Denon-MC7000.midi.xml"))),
                         QDir(config->getResourcePath()).filePath(QStringLiteral("controllers"))));
}

// ---------------------------------------------------------- audio policy --

TEST_F(DeckLabSystemTest, AudioPolicyRoutesFourTwoAndNoControllerOutputs) {
    using Kind = decklab::AudioDevice::Kind;
    const auto jack = audioDevice(QStringLiteral("jack"), 2, Kind::BuiltinJack);
    const auto hdmi = audioDevice(QStringLiteral("hdmi"), 2, Kind::Hdmi);
    const auto bluetooth = audioDevice(QStringLiteral("pipewire"), 2, Kind::Network);

    // 4+ outputs on the controller: Main 1-2, Headphones 3-4.
    auto plan = decklab::planAudio({jack, hdmi, audioDevice(QStringLiteral("controller"), 4, Kind::External, true)});
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("controller"), 0}), plan.main);
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("controller"), 2}), plan.headphones);

    // 2 outputs: Main on the controller; headphones only when chosen.
    const auto twoOut = audioDevice(QStringLiteral("controller"), 2, Kind::External, true);
    plan = decklab::planAudio({jack, twoOut});
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("controller"), 0}), plan.main);
    EXPECT_FALSE(plan.headphones.isValid());
    plan = decklab::planAudio({jack, twoOut}, QStringLiteral("jack"));
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("jack"), 0}), plan.headphones);
    // The controller's own outputs can't double as headphones.
    plan = decklab::planAudio({jack, twoOut}, QStringLiteral("controller"));
    EXPECT_FALSE(plan.headphones.isValid());

    // No controller audio: a USB DAC, else the Pi jack, else HDMI.
    plan = decklab::planAudio({jack, hdmi, audioDevice(QStringLiteral("dac"), 2, Kind::External)});
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("dac"), 0}), plan.main);
    plan = decklab::planAudio({hdmi, jack, bluetooth});
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("jack"), 0}), plan.main);
    plan = decklab::planAudio({hdmi, bluetooth});
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("hdmi"), 0}), plan.main);
    // Bluetooth/PipeWire is high latency: never chosen automatically.
    plan = decklab::planAudio({bluetooth});
    EXPECT_FALSE(plan.main.isValid());
    EXPECT_TRUE(decklab::isHighLatency(bluetooth));
    plan = decklab::planAudio({});
    EXPECT_FALSE(plan.main.isValid());
    EXPECT_FALSE(plan.reason.isEmpty());

    // Device names from PortAudio/ALSA classify as expected.
    EXPECT_EQ(Kind::BuiltinJack, decklab::classifyAudioDevice(QStringLiteral("bcm2835 Headphones: -"), QStringLiteral("hw:0,0"), false));
    EXPECT_EQ(Kind::Hdmi, decklab::classifyAudioDevice(QStringLiteral("vc4-hdmi-0: MAI PCM i2s-hifi-0"), QStringLiteral("hw:2,0"), false));
    EXPECT_EQ(Kind::External, decklab::classifyAudioDevice(QStringLiteral("Controller: USB Audio"), QStringLiteral("hw:1,0"), true));
    EXPECT_EQ(Kind::Network, decklab::classifyAudioDevice(QStringLiteral("pipewire"), QString(), false));
    EXPECT_EQ(QStringLiteral("HDMI 1"), decklab::prettyAudioName(QStringLiteral("vc4-hdmi-0: MAI PCM i2s-hifi-0")));
    EXPECT_EQ(QStringLiteral("3–4"), decklab::channelPairLabel(2));
}

// ------------------------------------------------------------ eject safety --

TEST_F(DeckLabSystemTest, EjectIsRefusedWhileALoadedTrackLivesOnThatDrive) {
    const QString mount = QStringLiteral("/media/decklab/0000-TEST");
    const QList<decklab::LoadedTrack> loaded{
            {QStringLiteral("[Channel1]"), mount + QStringLiteral("/Music/generated-tone-1.wav")},
            {QStringLiteral("[Sampler3]"), mount + QStringLiteral("/Samples/generated-hit.wav")},
            {QStringLiteral("[Channel2]"), QStringLiteral("/media/decklab/0000-TEST2/generated-tone-2.wav")}};
    EXPECT_EQ(QStringList({QStringLiteral("[Channel1]"), QStringLiteral("[Sampler3]")}),
            decklab::ejectBlockers(mount, loaded));
    // A sibling mount with a common prefix is a different drive.
    EXPECT_FALSE(decklab::isPathOnMount(QStringLiteral("/media/decklab/0000-TEST2/a.wav"), mount));
    EXPECT_TRUE(decklab::ejectBlockers(QStringLiteral("/media/decklab/1111-OTHER"), loaded).isEmpty());
    const QString refusal = decklab::ejectRefusal(QStringLiteral("TEST"), decklab::ejectBlockers(mount, loaded));
    EXPECT_TRUE(refusal.contains(QStringLiteral("Deck 1")));
    EXPECT_TRUE(refusal.contains(QStringLiteral("Sampler 3")));

    // The system service refuses before asking decklabd to eject.
    DeckLabFakeService fake;
    fake.drives = QJsonArray{QJsonObject{{"id", "0000-TEST"}, {"label", "TEST"}, {"mount", mount},
                                     {"size_mb", 1024}, {"free_mb", 512}, {"fs", "vfat"}, {"read_only", true},
                                     {"rekordbox", false}, {"busy", false}},
            QJsonObject{{"id", "1111-OTHER"}, {"label", "OTHER"}, {"mount", "/media/decklab/1111-OTHER"},
                    {"size_mb", 1024}, {"free_mb", 512}, {"fs", "vfat"}, {"read_only", true}, {"rekordbox", false},
                    {"busy", false}}};
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    system.setLoadedTracksProvider([loaded] { return loaded; });
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady() && system.usbDrives().size() == 2; }, 3000));
    bool refusedDone = false;
    bool refusedOk = true;
    QString refusedText;
    system.ejectDrive(system.usbDrives().at(0).toObject(), [&](bool ok, const QString& text) {
        refusedOk = ok;
        refusedText = text;
        refusedDone = true;
    });
    EXPECT_TRUE(refusedDone);
    EXPECT_FALSE(refusedOk);
    EXPECT_TRUE(refusedText.contains(QStringLiteral("Deck 1")));
    EXPECT_FALSE(fake.calls.contains(QStringLiteral("usb.eject")));
    bool ejected = false;
    system.ejectDrive(system.usbDrives().at(1).toObject(), [&](bool ok, const QString&) { ejected = ok; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return ejected; }, 2000));
    EXPECT_TRUE(fake.calls.contains(QStringLiteral("usb.eject")));
    EXPECT_TRUE(QTest::qWaitFor([&] { return system.usbDrives().size() == 1; }, 2000));
}

// --------------------------------------------- utilities (Get Music etc.) --

// Replaces the old Ctrl+Shift+L injection: when decklabd reports that the
// utility window closed, Mixxx adds the utility's download folder to the
// library if needed and starts one quiet rescan, never while a deck loads.
// While a utility is open it publishes the deck state Sample Maker checks.
TEST_F(DeckLabSystemTest, UtilityCloseAddsDownloadFolderAndRescansAfterDeckLoads) {
    const QString musicRoot = m_dir.filePath(QStringLiteral("Music"));
    const QString downloads = m_dir.filePath(QStringLiteral("media/0000-TEST/Deck Lab Downloads"));
    const QString samples = musicRoot + QStringLiteral("/Deck Lab Samples");
    ASSERT_TRUE(QDir().mkpath(downloads));
    ASSERT_TRUE(QDir().mkpath(samples));
    bool playing = false;
    bool recording = false;
    bool loading = false;
    QStringList roots{musicRoot};
    QStringList added;
    int scans = 0;
    DeckLabUtilities::Hooks hooks;
    hooks.playing = [&] { return playing; };
    hooks.recording = [&] { return recording; };
    hooks.loading = [&] { return loading; };
    hooks.libraryRoots = [&] { return roots; };
    hooks.addLibraryRoot = [&](const QString& folder) {
        added.append(folder);
        roots.append(folder);
        return true;
    };
    hooks.utilityFolders = [&] { return QStringList{downloads, samples}; };
    hooks.startQuietScan = [&] {
        ++scans;
        return true;
    };

    // The real decklabd client and DeckLabSystem, fed by the canned service.
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    system.utilities()->setHooks(hooks);
    system.utilities()->setStateDirectory(m_dir.filePath(QStringLiteral("run/decklab")));
    QStringList notices;
    QObject::connect(&system, &DeckLabSystem::notice, &system, [&](const QString& text) { notices.append(text); });
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));

    // Open: deck state for Sample Maker, refreshed every second, on tmpfs in the kiosk.
    fake.pushEvent(QStringLiteral("apps.changed"), QJsonObject{{QStringLiteral("app"), QStringLiteral("sample-maker")}});
    const QString stateFile = system.utilities()->stateFile();
    ASSERT_TRUE(QTest::qWaitFor([&] { return QFileInfo::exists(stateFile); }, 2000));
    const auto readState = [&] {
        QFile file(stateFile);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    };
    QJsonObject state = readState();
    EXPECT_EQ(1, state.value(QStringLiteral("schema")).toInt());
    EXPECT_FALSE(state.value(QStringLiteral("playing")).toBool(true));
    EXPECT_FALSE(state.value(QStringLiteral("recording")).toBool(true));
    EXPECT_EQ(QStringLiteral("sample-maker"), state.value(QStringLiteral("app")).toString());
    EXPECT_NEAR(QDateTime::currentMSecsSinceEpoch() / 1000.0, state.value(QStringLiteral("updated_at")).toDouble(), 5.0);
    playing = true;
    recording = true;
    ASSERT_TRUE(QTest::qWaitFor([&] { return readState().value(QStringLiteral("playing")).toBool(); }, 3000));
    EXPECT_TRUE(readState().value(QStringLiteral("recording")).toBool());
    playing = false;
    recording = false;
    EXPECT_EQ(0, scans);
    EXPECT_EQ(QStringLiteral("sample-maker"), system.utilities()->openApp());

    // Close while a deck loads a track: the state file goes, the rescan waits.
    loading = true;
    fake.pushEvent(QStringLiteral("apps.changed"), QJsonObject{{QStringLiteral("app"), QJsonValue::Null}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !QFileInfo::exists(stateFile); }, 2000));
    QTest::qWait(DeckLabUtilities::kLoadingRetryMs * 2 + 100);
    EXPECT_EQ(0, scans);
    EXPECT_TRUE(system.utilities()->refreshPending());
    EXPECT_TRUE(notices.isEmpty());
    loading = false;
    ASSERT_TRUE(QTest::qWaitFor([&] { return scans == 1; }, 2000));
    // Only the download folder was new: Deck Lab Samples is already under Music.
    EXPECT_EQ(QStringList{QDir::cleanPath(downloads)}, added);
    EXPECT_EQ(QStringList{QStringLiteral("Looking for new tracks…")}, notices);
    EXPECT_TRUE(system.utilities()->ownsScan());

    // Only our quiet scan reports, once.
    system.utilities()->onScanSummary(5, false);
    EXPECT_EQ(1, notices.size());
    system.utilities()->onScanSummary(3, true);
    EXPECT_EQ(QStringLiteral("3 new tracks are in your library."), notices.last());
    system.utilities()->onScanSummary(3, true);
    EXPECT_EQ(2, notices.size());

    // The next close only rescans; nothing is added twice.
    fake.pushEvent(QStringLiteral("apps.changed"), QJsonObject{{QStringLiteral("app"), QJsonValue::Null}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return scans == 2; }, 2000));
    EXPECT_EQ(1, added.size());
    system.utilities()->onScanSummary(0, true);
    EXPECT_EQ(QStringLiteral("No new tracks found."), notices.last());
    EXPECT_FALSE(system.utilities()->publishNow()) << "nothing is published while no utility is open";

    // A scan that could not start says nothing.
    hooks.startQuietScan = [] { return false; };
    system.utilities()->setHooks(hooks);
    const int before = notices.size();
    system.utilities()->onAppsChanged(QJsonObject{{QStringLiteral("app"), QJsonValue::Null}});
    EXPECT_EQ(before, notices.size());

    // Folder coverage is by path components, not text prefixes.
    EXPECT_EQ(QStringList({QStringLiteral("/media/decklab/A/Deck Lab Downloads"), QStringLiteral("/srv/Musical")}),
            DeckLabUtilities::missingRoots({QStringLiteral("/media/decklab/A/Deck Lab Downloads/"),
                                                   QStringLiteral("/srv/Music/New Music"),
                                                   QStringLiteral("/srv/Musical"),
                                                   QStringLiteral("/srv/Music"),
                                                   QString()},
                    {QStringLiteral("/srv/Music/")}));
}

// ------------------------------------- Sample Maker preview and PUT ON PAD --

namespace {
// [PreviewDeck1] and the sampler pads as DeckLabPreview's hooks see them.
struct FakePreviewDeck {
    bool decksPlaying = false;
    int loaded = 0;
    double duration = 0;
    double position = 0;
    bool playing = false;
    bool loop = false;
    QList<int> loads;
    QList<QList<double>> plays; // start, end, loop, from
    int ejects = 0;
    int emptyPad = 3;
    QStringList pads;

    DeckLabPreview::Hooks hooks(DeckLabSystem& system, const QString& samples) {
        DeckLabPreview::Hooks hooks;
        hooks.decksPlaying = [this] { return decksPlaying; };
        hooks.utilityOpen = [&system] { return !system.utilities()->openApp().isEmpty(); };
        hooks.loadedTrack = [this] { return loaded; };
        hooks.load = [this](int id, QString* error) {
            if (id == 404) {
                *error = QStringLiteral("That track is not in the library.");
                return false;
            }
            loads.append(id);
            return true;
        };
        hooks.duration = [this] { return duration; };
        hooks.position = [this] { return position; };
        hooks.playing = [this] { return playing; };
        hooks.play = [this](double start, double end, bool on, double from) {
            plays.append({start, end, on ? 1.0 : 0.0, from});
            loop = on;
            playing = true;
            if (from >= 0) {
                position = from;
            }
        };
        hooks.pause = [this] {
            playing = false;
            loop = false;
        };
        hooks.eject = [this] {
            ++ejects;
            loaded = 0;
            duration = 0;
        };
        hooks.samplesFolder = [samples] { return samples; };
        hooks.emptyPad = [this] { return emptyPad; };
        hooks.loadPad = [this](int pad, const QString& path, QString*) {
            pads.append(QStringLiteral("%1:%2").arg(pad).arg(path));
            return true;
        };
        return hooks;
    }
};
} // namespace

// Sample Maker can't reach the sound card (Mixxx owns it through ALSA), so
// decklabd relays its preview to Mixxx: [PreviewDeck1], in the headphones,
// with Mixxx's own loop. Nothing on the decks changes, and nothing starts
// while a deck or sampler plays.
TEST_F(DeckLabSystemTest, SampleMakerPreviewPlaysTheCutOnThePreviewDeckOnly) {
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    FakePreviewDeck deck;
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    system.preview()->setHooks(deck.hooks(system, m_dir.filePath(QStringLiteral("Samples"))));
    DeckLabUtilities::Hooks utilityHooks;
    utilityHooks.playing = [&deck] { return deck.playing || deck.decksPlaying; };
    system.utilities()->setHooks(utilityHooks);
    system.utilities()->setStateDirectory(m_dir.filePath(QStringLiteral("run/decklab")));
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    const QJsonArray topics = fake.requests.at(1).value("params").toObject().value("topics").toArray();
    EXPECT_TRUE(topics.contains(QStringLiteral("preview")) && topics.contains(QStringLiteral("sampler")))
            << "Mixxx asks for the relayed requests; an older decklabd ignores the topics";

    const auto ask = [&fake](const QString& event, const QJsonObject& data) {
        const qint64 serial = fake.relay(event, data);
        EXPECT_TRUE(QTest::qWaitFor([&] { return !fake.relayReply(serial).isEmpty(); }, 2000)) << event.toStdString();
        return fake.relayReply(serial);
    };
    const auto refusal = [](const QJsonObject& answer) {
        return answer.value("error").toObject().value("code").toString();
    };
    const QJsonObject cut{{"track_id", 7}, {"start", 2.0}, {"end", 4.0}, {"loop", false}};

    // Only for an open utility.
    EXPECT_EQ(QStringLiteral("denied"), refusal(ask(QStringLiteral("preview.play"), cut)));
    fake.pushEvent(QStringLiteral("apps.changed"), QJsonObject{{"app", "sample-maker"}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !system.utilities()->openApp().isEmpty(); }, 2000));

    // Checked again here, whatever decklabd let through.
    for (const QJsonObject& bad : {QJsonObject{{"track_id", 0}, {"start", 0}, {"end", 1}},
                 QJsonObject{{"track_id", 7.5}, {"start", 0}, {"end", 1}},
                 QJsonObject{{"track_id", "7"}, {"start", 0}, {"end", 1}},
                 QJsonObject{{"track_id", 7}, {"start", -1}, {"end", 1}},
                 QJsonObject{{"track_id", 7}, {"start", 2}, {"end", 2}},
                 QJsonObject{{"track_id", 7}, {"start", 0}, {"end", 7201}},
                 QJsonObject{{"track_id", 7}, {"start", 0}, {"end", 1}, {"loop", 1}},
                 QJsonObject{{"track_id", 7}, {"start", 1}, {"end", 2}, {"from", 2}}}) {
        EXPECT_EQ(QStringLiteral("bad_request"), refusal(ask(QStringLiteral("preview.play"), bad)));
    }
    // Never over a playing deck or sampler.
    deck.decksPlaying = true;
    const QJsonObject busy = ask(QStringLiteral("preview.play"), cut);
    EXPECT_EQ(QStringLiteral("busy"), refusal(busy));
    EXPECT_EQ(QStringLiteral("Stop the decks and samplers to preview."),
            busy.value("error").toObject().value("message").toString());
    EXPECT_TRUE(deck.loads.isEmpty());
    deck.decksPlaying = false;

    // Load by library id, then play IN to OUT once the track is in.
    QJsonObject answer = ask(QStringLiteral("preview.play"), cut);
    EXPECT_EQ(QStringLiteral("loading"), answer.value("result").toObject().value("state").toString());
    EXPECT_EQ(QList<int>{7}, deck.loads);
    EXPECT_TRUE(deck.plays.isEmpty());
    // OUT moved while the track loads: no second load, and the load plays the new cut.
    QJsonObject longer = cut;
    longer.insert("end", 4.5);
    answer = ask(QStringLiteral("preview.play"), longer);
    EXPECT_EQ(QStringLiteral("loading"), answer.value("result").toObject().value("state").toString());
    EXPECT_EQ(QList<int>{7}, deck.loads);
    deck.loaded = 7;
    deck.duration = 180;
    system.preview()->onPreviewLoaded(7);
    ASSERT_EQ(1, deck.plays.size());
    EXPECT_EQ((QList<double>{2, 4.5, 0, 2}), deck.plays.last()) << "IN to OUT, no loop, from IN";
    EXPECT_EQ(DeckLabPreview::State::Playing, system.preview()->state());
    const auto readState = [&] {
        QFile file(system.utilities()->stateFile());
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    };
    EXPECT_TRUE(readState().value("playing").toBool()) << "deck-state.json says so at once: no decoding during a preview";
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return fake.lastPreviewReport().value("state").toString() == QStringLiteral("playing");
    },
            2000));
    EXPECT_EQ(7, fake.lastPreviewReport().value("track_id").toInt());

    // Without loop it stops at OUT by itself.
    deck.position = 4.5;
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.preview()->state() == DeckLabPreview::State::Stopped; }, 1000));
    EXPECT_FALSE(deck.playing);
    EXPECT_FALSE(readState().value("playing").toBool(true));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return fake.lastPreviewReport().value("state").toString() == QStringLiteral("stopped");
    },
            2000));
    EXPECT_DOUBLE_EQ(4.5, fake.lastPreviewReport().value("position").toDouble());

    // Looped: Mixxx's loop wraps it, so nothing stops it at OUT. The track is in already.
    QJsonObject looped = cut;
    looped.insert("loop", true);
    answer = ask(QStringLiteral("preview.play"), looped);
    EXPECT_EQ(QStringLiteral("playing"), answer.value("result").toObject().value("state").toString());
    EXPECT_EQ((QList<double>{2, 4, 1, 2}), deck.plays.last());
    EXPECT_EQ(QList<int>{7}, deck.loads) << "the same track is not loaded twice";
    deck.position = 4.2;
    QTest::qWait(60);
    EXPECT_TRUE(deck.playing);

    // A moved OUT while it plays carries on from where it is, without a seek...
    deck.position = 3.0;
    QJsonObject moved{{"track_id", 7}, {"start", 2.0}, {"end", 6.0}, {"loop", true}, {"from", 3.1}};
    ask(QStringLiteral("preview.play"), moved);
    EXPECT_EQ((QList<double>{2, 6, 1, -1}), deck.plays.last());
    // ...but a far `from` seeks, and OUT past the end of the track stops at the end.
    moved.insert("from", 5.0);
    moved.insert("end", 190.0);
    ask(QStringLiteral("preview.play"), moved);
    EXPECT_EQ((QList<double>{2, 180, 1, 5}), deck.plays.last());

    // A deck that starts stops the preview, and the page is told why.
    deck.decksPlaying = true;
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.preview()->state() == DeckLabPreview::State::Stopped; }, 1500));
    EXPECT_FALSE(deck.playing);
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return fake.lastPreviewReport().value("reason").toString() ==
                QStringLiteral("A deck or sampler started, so the preview stopped.");
    },
            2000));
    deck.decksPlaying = false;

    // STOP; Browse taking the preview deck; a track that is not in the library.
    deck.loaded = 7;
    ask(QStringLiteral("preview.play"), cut);
    answer = ask(QStringLiteral("preview.stop"), QJsonObject());
    EXPECT_EQ(QStringLiteral("stopped"), answer.value("result").toObject().value("state").toString());
    EXPECT_FALSE(deck.playing);
    system.preview()->onPreviewLoaded(9);
    EXPECT_EQ(DeckLabPreview::State::Idle, system.preview()->state());
    answer = ask(QStringLiteral("preview.play"), QJsonObject{{"track_id", 404}, {"start", 0}, {"end", 1}});
    EXPECT_EQ(QStringLiteral("failed"), refusal(answer));
    EXPECT_EQ(QStringLiteral("That track is not in the library."),
            answer.value("error").toObject().value("message").toString());
    // A load that fails is reported, not played.
    ask(QStringLiteral("preview.play"), QJsonObject{{"track_id", 8}, {"start", 0}, {"end", 1}});
    EXPECT_EQ(DeckLabPreview::State::Loading, system.preview()->state());
    system.preview()->onPreviewLoaded(0);
    EXPECT_EQ(DeckLabPreview::State::Idle, system.preview()->state());
    EXPECT_EQ(QStringLiteral("The track could not be loaded."), system.preview()->report().value("reason").toString());

    // Closing Sample Maker pauses and unloads its preview; the decks were never touched.
    deck.loaded = 7;
    deck.duration = 180;
    ask(QStringLiteral("preview.play"), cut);
    EXPECT_TRUE(deck.playing);
    fake.pushEvent(QStringLiteral("apps.changed"), QJsonObject{{"app", QJsonValue::Null}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.preview()->state() == DeckLabPreview::State::Idle; }, 2000));
    EXPECT_FALSE(deck.playing);
    EXPECT_EQ(1, deck.ejects);
    EXPECT_EQ(QStringLiteral("denied"), refusal(ask(QStringLiteral("preview.play"), cut)));
}

// PUT ON PAD: a saved sample's basename, resolved inside Deck Lab Samples
// only, onto the first empty pad.
TEST_F(DeckLabSystemTest, PutOnPadLoadsOnlyASampleFromTheSamplesFolder) {
    const QString samples = m_dir.filePath(QStringLiteral("Music/Deck Lab Samples"));
    ASSERT_TRUE(QDir().mkpath(samples + QStringLiteral("/Beats")));
    const auto touch = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write("RIFF") == 4;
    };
    ASSERT_TRUE(touch(samples + QStringLiteral("/Hook - Cut.wav")));
    ASSERT_TRUE(touch(samples + QStringLiteral("/Beats/Kick.wav")));
    ASSERT_TRUE(touch(m_dir.filePath(QStringLiteral("Music/Outside.wav"))));
    const bool linked = QFile::link(m_dir.filePath(QStringLiteral("Music/Outside.wav")),
            samples + QStringLiteral("/Link.wav"));

    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    FakePreviewDeck deck;
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    system.preview()->setHooks(deck.hooks(system, samples));
    system.utilities()->setStateDirectory(m_dir.filePath(QStringLiteral("run/decklab")));
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    const auto ask = [&fake](const QJsonValue& name) {
        const qint64 serial = fake.relay(QStringLiteral("sampler.load"), QJsonObject{{"name", name}});
        EXPECT_TRUE(QTest::qWaitFor([&] { return !fake.relayReply(serial).isEmpty(); }, 2000));
        return fake.relayReply(serial);
    };
    const auto refusal = [](const QJsonObject& answer) {
        return answer.value("error").toObject().value("code").toString();
    };

    EXPECT_EQ(QStringLiteral("denied"), refusal(ask(QStringLiteral("Hook - Cut.wav"))));
    fake.pushEvent(QStringLiteral("apps.changed"), QJsonObject{{"app", "sample-maker"}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !system.utilities()->openApp().isEmpty(); }, 2000));

    QJsonObject answer = ask(QStringLiteral("Hook - Cut.wav"));
    EXPECT_EQ(3, answer.value("result").toObject().value("pad").toInt());
    EXPECT_EQ(QStringLiteral("Hook - Cut.wav"), answer.value("result").toObject().value("name").toString());
    ASSERT_EQ(1, deck.pads.size());
    EXPECT_EQ(QStringLiteral("3:") + QFileInfo(samples + QStringLiteral("/Hook - Cut.wav")).canonicalFilePath(),
            deck.pads.first());

    QJsonArray bad{QStringLiteral("../Outside.wav"),
            QStringLiteral("Beats/Kick.wav"),
            QStringLiteral("/tmp/Hook - Cut.wav"),
            QStringLiteral(".Hook.wav"),
            QStringLiteral("Hook - Cut.txt"),
            QStringLiteral(" Hook - Cut.wav"),
            QStringLiteral("Missing.wav"),
            QStringLiteral("Beats"),
            7};
    if (linked) {
        bad.append(QStringLiteral("Link.wav"));
    }
    for (const auto& name : std::as_const(bad)) {
        EXPECT_EQ(QStringLiteral("bad_request"), refusal(ask(name))) << name.toString().toStdString();
    }
    EXPECT_EQ(1, deck.pads.size()) << "nothing outside Deck Lab Samples reaches a pad";

    // Every pad full; anything playing (loading reads the disk).
    deck.emptyPad = 0;
    answer = ask(QStringLiteral("Hook - Cut.wav"));
    EXPECT_EQ(QStringLiteral("busy"), refusal(answer));
    EXPECT_EQ(QStringLiteral("Every pad is in use. Empty one on the Sampler page first."),
            answer.value("error").toObject().value("message").toString());
    deck.emptyPad = 4;
    deck.decksPlaying = true;
    EXPECT_EQ(QStringLiteral("busy"), refusal(ask(QStringLiteral("Hook - Cut.wav"))));
    deck.decksPlaying = false;
    deck.playing = true;
    EXPECT_EQ(QStringLiteral("busy"), refusal(ask(QStringLiteral("Hook - Cut.wav"))));
    EXPECT_EQ(1, deck.pads.size());
}

// ------------------------------------------------------------ persistence --

TEST_F(DeckLabSystemTest, ControllerSetupPersistsPerDeviceKeyAcrossRestart) {
    decklab::DeviceProfile profile;
    profile.key = decklab::DeviceStore::deviceKey(0x2b73, 0x0042, QStringLiteral("SN 01"), QStringLiteral("unused"));
    profile.name = QStringLiteral("Generated Controller");
    profile.controller = QStringLiteral("Generated Controller MIDI 1");
    profile.mapping = QStringLiteral("/generated/mappings/Generated.midi.xml");
    profile.decks = 4;
    profile.mainDevice = QStringLiteral("Generated Controller: USB Audio");
    profile.mainBase = 0;
    profile.headphonesDevice = QStringLiteral("Generated Controller: USB Audio");
    profile.headphonesBase = 2;
    EXPECT_EQ(QStringLiteral("2b73:0042:SN 01"), profile.key);
    EXPECT_EQ(QStringLiteral("2b73:0042:SN_01"), decklab::DeviceStore::configItem(profile.key));
    decklab::DeviceStore(config()).save(profile);
    saveAndReloadConfig();
    const auto restored = decklab::DeviceStore(config()).find(profile.key);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(profile, *restored);
    // Without a USB key, the Mixxx controller name finds it too.
    const auto byName = decklab::DeviceStore(config()).findByController(QStringLiteral("Generated Controller MIDI 1"));
    ASSERT_TRUE(byName.has_value());
    EXPECT_EQ(4, byName->decks);
    EXPECT_EQ(1, decklab::DeviceStore(config()).all().size());
    EXPECT_EQ(QStringLiteral("Plain Name MIDI 1"),
            decklab::DeviceStore::deviceKey(std::nullopt, std::nullopt, QString(), QStringLiteral("Plain Name MIDI 1")));
    EXPECT_FALSE(decklab::DeviceStore(config()).find(QStringLiteral("dead:beef")).has_value());
}

// -------------------------------------------------------------- shutdown --

TEST_F(DeckLabSystemTest, TerminationSignalBecomesOneGracefulQtRequest) {
#ifdef Q_OS_UNIX
    QObject owner;
    auto* signals_ = DeckLabTerminationSignals::install(&owner);
    ASSERT_NE(nullptr, signals_);
    QSignalSpy requested(signals_, &DeckLabTerminationSignals::terminationRequested);
    // Before arm(): held until the event loop is ready.
    std::raise(SIGTERM);
    QTest::qWait(100);
    EXPECT_EQ(0, requested.count());
    EXPECT_TRUE(DeckLabTerminationSignals::requested());
    signals_->arm();
    ASSERT_TRUE(QTest::qWaitFor([&] { return requested.count() == 1; }, 2000));
    EXPECT_EQ(SIGTERM, requested.at(0).at(0).toInt());
    // Delivered once; the default action is restored for a second signal.
    struct sigaction current = {};
    sigaction(SIGTERM, nullptr, &current);
    EXPECT_EQ(SIG_DFL, current.sa_handler);
#else
    GTEST_SKIP() << "POSIX signals only";
#endif
}

// ----------------------------------------------------- pages and first run --

TEST_F(DeckLabSystemTest, SystemPagesShowServiceStateAndRefuseUnsafeEject) {
    DeckLabFakeService fake;
    const QString mount = fake.drives.at(0).toObject().value("mount").toString();
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    system.setLoadedTracksProvider([mount] {
        return QList<decklab::LoadedTrack>{{QStringLiteral("[Channel1]"), mount + QStringLiteral("/generated.wav")}};
    });
    QWidget host;
    // The surface style sets min-height on every button and combo box.
    host.setStyleSheet(QStringLiteral("QPushButton {min-height:28px;padding:5px 9px;} "
                                      "QComboBox {min-height:38px;}"));
    host.resize(650, 440);
    auto* page = new DeckLabSystemPage(&host);
    page->resize(650, 440);
    page->setSystem(&system);
    host.show();
    // Offline first: the page explains, DJing is unaffected.
    page->showSection(DeckLabSystemPage::Section::Wifi);
    auto* offline = page->findChild<QLabel*>();
    ASSERT_NE(nullptr, offline);
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    ASSERT_TRUE(QTest::qWaitFor([&] { return visibleCount(page, QStringLiteral("decklab-wifi-network")) == 4; }, 3000));
    EXPECT_TRUE(fake.calls.contains(QStringLiteral("wifi.scan")));

    page->showSection(DeckLabSystemPage::Section::Storage);
    ASSERT_TRUE(QTest::qWaitFor([&] { return visibleCount(page, QStringLiteral("decklab-drive-eject")) == 2; }, 3000));
    visibleButton(page, QStringLiteral("decklab-drive-eject"), 0)->click();
    QLabel* message = nullptr;
    for (auto* label : page->findChildren<QLabel*>(QStringLiteral("decklab-system-message"))) {
        if (label->isVisibleTo(page)) {
            message = label;
        }
    }
    ASSERT_NE(nullptr, message);
    EXPECT_TRUE(message->text().contains(QStringLiteral("Deck 1")));
    EXPECT_FALSE(fake.calls.contains(QStringLiteral("usb.eject")));

    // Brightness never goes below 5%.
    page->showSection(DeckLabSystemPage::Section::Screen);
    auto* down = visibleButton(page, QStringLiteral("decklab-brightness-down"));
    ASSERT_NE(nullptr, down);
    auto* slider = page->findChild<QSlider*>(QStringLiteral("decklab-brightness"));
    ASSERT_NE(nullptr, slider);
    EXPECT_EQ(5, slider->minimum());
    // Wait for display.get to load the current 80 % before adjusting.
    ASSERT_TRUE(QTest::qWaitFor([&] { return slider->value() == 80; }, 2000));
    for (int press = 0; press < 15; ++press) {
        down->click();
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return fake.calls.contains(QStringLiteral("display.set")); }, 2000));
    EXPECT_EQ(5, fake.display.value("brightness").toInt());
    // Every visible control on these pages is a touch target.
    for (auto section : {DeckLabSystemPage::Section::Home, DeckLabSystemPage::Section::Wifi,
                 DeckLabSystemPage::Section::Bluetooth, DeckLabSystemPage::Section::Storage,
                 DeckLabSystemPage::Section::Power, DeckLabSystemPage::Section::Maintenance}) {
        page->showSection(section);
        QTest::qWait(50);
        for (auto* button : page->findChildren<QPushButton*>()) {
            if (button->isVisibleTo(page)) {
                EXPECT_GE(button->height(), 44) << qPrintable(button->objectName());
            }
        }
    }
}

TEST_F(DeckLabSystemTest, SystemKitUsesBarlowAndAboutNamesTheRealSource) {
    deckLabLoadFonts(config()->getResourcePath());
    ASSERT_TRUE(QFontDatabase::families().contains(QStringLiteral("Barlow")));
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    // A plain host: the kit's own sheet must carry the brand face and style
    // without any Deck Lab surface around it (Confirm sheets on the window).
    QWidget host;
    host.resize(800, 480);
    auto* page = new DeckLabSystemPage(&host);
    page->resize(800, 480);
    page->setSystem(&system);
    host.show();
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    page->showSection(DeckLabSystemPage::Section::About);
    QTest::qWait(50);
    auto* source = page->findChild<QLabel*>(QStringLiteral("decklab-about-source"));
    ASSERT_NE(nullptr, source);
    EXPECT_EQ(QStringLiteral("https://github.com/brakoish/mixxx"), source->text());
    EXPECT_TRUE(source->isVisibleTo(page));
    for (auto* label : page->findChildren<QLabel*>()) {
        EXPECT_FALSE(label->text().contains(QStringLiteral("REPO URL"))) << qPrintable(label->text());
    }
    for (auto section : {DeckLabSystemPage::Section::Home, DeckLabSystemPage::Section::About,
                 DeckLabSystemPage::Section::Time, DeckLabSystemPage::Section::Storage}) {
        page->showSection(section);
        QTest::qWait(50);
        for (auto* widget : page->findChildren<QWidget*>()) {
            if (widget->isVisibleTo(page) && (qobject_cast<QLabel*>(widget) || qobject_cast<QAbstractButton*>(widget))) {
                EXPECT_TRUE(QFontInfo(widget->font()).family().startsWith(QStringLiteral("Barlow")))
                        << qPrintable(widget->objectName()) << " " << qPrintable(QFontInfo(widget->font()).family());
            }
        }
    }
    // The clock is the hero readout: 36 px Barlow Semi Condensed.
    page->showSection(DeckLabSystemPage::Section::Time);
    auto* clock = page->findChild<QLabel*>(QStringLiteral("decklab-time-clock"));
    ASSERT_NE(nullptr, clock);
    EXPECT_EQ(36, QFontInfo(clock->font()).pixelSize());
    EXPECT_EQ(QStringLiteral("Barlow Semi Condensed"), QFontInfo(clock->font()).family());
    // A sheet hosted outside the surface gets the kit's face and buttons.
    auto* sheet = decklab::ui::Confirm::inform(&host, QStringLiteral("Status"), QStringLiteral("No sound output."));
    QTest::qWait(20);
    auto* ok = sheet->findChild<QPushButton*>(QStringLiteral("decklab-confirm-cancel"));
    ASSERT_NE(nullptr, ok);
    EXPECT_TRUE(QFontInfo(ok->font()).family().startsWith(QStringLiteral("Barlow")));
    EXPECT_EQ(16, QFontInfo(ok->font()).pixelSize());
    EXPECT_GE(ok->height(), 44);
}

TEST_F(DeckLabSystemTest, UseForRecordingsAsksBeforeMakingAReadOnlyDriveWritable) {
    // Generated drives mounted under the test's own temporary folder.
    const QString stick = m_dir.filePath(QStringLiteral("media/6A3F-1C2B"));
    const QString sets = m_dir.filePath(QStringLiteral("media/MIX-SETS"));
    ASSERT_TRUE(QDir().mkpath(stick));
    ASSERT_TRUE(QDir().mkpath(sets));
    DeckLabFakeService fake;
    fake.drives = QJsonArray{QJsonObject{{"id", "6A3F-1C2B"}, {"label", "DJ USB"}, {"mount", stick},
                                     {"size_mb", 30528}, {"free_mb", 12402}, {"fs", "exfat"}, {"read_only", true},
                                     {"rekordbox", true}, {"busy", false}},
            QJsonObject{{"id", "0e9c1d7a-test"}, {"label", "MIX SETS"}, {"mount", sets}, {"size_mb", 61056},
                    {"free_mb", 40220}, {"fs", "ext4"}, {"read_only", false}, {"rekordbox", false}, {"busy", false}}};
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    QWidget host;
    host.resize(800, 480);
    auto* page = new DeckLabSystemPage(&host);
    page->resize(800, 480);
    page->setSystem(&system);
    host.show();
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady() && system.usbDrives().size() == 2; }, 3000));
    page->showSection(DeckLabSystemPage::Section::Storage);
    ASSERT_TRUE(QTest::qWaitFor([&] { return visibleCount(page, QStringLiteral("decklab-drive-record")) == 2; }, 3000));

    // A read-only rekordbox stick: say what happens and suggest the other drive.
    visibleButton(page, QStringLiteral("decklab-drive-record"), 0)->click();
    auto* sheet = openSheet(&host);
    ASSERT_NE(nullptr, sheet);
    const QString text = sheetText(sheet);
    EXPECT_TRUE(text.contains(QStringLiteral("Make DJ USB writable?"))) << qPrintable(text);
    EXPECT_TRUE(text.contains(QStringLiteral("Deck Lab will write recordings to DJ USB. Your rekordbox export isn't changed.")))
            << qPrintable(text);
    EXPECT_TRUE(text.contains(QStringLiteral("MIX SETS is ready to use"))) << qPrintable(text);
    QTest::qWait(100);
    EXPECT_FALSE(fake.calls.contains(QStringLiteral("usb.set_writable")));
    // Cancel leaves the drive alone.
    auto* cancel = visibleButton(sheet, QStringLiteral("decklab-confirm-cancel"));
    ASSERT_NE(nullptr, cancel);
    EXPECT_EQ(QStringLiteral("Cancel"), cancel->text());
    EXPECT_EQ(QStringLiteral("Make writable"),
            visibleButton(sheet, QStringLiteral("decklab-confirm-accept"))->text());
    cancel->click();
    QTest::qWait(100);
    EXPECT_EQ(nullptr, openSheet(&host));
    EXPECT_FALSE(fake.calls.contains(QStringLiteral("usb.set_writable")));
    // Confirming makes it writable and sets up the recordings folder.
    visibleButton(page, QStringLiteral("decklab-drive-record"), 0)->click();
    sheet = openSheet(&host);
    ASSERT_NE(nullptr, sheet);
    visibleButton(sheet, QStringLiteral("decklab-confirm-accept"))->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return fake.calls.contains(QStringLiteral("usb.set_writable")); }, 2000));
    EXPECT_TRUE(QTest::qWaitFor([&] {
        return QFileInfo(QDir(stick).filePath(QStringLiteral("Deck Lab Recordings"))).isDir();
    }, 2000));
}

TEST_F(DeckLabSystemTest, BluetoothRowTapOpensDetailsAndOnlyTheButtonActs) {
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    QWidget host;
    host.resize(800, 480);
    auto* page = new DeckLabSystemPage(&host);
    page->resize(800, 480);
    page->setSystem(&system);
    host.show();
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    page->showSection(DeckLabSystemPage::Section::Bluetooth);
    ASSERT_TRUE(QTest::qWaitFor([&] { return visibleCount(page, QStringLiteral("decklab-bt-device")) == 2; }, 3000));
    const auto actions = [&fake] {
        int count = 0;
        for (const auto& call : std::as_const(fake.calls)) {
            count += call == QStringLiteral("bt.pair") || call == QStringLiteral("bt.connect") ||
                    call == QStringLiteral("bt.disconnect") || call == QStringLiteral("bt.forget");
        }
        return count;
    };
    // Tapping the paired speaker's row only opens its details.
    visibleButton(page, QStringLiteral("decklab-bt-device"), 0)->click();
    QTest::qWait(400);
    EXPECT_EQ(0, actions());
    auto* sheet = openSheet(&host);
    ASSERT_NE(nullptr, sheet);
    const QString text = sheetText(sheet);
    EXPECT_TRUE(text.contains(QStringLiteral("Practice Speaker"))) << qPrintable(text);
    EXPECT_TRUE(text.contains(QStringLiteral("02:00:00:00:00:01"))) << qPrintable(text);
    auto* close = visibleButton(sheet, QStringLiteral("decklab-confirm-cancel"));
    ASSERT_NE(nullptr, close);
    EXPECT_EQ(QStringLiteral("Close"), close->text());
    close->click();
    QTest::qWait(100);
    EXPECT_EQ(0, actions());
    // Only the explicit action acts: here, from the details sheet.
    visibleButton(page, QStringLiteral("decklab-bt-device"), 0)->click();
    sheet = openSheet(&host);
    ASSERT_NE(nullptr, sheet);
    auto* connect = visibleButton(sheet, QStringLiteral("decklab-confirm-accept"));
    ASSERT_NE(nullptr, connect);
    EXPECT_EQ(QStringLiteral("Connect"), connect->text());
    connect->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return fake.calls.contains(QStringLiteral("bt.connect")); }, 2000));
    EXPECT_EQ(1, actions());
}

TEST_F(DeckLabSystemTest, TestTonesStartQuietBehindALevelsWarning) {
    // -24 dBFS peak: about 6 % of full scale, a quarter of the old -12 dBFS.
    EXPECT_DOUBLE_EQ(-24.0, decklab::kTestToneDbfs);
    EXPECT_NEAR(0.0631, decklab::testToneAmplitude(), 0.0001);
    // r4-system (sound check): a second tap adds 6 dB, never more.
    EXPECT_DOUBLE_EQ(-18.0, decklab::kTestToneBoostDbfs);
    EXPECT_NEAR(0.1259, decklab::testToneAmplitude(decklab::kTestToneBoostDbfs), 0.0001);
    EXPECT_NEAR(0.1259, decklab::testToneAmplitude(0.0), 0.0001);
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    QWidget host;
    host.resize(800, 480);
    host.show();
    auto* wizard = new DeckLabSetupWizard(&system, &host);
    wizard->start(DeckLabSetupWizard::Step::Audio);
    QTest::qWait(20);
    auto* levels = wizard->findChild<QLabel*>(QStringLiteral("decklab-setup-level-warning"));
    ASSERT_NE(nullptr, levels);
    EXPECT_TRUE(levels->isVisibleTo(wizard));
    EXPECT_EQ(QStringLiteral("Turn MASTER and PHONES down before you test."), levels->text());
    EXPECT_GE(QFontInfo(levels->font()).pixelSize(), 16);
    // Sound check: big tone pads, and YES / NO only after a tone.
    for (const char* name : {"decklab-setup-test-left", "decklab-setup-test-right", "decklab-setup-test-phones"}) {
        auto* pad = wizard->findChild<QPushButton*>(QString::fromLatin1(name));
        ASSERT_NE(nullptr, pad) << name;
        EXPECT_GE(pad->height(), 44) << name;
    }
    auto* heard = wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-heard-main"));
    ASSERT_NE(nullptr, heard);
    EXPECT_FALSE(heard->isVisibleTo(wizard));
    // No output chosen: the pad says so in its panel, nothing plays.
    wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-test-left"))->click();
    QTest::qWait(20);
    auto* question = wizard->findChild<QLabel*>(QStringLiteral("decklab-setup-question-main"));
    ASSERT_NE(nullptr, question);
    EXPECT_FALSE(question->text().isEmpty());
    EXPECT_FALSE(heard->isVisibleTo(wizard));
}

// ------------------------------------------------------------ r4-system --

TEST_F(DeckLabSystemTest, RailLampsTeachAndRedIsOnlyForLive) {
    using decklab::rail::Inputs;
    using decklab::rail::Slot;
    using decklab::ui::Lamp;
    Inputs in;
    in.outputWorking = true;
    in.serviceAttached = true;
    in.serviceOnline = true;
    in.wifi = Inputs::Wifi::Connected;
    in.controller = Inputs::Controller::Working;
    // All good: green lamps, nothing red.
    for (Slot slot : {Slot::Controller, Slot::Network, Slot::Recording, Slot::Output}) {
        EXPECT_NE(Lamp::Live, decklab::rail::slotView(slot, in).lamp);
        EXPECT_NE(Lamp::LiveOutline, decklab::rail::slotView(slot, in).lamp);
    }
    EXPECT_EQ(Lamp::Go, decklab::rail::slotView(Slot::Output, in).lamp);
    EXPECT_EQ(decklab::rail::Alert::None, decklab::rail::alertFor(in));
    // Recording is live: red, with the elapsed time in the slot.
    in.recording = true;
    in.recordingSeconds = 754;
    EXPECT_EQ(Lamp::Live, decklab::rail::slotView(Slot::Recording, in).lamp);
    EXPECT_EQ(QStringLiteral("12:34"), decklab::rail::slotView(Slot::Recording, in).label);
    EXPECT_EQ(QStringLiteral("1h02"), decklab::rail::recordingTime(3725));
    // Dropouts and an offline service are caution, never red or amber.
    in.recentDropouts = true;
    EXPECT_EQ(Lamp::Caution, decklab::rail::slotView(Slot::Output, in).lamp);
    in.serviceOnline = false;
    EXPECT_EQ(Lamp::Caution, decklab::rail::slotView(Slot::Network, in).lamp);
    // No sound output is the one red alert; low power is caution.
    in.outputWorking = false;
    EXPECT_EQ(decklab::rail::Alert::NoOutput, decklab::rail::alertFor(in));
    in.outputPending = true; // start-up: devices not open yet
    EXPECT_EQ(decklab::rail::Alert::None, decklab::rail::alertFor(in));
    in.outputPending = false;
    in.outputWorking = true;
    in.undervoltage = true;
    EXPECT_EQ(decklab::rail::Alert::LowPower, decklab::rail::alertFor(in));
    // USB: presence and count. r5-polish: no "RB" abbreviation; the stick's
    // name ("DJ USB") is in Browse and the status sheet.
    in.usbDrives = 1;
    in.usbRekordbox = true;
    EXPECT_EQ(QStringLiteral("USB"), decklab::rail::usbView(in).label);
    in.usbDrives = 2;
    EXPECT_EQ(QStringLiteral("USB 2"), decklab::rail::usbView(in).label);
    in.controllerNew = true;
    EXPECT_EQ(QStringLiteral("NEW"), decklab::rail::slotView(Slot::Controller, in).label);
}

TEST_F(DeckLabSystemTest, HoldToConfirmNeedsTheWholeHold) {
    QWidget host;
    host.resize(400, 200);
    auto* hold = new decklab::ui::HoldButton(QStringLiteral("Turn off"), &host, QStringLiteral("hold"));
    hold->setGeometry(10, 10, 200, 44);
    host.show();
    int fired = 0;
    QObject::connect(hold, &decklab::ui::HoldButton::confirmed, [&fired] { ++fired; });
    // A tap never acts.
    hold->click();
    QTest::mouseClick(hold, Qt::LeftButton);
    EXPECT_EQ(0, fired);
    // Let go early: snaps back.
    QTest::mousePress(hold, Qt::LeftButton);
    QTest::qWait(500);
    EXPECT_GT(hold->progress(), 0.2);
    QTest::mouseRelease(hold, Qt::LeftButton);
    EXPECT_EQ(0, fired);
    EXPECT_DOUBLE_EQ(0, hold->progress());
    // The whole 1.2 s.
    QTest::mousePress(hold, Qt::LeftButton);
    EXPECT_TRUE(QTest::qWaitFor([&] { return fired == 1; }, 2500));
    QTest::mouseRelease(hold, Qt::LeftButton);
    // The knob: press arms, a second press confirms; back disarms.
    EXPECT_FALSE(hold->knobPress());
    EXPECT_TRUE(hold->armed());
    hold->knobCancel();
    EXPECT_FALSE(hold->armed());
    hold->knobPress();
    hold->knobPress();
    EXPECT_EQ(2, fired);
    // A held knob press (scripts that report the release) works like a finger.
    hold->knobHold(true);
    EXPECT_TRUE(QTest::qWaitFor([&] { return fired == 3; }, 2500));
    // A gate refuses with the reason; a reason locks it.
    QString refused;
    QObject::connect(hold, &decklab::ui::HoldButton::refused, [&refused](const QString& text) { refused = text; });
    hold->setGate([] { return QStringLiteral("Stop all decks and samplers first."); });
    QTest::mousePress(hold, Qt::LeftButton);
    EXPECT_EQ(QStringLiteral("Stop all decks and samplers first."), refused);
    EXPECT_FALSE(hold->holding());
    QTest::mouseRelease(hold, Qt::LeftButton);
    hold->setReason(QStringLiteral("Unavailable"));
    EXPECT_FALSE(hold->isEnabled());
}

TEST_F(DeckLabSystemTest, UnitHomeTilesShowLiveValuesAndBrightnessInline) {
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    QWidget host;
    host.resize(800, 436);
    auto* page = new DeckLabSystemPage(&host);
    page->resize(800, 436);
    page->setSystem(&system);
    host.show();
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    page->showSection(DeckLabSystemPage::Section::Home);
    auto* wifi = page->tile(DeckLabSystemPage::Section::Wifi);
    auto* screen = page->tile(DeckLabSystemPage::Section::Screen);
    auto* time = page->tile(DeckLabSystemPage::Section::Time);
    ASSERT_NE(nullptr, wifi); ASSERT_NE(nullptr, screen); ASSERT_NE(nullptr, time);
    // Live values, not descriptions (Y-02, Y-03, Y-27).
    EXPECT_TRUE(QTest::qWaitFor([&] { return wifi->accessibleDescription().contains(QStringLiteral("Studio-WiFi")); }, 3000));
    EXPECT_TRUE(QTest::qWaitFor([&] { return screen->accessibleDescription().startsWith(QStringLiteral("80 %")); }, 3000));
    EXPECT_TRUE(QTest::qWaitFor([&] { return time->accessibleDescription().contains(QStringLiteral("London")); }, 3000));
    EXPECT_FALSE(page->findChild<QLabel*>(QStringLiteral("decklab-system-service-state"))->isVisibleTo(page));
    // Brightness inline: snapped to 10s, never below 5 %, booth preset 30 %.
    visibleButton(page, QStringLiteral("decklab-tile-brightness-down"))->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return fake.display.value("brightness").toInt() == 70; }, 2000));
    visibleButton(page, QStringLiteral("decklab-tile-brightness-booth"))->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return fake.display.value("brightness").toInt() == 30; }, 2000));
    for (int press = 0; press < 6; ++press) {
        visibleButton(page, QStringLiteral("decklab-tile-brightness-down"))->click();
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return fake.display.value("brightness").toInt() == 5; }, 2000));
    // Power is hold-to-confirm, right on the tile; a tap does nothing.
    auto* off = page->findChild<decklab::ui::HoldButton*>(QStringLiteral("decklab-tile-power-off"));
    ASSERT_NE(nullptr, off);
    off->click();
    QTest::qWait(50);
    EXPECT_FALSE(fake.calls.contains(QStringLiteral("system.power")));
    // The Power page rows are hold rows too, with the copy deck's names.
    page->showSection(DeckLabSystemPage::Section::Power);
    auto* restart = page->findChild<decklab::ui::HoldButton*>(QStringLiteral("decklab-power-restart"));
    ASSERT_NE(nullptr, restart);
    EXPECT_EQ(QStringLiteral("Restart app"), restart->text());
    EXPECT_EQ(64, restart->height());
}


TEST_F(DeckLabSystemTest, FirstRunFlowCompletesWithoutAControllerAndSetsTheFlag) {
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    using Kind = decklab::AudioDevice::Kind;
    system.setAudioDevicesOverride({audioDevice(QStringLiteral("Controller: USB Audio"), 4, Kind::External, true),
            audioDevice(QStringLiteral("bcm2835 Headphones: -"), 2, Kind::BuiltinJack)});
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady() && !system.devices().isEmpty(); }, 3000));
    EXPECT_FALSE(system.firstRunDone());
    QWidget host;
    // The surface style sets min-height on every button and combo box.
    host.setStyleSheet(QStringLiteral("QPushButton {min-height:28px;padding:5px 9px;} "
                                      "QComboBox {min-height:38px;}"));
    host.resize(800, 480);
    host.show();
    auto* wizard = new DeckLabSetupWizard(&system, &host);
    QSignalSpy finished(wizard, &DeckLabSetupWizard::finished);
    wizard->startFirstRun();
    using Step = DeckLabSetupWizard::Step;
    EXPECT_EQ(Step::Welcome, wizard->step());
    auto* next = wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-next"));
    ASSERT_NE(nullptr, next);
    next->click();
    EXPECT_EQ(Step::Wifi, wizard->step());
    next->click();
    EXPECT_EQ(Step::Time, wizard->step());
    next->click();
    EXPECT_EQ(Step::Controller, wizard->step());
    // decklabd reports a controller Mixxx has not enumerated: the mapping is
    // suggested from bundled metadata, and Next explains instead of failing.
    EXPECT_TRUE(wizard->chosenMapping().endsWith(QStringLiteral("Pioneer-DDJ-FLX4.midi.xml")))
            << qPrintable(wizard->chosenMapping());
    next->click();
    EXPECT_EQ(Step::Controller, wizard->step());
    // Audio policy for a 4-output controller; combo boxes keep touch height.
    wizard->start(Step::Audio);
    auto* main = wizard->findChild<QComboBox*>(QStringLiteral("decklab-setup-main"));
    ASSERT_NE(nullptr, main);
    EXPECT_GE(main->height(), 44);
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("Controller: USB Audio"), 0}), wizard->chosenAudio().main);
    EXPECT_EQ((decklab::AudioRoute{QStringLiteral("Controller: USB Audio"), 2}), wizard->chosenAudio().headphones);
    // "Skip – set up later" finishes the first run without a controller.
    wizard->startFirstRun();
    wizard->skipToDone();
    EXPECT_EQ(Step::Done, wizard->step());
    for (auto* button : wizard->findChildren<QPushButton*>()) {
        if (button->isVisibleTo(wizard)) {
            EXPECT_GE(button->height(), 44) << qPrintable(button->objectName());
            EXPECT_TRUE(QRect(QPoint(), host.size()).contains(QRect(button->mapTo(&host, QPoint()), button->size())))
                    << qPrintable(button->objectName());
        }
    }
    next->click();
    ASSERT_EQ(1, finished.count());
    EXPECT_TRUE(finished.at(0).at(0).toBool());
    EXPECT_TRUE(system.firstRunDone());
    EXPECT_TRUE(decklab::DeviceStore(config()).all().isEmpty());
}

// ------------------------------------------------------------ r5-polish --
// Fake enumerations only: generated USB locations and keys, bundled mappings.

namespace {

decklab::ControllerEndpoint midiPort(const QString& name, const QString& usbPath,
        const QString& mapping = QString(), bool open = false) {
    decklab::ControllerEndpoint endpoint;
    endpoint.name = name;
    endpoint.usbPath = usbPath;
    endpoint.mapping = mapping;
    endpoint.open = open;
    endpoint.enabled = open;
    return endpoint;
}

// A HID interface as Mixxx's HID enumerator names it ("<product> <serial>_<interface>").
decklab::ControllerEndpoint hidInterface(const QString& product, uint16_t usagePage, int interface,
        const QString& usbPath, uint16_t vendor = 0x15e4, uint16_t productId = 0x0035) {
    decklab::ControllerEndpoint endpoint;
    endpoint.name = QStringLiteral("%1 _%2").arg(product).arg(interface);
    endpoint.hid = true;
    endpoint.product = product;
    endpoint.usagePage = usagePage;
    endpoint.interfaceNumber = interface;
    endpoint.vendorId = vendor;
    endpoint.productId = productId;
    endpoint.usbPath = usbPath;
    return endpoint;
}

// decklabd's report for one USB device: MIDI port, vendor HID, audio card.
QJsonObject reportedMixTrack(const QString& key, const QString& usbPort) {
    return QJsonObject{{"key", key}, {"vid", "15e4"}, {"pid", "0035"}, {"serial", QJsonValue()},
            {"name", "Numark MixTrack Pro 3"}, {"midi_ports", QJsonArray{"MixTrack Pro 3 MIDI 1"}}, {"hid", true},
            {"audio_card", QJsonObject{{"alsa_id", "M3"}, {"name", "MixTrack Pro 3"}, {"playback_channels", 4},
                                   {"capture_channels", 0}}},
            {"usb_port", usbPort}};
}

QList<decklab::ui::Row*> visibleRows(QWidget* root, const QString& name) {
    QList<decklab::ui::Row*> rows;
    for (auto* row : root->findChildren<decklab::ui::Row*>(name)) {
        if (row->isVisibleTo(root)) {
            rows.append(row);
        }
    }
    return rows;
}

void captureWidget(QWidget* widget, const QString& name) {
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        EXPECT_TRUE(widget->grab().save(QDir(output).filePath(name))) << name.toStdString();
    }
}

void writeFile(const QString& path, const QByteArray& content) {
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(content);
}

} // namespace

// Issue: one MixTrack Pro 3 showed as two rows, its vendor HID interface as
// "Ready to set up", and that interface got opened ("Opening HID device ...
// by HID path"). The MIDI port and the HID interface are one row now, the
// badge says MIDI, and the HID interface is never opened.
TEST_F(DeckLabSystemTest, OneControllerIsOneRowAndItsVendorHidInterfaceStaysClosed) {
    DeckLabFakeService fake;
    fake.devices = QJsonObject{{"controllers", QJsonArray{reportedMixTrack("15e4:0035", "1-1.2")}},
            {"audio_cards", QJsonArray{}}};
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    const QString midiMapping = mappingPath(config(), QStringLiteral("Numark-Mixtrack-3.midi.xml"));
    ASSERT_TRUE(QFileInfo::exists(midiMapping));
    const QString location = QStringLiteral("usb-0000:01:00.0-1.2");
    QList<decklab::ControllerEndpoint> endpoints{
            midiPort(QStringLiteral("MixTrack Pro 3 MIDI 1"), location, midiMapping, true),
            hidInterface(QStringLiteral("MixTrack Pro 3"), 0xFFA0, 3, location)};
    QStringList opened;
    QStringList disabled;
    DeckLabSystem::ControllerHooks hooks;
    hooks.endpoints = [&endpoints] { return endpoints; };
    hooks.open = [&](const QString& name, const QString& mapping, QString*) {
        opened.append(name);
        for (auto& endpoint : endpoints) {
            if (endpoint.name == name) {
                endpoint.open = true;
                endpoint.enabled = true;
                endpoint.mapping = mapping;
            }
        }
        return true;
    };
    // Records only: the real disable is queued on the controller thread.
    hooks.disable = [&disabled](const decklab::ControllerEndpoint& endpoint) { disabled.append(endpoint.name); };
    system.setControllerHooksForTesting(hooks);
    using Kind = decklab::AudioDevice::Kind;
    system.setAudioDevicesOverride({audioDevice(QStringLiteral("MixTrack Pro 3: USB Audio"), 4, Kind::External, true)});
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady() && !system.devices().isEmpty(); }, 3000));

    // One row: decklabd's device with both interfaces, MIDI in use.
    auto rows = system.controllers();
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ(QStringLiteral("15e4:0035"), rows[0].key);
    EXPECT_EQ(QStringLiteral("Numark MixTrack Pro 3"), rows[0].name);
    EXPECT_EQ(QStringLiteral("MixTrack Pro 3 MIDI 1"), rows[0].mixxxName);
    EXPECT_FALSE(rows[0].hid);
    EXPECT_TRUE(rows[0].open);
    EXPECT_EQ((QStringList{"MixTrack Pro 3 MIDI 1", "MixTrack Pro 3 _3"}), rows[0].interfaces);
    EXPECT_EQ(QStringList{QStringLiteral("MixTrack Pro 3 _3")}, rows[0].suppressed);
    // Before Mixxx enumerates it, decklabd's device is a MIDI row (it has a
    // MIDI port), not HID.
    {
        const auto saved = endpoints;
        endpoints.clear();
        const auto pending = system.controllers();
        ASSERT_EQ(1, pending.size());
        EXPECT_TRUE(pending[0].mixxxName.isEmpty());
        EXPECT_FALSE(pending[0].hid);
        endpoints = saved;
    }

    // The wizard's controller step: one row, MIDI badge, working; Next keeps
    // the working mapping and opens nothing.
    deckLabLoadFonts(config()->getResourcePath());
    QWidget host;
    host.setStyleSheet(QStringLiteral("QPushButton {min-height:28px;padding:5px 9px;} QComboBox {min-height:38px;}"));
    host.resize(800, 480);
    host.show();
    auto* wizard = new DeckLabSetupWizard(&system, &host);
    wizard->start();
    QTest::qWait(30);
    const auto shown = visibleRows(wizard, QStringLiteral("decklab-setup-controller"));
    ASSERT_EQ(1, shown.size());
    EXPECT_EQ(QStringLiteral("Numark MixTrack Pro 3"), shown[0]->titleText());
    EXPECT_EQ(QStringList{QStringLiteral("MIDI")}, shown[0]->tags());
    EXPECT_TRUE(shown[0]->subtitleText().startsWith(QStringLiteral("Working"))) << shown[0]->subtitleText().toStdString();
    wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-next"))->click();
    EXPECT_EQ(DeckLabSetupWizard::Step::Audio, wizard->step());
    EXPECT_TRUE(opened.isEmpty()) << opened.join(QStringLiteral(", ")).toStdString();
    wizard->hide();

    // An earlier build gave the HID interface the MIDI mapping and enabled
    // it, so Mixxx reopened it at every start: Deck Lab closes and disables it.
    endpoints[1].open = true;
    endpoints[1].enabled = true;
    endpoints[1].mapping = midiMapping;
    rows = system.controllers();
    ASSERT_EQ(1, rows.size());
    EXPECT_FALSE(rows[0].hid); // the badge says what is in use
    EXPECT_EQ(QStringList{QStringLiteral("MixTrack Pro 3 _3")}, system.closeStrayInterfaces());
    EXPECT_EQ(QStringList{QStringLiteral("MixTrack Pro 3 _3")}, disabled);
    // Once Mixxx has applied it (closed, enabled = 0, no mapping), nothing more.
    endpoints[1].open = false;
    endpoints[1].enabled = false;
    endpoints[1].mapping.clear();
    EXPECT_TRUE(system.closeStrayInterfaces().isEmpty());
    // Never opened by Deck Lab, with any mapping.
    QString error;
    EXPECT_FALSE(system.applyMapping(QStringLiteral("MixTrack Pro 3 _3"), midiMapping, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(system.applyMapping(QStringLiteral("MixTrack Pro 3 _3"),
            mappingPath(config(), QStringLiteral("Traktor Kontrol S4 MK3.hid.xml")), &error));
    // Nor a HID mapping on the MIDI port.
    EXPECT_FALSE(system.applyMapping(QStringLiteral("MixTrack Pro 3 MIDI 1"),
            mappingPath(config(), QStringLiteral("Traktor Kontrol S4 MK3.hid.xml")), &error));
    EXPECT_TRUE(opened.isEmpty());

    // Replugged, with a profile an earlier build saved for the HID interface:
    // the mapping goes back on the MIDI port only.
    decklab::DeviceProfile profile;
    profile.key = QStringLiteral("15e4:0035");
    profile.name = QStringLiteral("Numark MixTrack Pro 3");
    profile.controller = QStringLiteral("MixTrack Pro 3 _3");
    profile.mapping = midiMapping;
    decklab::DeviceStore(config()).save(profile);
    endpoints[0].open = false;
    QJsonObject changed = fake.devices;
    changed.insert(QStringLiteral("added"), QJsonArray{QStringLiteral("15e4:0035")});
    fake.pushEvent(QStringLiteral("devices.changed"), changed);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !opened.isEmpty(); }, 3000));
    EXPECT_EQ(QStringList{QStringLiteral("MixTrack Pro 3 MIDI 1")}, opened);
    EXPECT_FALSE(endpoints[1].open);

    // Without decklabd (or on a desktop without USB locations) the name and
    // the USB ids still make one row.
    endpoints[0].usbPath.clear();
    endpoints[1].usbPath.clear();
    const auto groups = decklab::groupControllers(endpoints, {});
    ASSERT_EQ(1, groups.size());
    ASSERT_NE(nullptr, groups[0].primaryEndpoint());
    EXPECT_FALSE(groups[0].primaryEndpoint()->hid);
    EXPECT_EQ(QList<int>{1}, groups[0].suppressed);
}

TEST_F(DeckLabSystemTest, HidOnlyAndTwinControllersKeepTheirOwnRows) {
    // A HID-only controller (vendor page, as most HID DJ controllers use):
    // one HID row, opened with its .hid.xml mapping as before.
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    QList<decklab::ControllerEndpoint> endpoints{
            hidInterface(QStringLiteral("Generated HID Deck"), 0xFF01, 4, QStringLiteral("usb-0000:01:00.0-1.3"),
                    0x17cc, 0x1720)};
    QStringList opened;
    DeckLabSystem::ControllerHooks hooks;
    hooks.endpoints = [&endpoints] { return endpoints; };
    hooks.open = [&opened](const QString& name, const QString&, QString*) {
        opened.append(name);
        return true;
    };
    hooks.disable = [](const decklab::ControllerEndpoint& endpoint) { ADD_FAILURE() << "disabled " << endpoint.name.toStdString(); };
    system.setControllerHooksForTesting(hooks);
    auto rows = system.controllers();
    ASSERT_EQ(1, rows.size());
    EXPECT_TRUE(rows[0].hid);
    EXPECT_EQ(QStringLiteral("Generated HID Deck _4"), rows[0].mixxxName);
    EXPECT_EQ(QStringLiteral("Generated HID Deck"), rows[0].name);
    EXPECT_EQ(QStringLiteral("17cc:1720"), rows[0].key);
    EXPECT_TRUE(rows[0].suppressed.isEmpty());
    EXPECT_TRUE(system.closeStrayInterfaces().isEmpty());
    QString error;
    EXPECT_TRUE(system.applyMapping(rows[0].mixxxName,
            mappingPath(config(), QStringLiteral("Traktor Kontrol S4 MK3.hid.xml")), &error))
            << error.toStdString();
    EXPECT_EQ(QStringList{QStringLiteral("Generated HID Deck _4")}, opened);

    // A HID interface with a mapping made for it is used rather than an
    // unconfigured MIDI port of the same device.
    const QString shared = QStringLiteral("usb-0000:01:00.0-1.4");
    const auto withMapping = decklab::groupControllers(
            {midiPort(QStringLiteral("Generated Deck MIDI 1"), shared),
                    hidInterface(QStringLiteral("Generated Deck"), 0xFF00, 0, shared, 0x08e4, 0x0001)},
            {},
            [](const decklab::ControllerEndpoint& endpoint) { return endpoint.hid; });
    ASSERT_EQ(1, withMapping.size());
    ASSERT_NE(nullptr, withMapping[0].primaryEndpoint());
    EXPECT_TRUE(withMapping[0].primaryEndpoint()->hid);
    EXPECT_TRUE(withMapping[0].suppressed.isEmpty());

    // Two identical MixTrack Pro 3s: two rows, each its own MIDI port in use
    // and its own HID interface closed, matched to decklabd by USB port.
    const QString left = QStringLiteral("usb-0000:01:00.0-1.2");
    const QString right = QStringLiteral("usb-0000:01:00.0-1.4");
    const QList<decklab::ControllerEndpoint> twins{midiPort(QStringLiteral("MixTrack Pro 3 MIDI 1"), right),
            midiPort(QStringLiteral("MixTrack Pro 3 MIDI 1"), left),
            hidInterface(QStringLiteral("MixTrack Pro 3"), 0xFFA0, 3, left),
            hidInterface(QStringLiteral("MixTrack Pro 3"), 0xFFA0, 3, right)};
    const auto reported = decklab::reportedControllers(QJsonObject{{"controllers",
            QJsonArray{reportedMixTrack(QStringLiteral("15e4:0035"), QStringLiteral("1-1.2")),
                    reportedMixTrack(QStringLiteral("15e4:0035:1-1.4"), QStringLiteral("1-1.4"))}}});
    const auto groups = decklab::groupControllers(twins, reported);
    ASSERT_EQ(2, groups.size());
    for (int index = 0; index < 2; ++index) {
        const auto& group = groups.at(index);
        EXPECT_EQ(index, group.reported);
        ASSERT_EQ(2, group.endpoints.size());
        ASSERT_NE(nullptr, group.primaryEndpoint());
        EXPECT_FALSE(group.primaryEndpoint()->hid);
        EXPECT_EQ(index == 0 ? left : right, group.primaryEndpoint()->usbPath);
        ASSERT_EQ(1, group.suppressed.size());
        EXPECT_EQ(group.primaryEndpoint()->usbPath, group.endpoints.at(group.suppressed.first()).usbPath);
    }
    // Without decklabd: still two.
    EXPECT_EQ(2, decklab::groupControllers(twins, {}).size());
}

TEST_F(DeckLabSystemTest, UsbLocationsComeFromHidrawAndTheAlsaCard) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    writeFile(root.filePath(QStringLiteral("sys/class/hidraw/hidraw0/device/uevent")),
            "DRIVER=hid-generic\nHID_ID=0003:000015E4:00000035\nHID_NAME=Numark MixTrack Pro 3\n"
            "HID_PHYS=usb-0000:01:00.0-1.2/input3\nHID_UNIQ=\n");
    writeFile(root.filePath(QStringLiteral("proc/asound/cards")),
            " 0 [Headphones     ]: bcm2835_headpho - bcm2835 Headphones\n"
            "                      bcm2835 Headphones\n"
            " 1 [M3             ]: USB-Audio - MixTrack Pro 3\n"
            "                      Numark MixTrack Pro 3 at usb-0000:01:00.0-1.2, full speed\n");
    writeFile(root.filePath(QStringLiteral("proc/asound/seq/clients")),
            "Client info\n  cur  clients : 4\n\n"
            "Client   0 : \"System\" [Kernel Legacy]\n  Port   0 : \"Timer\" (Rwe-) [In/Out]\n"
            "Client  14 : \"Midi Through\" [Kernel Legacy]\n  Port   0 : \"Midi Through Port-0\" (RWe-) [In/Out]\n"
            "Client  20 : \"MixTrack Pro 3\" [Kernel Legacy]\n  Port   0 : \"MixTrack Pro 3 MIDI 1\" (RWeX) [In/Out]\n"
            "Client 128 : \"Generated Client\" [User Legacy]\n  Port   0 : \"port-0\" (Rwe-) [In/Out]\n");
    EXPECT_EQ(QStringLiteral("usb-0000:01:00.0-1.2"),
            decklab::usbpath::hidrawLocation(QStringLiteral("/dev/hidraw0"), root.path()));
    EXPECT_TRUE(decklab::usbpath::hidrawLocation(QStringLiteral("/dev/hidraw7"), root.path()).isEmpty());
    const auto ports = decklab::usbpath::midiPortLocations(root.path());
    EXPECT_EQ(QStringLiteral("usb-0000:01:00.0-1.2"), ports.value(QStringLiteral("MixTrack Pro 3 MIDI 1")));
    EXPECT_FALSE(ports.contains(QStringLiteral("Midi Through Port-0"))); // not a USB card
    EXPECT_FALSE(ports.contains(QStringLiteral("port-0")));
    EXPECT_EQ(QStringLiteral("1.2"), decklab::usbPortChain(QStringLiteral("usb-0000:01:00.0-1.2/input3")));
    EXPECT_EQ(QStringLiteral("1.2"), decklab::usbPortChain(QStringLiteral("1-1.2")));
    EXPECT_EQ(QStringLiteral("1.4.2"), decklab::usbPortChain(QStringLiteral("usb-3f980000.usb-1.4.2")));
    EXPECT_TRUE(decklab::usbPortChain(QString()).isEmpty());
}

// Issue: "Set up later" did not stick; the Welcome flow came back at every start.
TEST_F(DeckLabSystemTest, SetUpLaterSticksAndSettingsOffersFinishSetup) {
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    using Kind = decklab::AudioDevice::Kind;
    system.setAudioDevicesOverride({audioDevice(QStringLiteral("Controller: USB Audio"), 4, Kind::External, true)});
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    EXPECT_TRUE(system.firstRunDue());
    QWidget host;
    host.setStyleSheet(QStringLiteral("QPushButton {min-height:28px;padding:5px 9px;} QComboBox {min-height:38px;}"));
    host.resize(800, 480);
    host.show();
    DeckLabSystemUi ui(&host, DeckLabSystemUi::Hooks{});
    QWidget* panel = ui.createSetupPanel(&host);
    ui.attach(&system);
    // The first run opens by itself while it is due.
    ASSERT_TRUE(QTest::qWaitFor([&] { return ui.wizard() != nullptr; }, 4000));
    QPointer<DeckLabSetupWizard> wizard = ui.wizard();
    EXPECT_TRUE(wizard->isFirstRun());
    auto* later = wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-close"));
    ASSERT_NE(nullptr, later);
    EXPECT_EQ(QStringLiteral("Set up later"), later->text());
    later->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(wizard.isNull());
    EXPECT_TRUE(system.firstRunDeferred());
    EXPECT_FALSE(system.firstRunDone());
    EXPECT_FALSE(system.firstRunDue());
    EXPECT_EQ(1, config()->getValue(ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("first_run_deferred")), 0));
    // The next start: no Welcome.
    {
        DeckLabSystem again(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
        QWidget other;
        other.resize(800, 480);
        DeckLabSystemUi nextStart(&other, DeckLabSystemUi::Hooks{});
        nextStart.attach(&again);
        QTest::qWait(1900);
        EXPECT_EQ(nullptr, nextStart.wizard());
    }
    // Settings › Controller & audio offers Finish setup; so does the status sheet.
    auto* finish = panel->findChild<QPushButton*>(QStringLiteral("decklab-finish-setup"));
    ASSERT_NE(nullptr, finish);
    EXPECT_TRUE(finish->isVisibleTo(panel));
    decklab::rail::Inputs inputs;
    ui.fillRailInputs(inputs);
    EXPECT_TRUE(inputs.setupPending);
    int finishes = 0;
    DeckLabStatusSheet::Actions actions;
    actions.finishSetup = [&finishes] { ++finishes; };
    auto* sheet = new DeckLabStatusSheet(&host, actions);
    sheet->refresh(inputs);
    sheet->show();
    ASSERT_FALSE(sheet->lines().isEmpty());
    EXPECT_TRUE(sheet->lines().first().startsWith(QStringLiteral("Setup: Not finished"))) << sheet->lines().first().toStdString();
    auto* sheetFinish = sheet->findChild<QPushButton*>(QStringLiteral("decklab-status-finish-setup"));
    ASSERT_NE(nullptr, sheetFinish);
    ASSERT_TRUE(sheetFinish->isVisibleTo(sheet));
    sheetFinish->click();
    EXPECT_EQ(1, finishes);
    // Finish setup opens the first run; finishing it completes setup.
    finish->click();
    wizard = ui.wizard();
    ASSERT_FALSE(wizard.isNull());
    EXPECT_TRUE(wizard->isFirstRun());
    wizard->skipToDone();
    wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-next"))->click();
    EXPECT_TRUE(system.firstRunDone());
    EXPECT_FALSE(system.firstRunDeferred());
    QCoreApplication::processEvents();
    EXPECT_FALSE(finish->isVisibleTo(panel));
    ui.fillRailInputs(inputs);
    EXPECT_FALSE(inputs.setupPending);
    sheet->refresh(inputs);
    for (const QString& line : sheet->lines()) {
        EXPECT_FALSE(line.startsWith(QStringLiteral("Setup:"))) << line.toStdString();
    }
}

// Issue: "CONTRO…" in the step bar at 800x480.
TEST_F(DeckLabSystemTest, EveryDeckSetupStepNameFitsAt800) {
    deckLabLoadFonts(config()->getResourcePath());
    ASSERT_TRUE(QFontDatabase::families().contains(QStringLiteral("Barlow")));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    QWidget host;
    host.setStyleSheet(QStringLiteral("QPushButton {min-height:28px;padding:5px 9px;} QComboBox {min-height:38px;}"));
    host.resize(800, 480);
    host.show();
    auto* wizard = new DeckLabSetupWizard(&system, &host);
    for (const bool firstRun : {true, false}) {
        if (firstRun) {
            wizard->startFirstRun();
        } else {
            wizard->start();
        }
        QTest::qWait(30);
        auto* progress = dynamic_cast<DeckLabProgressLine*>(
                wizard->findChild<QWidget*>(QStringLiteral("decklab-setup-progress")));
        ASSERT_NE(nullptr, progress);
        QStringList expected;
        for (const QString& step : progress->steps()) {
            expected.append(step.toUpper());
        }
        EXPECT_EQ(firstRun ? 7 : 4, expected.size());
        EXPECT_TRUE(expected.contains(QStringLiteral("CONTROLLER")));
        EXPECT_EQ(expected, progress->shownSteps());
        // Brand type: 14 px Barlow caps; each label inside its own cell.
        const QFont font = progress->stepFont();
        EXPECT_EQ(14, QFontInfo(font).pixelSize());
        EXPECT_TRUE(QFontInfo(font).family().startsWith(QStringLiteral("Barlow"))) << QFontInfo(font).family().toStdString();
        EXPECT_EQ(QFont::AllUppercase, font.capitalization());
        const QFontMetricsF metrics(font);
        const auto cells = progress->stepCells();
        ASSERT_EQ(expected.size(), cells.size());
        for (int index = 0; index < cells.size(); ++index) {
            EXPECT_LE(metrics.horizontalAdvance(expected.at(index)), cells.at(index).width()) << expected.at(index).toStdString();
            EXPECT_LE(cells.at(index).right(), progress->width() + 0.5);
        }
        captureWidget(&host, firstRun ? QStringLiteral("setup-step-bar-first-run.png") : QStringLiteral("setup-step-bar.png"));
    }
}

// Issue: Browse showed an unlabeled stick as its filesystem UUID.
TEST_F(DeckLabSystemTest, UsbSticksHaveFriendlyNamesNeverTheirUuid) {
    using decklab::UsbNameSource;
    const auto source = [](const QString& id, const QString& label, bool rekordbox, double gigabytes = -1) {
        UsbNameSource result;
        result.id = id;
        result.label = label;
        result.rekordbox = rekordbox;
        result.bytes = gigabytes > 0 ? static_cast<qint64>(gigabytes * 1e9) : -1;
        return result;
    };
    // decklabd sends the UUID as the label when there is none.
    EXPECT_EQ(QStringLiteral("DJ USB"), decklab::usbBaseName(source("3A1F-0C2D", "3A1F-0C2D", true)));
    EXPECT_EQ(QStringLiteral("USB stick"), decklab::usbBaseName(source("3A1F-0C2D", "", false)));
    EXPECT_EQ(QStringLiteral("MIX SETS"), decklab::usbBaseName(source("3A1F-0C2D", "MIX SETS", false)));
    for (const char* uuid : {"6A3F-1C2B", "0123456789ABCDEF", "0e9c1d7a-55aa-4b33-9e3f-2f9d0c4a1b77"}) {
        EXPECT_TRUE(decklab::usbLabelIsPlaceholder(QString::fromLatin1(uuid))) << uuid;
    }
    EXPECT_FALSE(decklab::usbLabelIsPlaceholder(QStringLiteral("DJ USB")));
    // The size it was sold as.
    EXPECT_EQ(QStringLiteral("32 GB"), decklab::usbCapacityText(static_cast<qint64>(29.6e9)));
    EXPECT_EQ(QStringLiteral("32 GB"), decklab::usbCapacityText(static_cast<qint64>(32.4e9)));
    EXPECT_EQ(QStringLiteral("16 GB"), decklab::usbCapacityText(static_cast<qint64>(15.4e9)));
    EXPECT_EQ(QStringLiteral("1 TB"), decklab::usbCapacityText(static_cast<qint64>(1.0e12)));
    EXPECT_TRUE(decklab::usbCapacityText(-1).isEmpty());
    // Several sticks: different kinds need nothing more; the same name: the
    // first keeps it, a later one gets its size, or a number when the sizes
    // match. Never a UUID.
    {
        decklab::UsbNameBook book;
        EXPECT_EQ((QStringList{"DJ USB", "USB stick"}),
                book.names({source("3A1F-0C2D", "", true, 31), source("55AA-1234", "", false, 31)}));
    }
    {
        decklab::UsbNameBook book;
        EXPECT_EQ((QStringList{"DJ USB", "DJ USB 16 GB"}),
                book.names({source("3A1F-0C2D", "", true, 31), source("55AA-1234", "", true, 15.5)}));
    }
    {
        // A name shown stays while the stick is in: A alone is "DJ USB"; B
        // arriving (its UUID sorts first) is "DJ USB 2", A is not renamed;
        // A leaving does not rename B; A back gets its name again.
        decklab::UsbNameBook book;
        const auto a = source("55AA-1234", "", true, 31);
        const auto b = source("0B7E-4C11", "", true, 31);
        EXPECT_EQ(QStringList{"DJ USB"}, book.names({a}));
        EXPECT_EQ((QStringList{"DJ USB", "DJ USB 2"}), book.names({a, b}));
        EXPECT_EQ((QStringList{"DJ USB 2", "DJ USB"}), book.names({b, a})); // any order
        EXPECT_EQ(QStringList{"DJ USB 2"}, book.names({b}));
        EXPECT_EQ((QStringList{"DJ USB", "DJ USB 2"}), book.names({a, b}));
        // Without a remembered name, the first by UUID keeps the plain one.
        decklab::UsbNameBook fresh;
        EXPECT_EQ((QStringList{"DJ USB 2", "DJ USB"}), fresh.names({a, b}));
    }
    decklab::usbNameBook().clear();
    // The rail and Storage name decklabd's drives the same way.
    const QJsonArray drives{QJsonObject{{"id", "3A1F-0C2D"}, {"label", "3A1F-0C2D"}, {"mount", "/media/decklab/3A1F-0C2D"},
                                    {"size_mb", 30528}, {"free_mb", 12402}, {"fs", "exfat"}, {"read_only", true},
                                    {"rekordbox", true}, {"busy", false}},
            QJsonObject{{"id", "0e9c1d7a-55aa-4b33-9e3f-2f9d0c4a1b77"}, {"label", "MIX SETS"},
                    {"mount", "/media/decklab/0e9c1d7a-55aa-4b33-9e3f-2f9d0c4a1b77"}, {"size_mb", 61056},
                    {"free_mb", 40220}, {"fs", "ext4"}, {"read_only", false}, {"rekordbox", false}, {"busy", false}}};
    EXPECT_EQ(QStringLiteral("DJ USB"), decklab::usbDriveName(drives.at(0).toObject(), drives));
    DeckLabFakeService fake;
    fake.drives = drives;
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    QWidget host;
    DeckLabSystemUi ui(&host, DeckLabSystemUi::Hooks{});
    ui.attach(&system);
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.usbDrives().size() == 2; }, 3000));
    QCoreApplication::processEvents();
    decklab::rail::Inputs inputs;
    ui.fillRailInputs(inputs);
    EXPECT_EQ(QStringLiteral("DJ USB (read-only) · MIX SETS"), inputs.usbSummary);
    EXPECT_FALSE(inputs.usbSummary.contains(QStringLiteral("3A1F")));
    EXPECT_EQ(QStringLiteral("USB 2"), decklab::rail::usbView(inputs).label);
    EXPECT_TRUE(system.ejectRefusal(drives.at(0).toObject()).isEmpty());
    // A track playing from the unlabeled stick: the refusal names it.
    system.setLoadedTracksProvider([] {
        return QList<decklab::LoadedTrack>{{QStringLiteral("[Channel1]"),
                QStringLiteral("/media/decklab/3A1F-0C2D/Contents/Generated Track.mp3")}};
    });
    const QString refusal = system.ejectRefusal(drives.at(0).toObject());
    EXPECT_TRUE(refusal.contains(QStringLiteral("DJ USB"))) << refusal.toStdString();
    EXPECT_FALSE(refusal.contains(QStringLiteral("3A1F"))) << refusal.toStdString();
    // Settings › Unit › Storage: its card and the home tile.
    host.resize(800, 480);
    auto* page = new DeckLabSystemPage(&host);
    page->resize(800, 480);
    page->setSystem(&system);
    host.show();
    page->showSection(DeckLabSystemPage::Section::Storage);
    QTest::qWait(50);
    QStringList cards;
    for (auto* row : page->findChildren<decklab::ui::Row*>(QStringLiteral("decklab-drive-row"))) {
        cards.append(row->titleText());
    }
    EXPECT_EQ((QStringList{"DJ USB", "MIX SETS"}), cards);
    page->showSection(DeckLabSystemPage::Section::Home);
    QTest::qWait(50);
    auto* tile = dynamic_cast<DeckLabLiveTile*>(page->tile(DeckLabSystemPage::Section::Storage));
    ASSERT_NE(nullptr, tile);
    QStringList bars;
    for (const auto& bar : tile->bars()) {
        bars.append(bar.name);
    }
    EXPECT_EQ((QStringList{"DJ USB", "MIX SETS"}), bars);
}

// The HID repair through its real triggers: Deck Lab starting after Mixxx
// opened the armed interface, and a rescan (Detect again). The fake records
// only, as the real disable is queued on the controller thread.
TEST_F(DeckLabSystemTest, StrayHidInterfaceIsClosedAtStartAndAfterARescan) {
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    const QString midiMapping = mappingPath(config(), QStringLiteral("Numark-Mixtrack-3.midi.xml"));
    const QString location = QStringLiteral("usb-0000:01:00.0-1.3");
    auto hid = hidInterface(QStringLiteral("MixTrack Pro 3"), 0xFFA0, 3, location);
    hid.devicePath = QStringLiteral("/dev/hidraw4");
    // As an earlier build left it: enabled with the MIDI mapping (closed: armed).
    hid.enabled = true;
    hid.mapping = midiMapping;
    QList<decklab::ControllerEndpoint> endpoints{
            midiPort(QStringLiteral("MixTrack Pro 3 MIDI 1"), location, midiMapping, true), hid};
    QStringList disabled;
    int rescans = 0;
    DeckLabSystem::ControllerHooks hooks;
    hooks.endpoints = [&endpoints] { return endpoints; };
    hooks.open = [](const QString& name, const QString&, QString*) {
        ADD_FAILURE() << "opened " << name.toStdString();
        return false;
    };
    hooks.disable = [&disabled](const decklab::ControllerEndpoint& endpoint) {
        disabled.append(endpoint.name + QLatin1Char('@') + endpoint.devicePath);
    };
    hooks.rescan = [&rescans] { ++rescans; };
    system.setControllerHooksForTesting(hooks);
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !disabled.isEmpty(); }, 2000));
    EXPECT_EQ(QStringList{QStringLiteral("MixTrack Pro 3 _3@/dev/hidraw4")}, disabled);
    // Open after a rescan (Mixxx reopens what is enabled): Detect again closes it.
    disabled.clear();
    endpoints[1].open = true;
    QWidget host;
    host.resize(800, 480);
    host.show();
    auto* wizard = new DeckLabSetupWizard(&system, &host);
    wizard->start();
    QTest::qWait(20);
    QSignalSpy rescanned(&system, &DeckLabSystem::controllersRescanned);
    auto* detect = wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-detect"));
    ASSERT_NE(nullptr, detect);
    detect->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return rescanned.count() == 1; }, 2000));
    EXPECT_EQ(1, rescans);
    EXPECT_EQ(QStringList{QStringLiteral("MixTrack Pro 3 _3@/dev/hidraw4")}, disabled);
    // Neither closed nor armed: nothing to do.
    disabled.clear();
    endpoints[1].open = false;
    endpoints[1].enabled = false;
    endpoints[1].mapping.clear();
    detect->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return rescanned.count() == 2; }, 2000));
    EXPECT_TRUE(disabled.isEmpty()) << disabled.join(QStringLiteral(", ")).toStdString();
}

// Two identical MixTrack Pro 3s as Linux and Mixxx really report them: the
// MIDI ports share one name, so /proc can't place them (no location), the
// HID interfaces share a name too but have their own hidraw nodes.
TEST_F(DeckLabSystemTest, TwinControllersAsLinuxReportThemAreTwoRowsAndBothHidClose) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    writeFile(root.filePath(QStringLiteral("sys/class/hidraw/hidraw0/device/uevent")),
            "HID_ID=0003:000015E4:00000035\nHID_NAME=Numark MixTrack Pro 3\nHID_PHYS=usb-0000:01:00.0-1.2/input3\n");
    writeFile(root.filePath(QStringLiteral("sys/class/hidraw/hidraw1/device/uevent")),
            "HID_ID=0003:000015E4:00000035\nHID_NAME=Numark MixTrack Pro 3\nHID_PHYS=usb-0000:01:00.0-1.4/input3\n");
    writeFile(root.filePath(QStringLiteral("proc/asound/cards")),
            " 1 [M3             ]: USB-Audio - MixTrack Pro 3\n"
            "                      Numark MixTrack Pro 3 at usb-0000:01:00.0-1.2, full speed\n"
            " 2 [M3_1           ]: USB-Audio - MixTrack Pro 3\n"
            "                      Numark MixTrack Pro 3 at usb-0000:01:00.0-1.4, full speed\n");
    writeFile(root.filePath(QStringLiteral("proc/asound/seq/clients")),
            "Client  20 : \"MixTrack Pro 3\" [Kernel Legacy]\n  Port   0 : \"MixTrack Pro 3 MIDI 1\" (RWeX) [In/Out]\n"
            "Client  24 : \"MixTrack Pro 3\" [Kernel Legacy]\n  Port   0 : \"MixTrack Pro 3 MIDI 1\" (RWeX) [In/Out]\n");
    const auto ports = decklab::usbpath::midiPortLocations(root.path());
    EXPECT_FALSE(ports.contains(QStringLiteral("MixTrack Pro 3 MIDI 1"))); // two cards share it
    const QString midiMapping = mappingPath(config(), QStringLiteral("Numark-Mixtrack-3.midi.xml"));
    QList<decklab::ControllerEndpoint> endpoints;
    for (int unit = 0; unit < 2; ++unit) {
        endpoints.append(midiPort(QStringLiteral("MixTrack Pro 3 MIDI 1"),
                ports.value(QStringLiteral("MixTrack Pro 3 MIDI 1")), midiMapping, true));
    }
    for (int unit = 0; unit < 2; ++unit) {
        const QString node = QStringLiteral("/dev/hidraw%1").arg(unit);
        auto hid = hidInterface(QStringLiteral("MixTrack Pro 3"), 0xFFA0, 3,
                decklab::usbpath::hidrawLocation(node, root.path()));
        hid.devicePath = node;
        hid.open = true; // both opened by Mixxx at start (they share one config key)
        hid.enabled = true;
        hid.mapping = midiMapping;
        endpoints.append(hid);
    }
    EXPECT_EQ(QStringLiteral("usb-0000:01:00.0-1.2"), endpoints[2].usbPath);
    EXPECT_EQ(QStringLiteral("usb-0000:01:00.0-1.4"), endpoints[3].usbPath);
    DeckLabFakeService fake;
    fake.devices = QJsonObject{{"controllers",
                                       QJsonArray{reportedMixTrack(QStringLiteral("15e4:0035"), QStringLiteral("1-1.2")),
                                               reportedMixTrack(QStringLiteral("15e4:0035:1-1.4"), QStringLiteral("1-1.4"))}},
            {"audio_cards", QJsonArray{}}};
    ASSERT_TRUE(fake.listen(socketPath()));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketPath());
    QStringList disabled;
    DeckLabSystem::ControllerHooks hooks;
    hooks.endpoints = [&endpoints] { return endpoints; };
    hooks.disable = [&disabled](const decklab::ControllerEndpoint& endpoint) { disabled.append(endpoint.devicePath); };
    system.setControllerHooksForTesting(hooks);
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady() && !system.devices().isEmpty(); }, 3000));
    const auto rows = system.controllers();
    ASSERT_EQ(2, rows.size());
    EXPECT_EQ(QStringLiteral("15e4:0035"), rows[0].key);
    EXPECT_EQ(QStringLiteral("15e4:0035:1-1.4"), rows[1].key);
    for (const auto& row : rows) {
        EXPECT_FALSE(row.hid);
        EXPECT_EQ(QStringLiteral("MixTrack Pro 3 MIDI 1"), row.mixxxName);
        EXPECT_EQ(QStringList{QStringLiteral("MixTrack Pro 3 _3")}, row.suppressed);
    }
    // decklabd's devices claimed the right HID interface each (by USB port).
    const auto groups = decklab::groupControllers(endpoints, decklab::reportedControllers(system.devices()));
    ASSERT_EQ(2, groups.size());
    EXPECT_EQ(QStringLiteral("/dev/hidraw0"), groups[0].endpoints.at(groups[0].suppressed.first()).devicePath);
    EXPECT_EQ(QStringLiteral("/dev/hidraw1"), groups[1].endpoints.at(groups[1].suppressed.first()).devicePath);
    // Both HID interfaces are closed, each by its own device path.
    ASSERT_TRUE(QTest::qWaitFor([&] { return disabled.size() >= 2; }, 2000));
    disabled.sort();
    EXPECT_EQ((QStringList{"/dev/hidraw0", "/dev/hidraw1"}), disabled.mid(0, 2));
}

// Finishing Deck Setup from Settings (not the first run) completes setup.
TEST_F(DeckLabSystemTest, FinishingANormalDeckSetupCompletesSetup) {
    config()->setValue(ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("first_run_deferred")), 1);
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    QWidget host;
    host.resize(800, 480);
    host.show();
    DeckLabSystemUi ui(&host, DeckLabSystemUi::Hooks{});
    QWidget* panel = ui.createSetupPanel(&host);
    ui.attach(&system);
    auto* finish = panel->findChild<QPushButton*>(QStringLiteral("decklab-finish-setup"));
    ASSERT_NE(nullptr, finish);
    EXPECT_TRUE(finish->isVisibleTo(panel));
    QPointer<DeckLabSetupWizard> wizard = ui.openWizard(0);
    ASSERT_FALSE(wizard.isNull());
    EXPECT_FALSE(wizard->isFirstRun());
    wizard->skipToDone();
    wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-next"))->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(system.firstRunDone());
    EXPECT_FALSE(system.firstRunDeferred());
    EXPECT_FALSE(finish->isVisibleTo(panel));
}

// Upgrade: a unit already set up (a remembered Deck Setup) never saw Play in
// the first run. It counts as set up: no Welcome, no "Not finished".
TEST_F(DeckLabSystemTest, AUnitAlreadySetUpSkipsWelcomeAfterTheUpdate) {
    decklab::DeviceProfile profile;
    profile.key = QStringLiteral("15e4:0035");
    profile.name = QStringLiteral("Numark MixTrack Pro 3");
    profile.controller = QStringLiteral("MixTrack Pro 3 MIDI 1");
    profile.mapping = mappingPath(config(), QStringLiteral("Numark-Mixtrack-3.midi.xml"));
    profile.mainDevice = QStringLiteral("MixTrack Pro 3: USB Audio");
    profile.mainBase = 0;
    decklab::DeviceStore(config()).save(profile);
    EXPECT_EQ(0, config()->getValue(ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("first_run_done")), 0));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    EXPECT_TRUE(system.firstRunDone());
    EXPECT_FALSE(system.firstRunDue());
    QWidget host;
    host.resize(800, 480);
    DeckLabSystemUi ui(&host, DeckLabSystemUi::Hooks{});
    QWidget* panel = ui.createSetupPanel(&host);
    ui.attach(&system);
    QTest::qWait(1800);
    EXPECT_EQ(nullptr, ui.wizard());
    auto* finish = panel->findChild<QPushButton*>(QStringLiteral("decklab-finish-setup"));
    ASSERT_NE(nullptr, finish);
    EXPECT_FALSE(finish->isVisibleTo(panel));
    // A unit that was never set up still gets the Welcome.
    {
        DeckLabSystem::Services fresh;
        fresh.config = config();
        decklab::DeviceStore(config()).remove(profile.key);
        config()->remove(ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("first_run_done")));
        DeckLabSystem untouched(fresh, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
        EXPECT_TRUE(untouched.firstRunDue());
    }
}
