// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab system integration: decklabd client protocol, controller mapping
// matching, Deck Setup policy and persistence, eject safety, System pages and
// the first-run flow. Only generated fixtures and bundled mappings are used:
// no hardware, host services, personal media or network.
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
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
#include "decklab/decklabsetupmodel.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsignals.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabsystempages.h"
#include "decklab/decklabui.h"
#include "test/mixxxtest.h"

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
