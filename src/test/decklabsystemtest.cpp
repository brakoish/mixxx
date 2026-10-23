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
#include "decklab/decklabsetupmodel.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsignals.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabsystempages.h"
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
