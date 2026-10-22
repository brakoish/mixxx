// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabsmoke.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <memory>
#include "control/controlobject.h"
#include "decklab/decklabdclient.h"
#include "decklab/decklabfakeservice.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabsystempages.h"
#include "decklab/decklabsystemui.h"
#include "util/cmdlineargs.h"
#include "widget/wdecklab.h"
#include "widget/woverview.h"

namespace {
// Deck Lab system pages, the first-run flow and banners, captured against a
// canned decklabd on a private socket so the pages show real content. The
// host's own decklabd and devices are never touched.
constexpr int kSystemFirst = 21;
constexpr int kSystemSteps = 22;
constexpr int kLargeFirst = kSystemFirst + 20; // 1024x600 captures

void clickVisible(QWidget* root, const QString& name, const QString& text = QString()) {
    for (auto* button : root->findChildren<QPushButton*>(name)) {
        if (button->isVisibleTo(root) && (text.isEmpty() || button->text() == text)) {
            button->click();
            return;
        }
    }
}

// Every control on the system pages and in Deck Setup is a 44 px+ target in
// the real surface style, not just in unit tests.
void checkSystemTouchTargets(QWidget* surface, QJsonArray& failures) {
    for (const auto& name : {QStringLiteral("decklab-system-page"), QStringLiteral("decklab-setup-wizard")}) {
        for (auto* root : surface->findChildren<QWidget*>(name)) {
            if (!root->isVisible()) {
                continue;
            }
            for (auto* button : root->findChildren<QPushButton*>()) {
                if (button->isVisible() && button->height() < 44) {
                    failures.append(QStringLiteral("Small touch target %1 (%2 px)")
                                            .arg(button->objectName().isEmpty() ? button->text() : button->objectName())
                                            .arg(button->height()));
                }
            }
        }
    }
}

void systemSmokeStep(int index, WDeckLab* surface) {
    using Section = DeckLabSystemPage::Section;
    using Step = DeckLabSetupWizard::Step;
    auto* system = surface->window()->findChild<DeckLabSystem*>();
    auto* ui = surface->findChild<DeckLabSystemUi*>();
    if (index == 0 && system && !surface->findChild<DeckLabFakeService*>()) {
        auto* fake = new DeckLabFakeService(surface);
        const QString path = QDir(QDir::tempPath()).filePath(
                QStringLiteral("decklab-smoke-%1/decklabd.sock").arg(QCoreApplication::applicationPid()));
        fake->listen(path);
        system->client()->setSocketPath(path);
        using Kind = decklab::AudioDevice::Kind;
        const auto device = [](const QString& id, const QString& name, int outputs, Kind kind, bool controller) {
            decklab::AudioDevice entry;
            entry.id = id;
            entry.name = name;
            entry.outputs = outputs;
            entry.kind = kind;
            entry.controllerCard = controller;
            return entry;
        };
        system->setAudioDevicesOverride({device(QStringLiteral("DDJ-FLX4: USB Audio"), QStringLiteral("DDJ-FLX4"), 4, Kind::External, true),
                device(QStringLiteral("bcm2835 Headphones: -"), QStringLiteral("Deck Lab headphone jack"), 2, Kind::BuiltinJack, false),
                device(QStringLiteral("vc4-hdmi-0: MAI PCM i2s-hifi-0"), QStringLiteral("HDMI 1"), 2, Kind::Hdmi, false),
                device(QStringLiteral("pipewire"), QStringLiteral("System audio (Bluetooth)"), 2, Kind::Network, false)});
    }
    auto* page = surface->findChild<DeckLabSystemPage*>(QStringLiteral("decklab-system-page"));
    const auto settings = [surface, page](Section section) {
        surface->selectPage(4);
        if (auto* category = surface->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-5"))) {
            category->click();
        }
        if (page) {
            page->showSection(section);
        }
    };
    auto* wizard = ui ? ui->wizard() : nullptr;
    switch (index) {
    case 0: settings(Section::Home); break;
    case 1: settings(Section::Wifi); break;
    case 2:
        settings(Section::Wifi);
        clickVisible(surface, QStringLiteral("decklab-wifi-network"), QStringLiteral("Example-Network"));
        break;
    case 3: settings(Section::Bluetooth); break;
    case 4: settings(Section::Screen); break;
    case 5: settings(Section::Time); break;
    case 6:
        settings(Section::Time);
        clickVisible(surface, QStringLiteral("decklab-time-zone"));
        break;
    case 7: settings(Section::Storage); break;
    case 8: settings(Section::Power); break;
    case 9: settings(Section::About); break;
    case 10: settings(Section::Maintenance); break;
    case 11:
        surface->selectPage(0);
        if (ui) {
            ui->openFirstRun();
        }
        break;
    case 12:
    case 13:
    case 14:
        clickVisible(surface, QStringLiteral("decklab-setup-next"));
        break;
    case 15:
        if (wizard) {
            wizard->showMappingPicker();
        }
        break;
    case 16:
        if (wizard) {
            wizard->start(Step::Audio);
        }
        break;
    case 17:
        if (wizard) {
            wizard->start(Step::Layout);
        }
        break;
    case 18:
        if (wizard) {
            wizard->start(Step::Done);
        }
        break;
    case 19:
        clickVisible(surface, QStringLiteral("decklab-setup-close"));
        surface->selectPage(0);
        if (auto* fake = surface->findChild<DeckLabFakeService*>()) {
            fake->pushEvent(QStringLiteral("thermal.changed"),
                    QJsonObject{{QStringLiteral("temp_c"), 81.2}, {QStringLiteral("throttled"), QStringLiteral("0x20002")},
                            {QStringLiteral("undervoltage_now"), false}, {QStringLiteral("level"), QStringLiteral("hot")}});
        }
        break;
    case 20: settings(Section::Home); break;
    case 21:
        if (ui) {
            ui->openFirstRun();
        }
        break;
    default: break;
    }
}
} // namespace

void startDeckLabSmoke(WDeckLab* surface, UserSettingsPointer config) {
    const QString output = qEnvironmentVariable("DECKLAB_SMOKE_DIR");
    if (output.isEmpty() || !surface || !CmdlineArgs::Instance().getDeveloper()) return;
    // Do not turn a forgotten test environment variable into automation on a
    // personal profile. The build recipe creates this separate scratch profile.
    if (!QDir(config->getSettingsPath()).dirName().startsWith("decklab-native-smoke")) {
        qCritical("Refusing smoke check outside its dedicated profile"); qApp->exit(70); return;
    }
    auto* timer = new QTimer(surface);
    auto step = std::make_shared<int>(0);
    auto failures = std::make_shared<QJsonArray>();
    const auto check = [surface, failures] {
        if (surface->size() != QSize(800, 480)) failures->append("Surface is not 800x480");
        for (int deck = 1; deck <= 4; ++deck) {
            auto* overview = surface->findChild<WOverview*>(QStringLiteral("decklab-overview-%1").arg(deck));
            bool positionBound = false;
            // WBaseWidget owns these via unique_ptr, not the QObject child tree.
            // Its developer tooltip reports the actual display connection.
            if (overview) {
                QHelpEvent help(QEvent::ToolTip, QPoint(), overview->mapToGlobal(QPoint()));
                QCoreApplication::sendEvent(overview, &help); QToolTip::hideText();
                const QString prefix = QStringLiteral("DisplayConnection: [Channel%1],playposition ").arg(deck);
                for (const QString& line : overview->toolTip().split(QChar('\n')))
                    if (line.startsWith(prefix) && line.contains("Direction: TO_WIDGET")) positionBound = true;
            }
            if (!positionBound) failures->append(QStringLiteral("Deck %1 overview missing read-only playposition binding").arg(deck));
        }
        for (auto* b : surface->findChildren<QPushButton*>()) {
            if (!b->isVisible()) continue;
            QScrollArea* scroll = nullptr;
            for (auto* parent = b->parentWidget(); parent; parent = parent->parentWidget())
                if ((scroll = qobject_cast<QScrollArea*>(parent))) break;
            if (scroll) {
                scroll->ensureWidgetVisible(b, 0, 0); QCoreApplication::processEvents();
                if (!scroll->viewport()->rect().contains(QRect(b->mapTo(scroll->viewport(), QPoint()), b->size())))
                    failures->append(QString(QStringLiteral("Unreachable scroll control: ") + b->text()));
            }
            const QRect rect(b->mapTo(surface, QPoint()), b->size());
            if (!surface->rect().contains(rect)) failures->append(QString(QStringLiteral("Clipped button: ") + b->text()));
        }
        for (auto* scroll : surface->findChildren<QScrollArea*>()) {
            scroll->verticalScrollBar()->setValue(0); scroll->horizontalScrollBar()->setValue(0);
        }
        QCoreApplication::processEvents();
        // The smoke check may navigate/configure views, never start audio.
        for (int deck = 1; deck <= 4; ++deck) if (ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play"))) failures->append("Unexpected playing deck");
        for (int slot = 1; slot <= 16; ++slot) if (ControlObject::get(ConfigKey(QStringLiteral("[Sampler%1]").arg(slot), "play"))) failures->append("Unexpected playing sampler");
        for (int unit = 1; unit <= 2; ++unit)
            if (ControlObject::get(ConfigKey(QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit), "loaded")) != 1)
                failures->append("Bundled native RMX processor is unavailable");
    };
    QObject::connect(timer, &QTimer::timeout, surface, [=] {
        // Capture the real parser/Library/PlayerManager surface, including the
        // empty native waveform viewers. These are not moving-audio evidence.
        if (*step < 5) surface->findChild<QPushButton*>(QStringLiteral("decklab-tab-%1").arg(*step))->click();
        else if (*step < 10) { surface->selectPage(3); surface->findChild<QPushButton*>(QStringLiteral("decklab-fx-view-%1").arg(*step - 5))->click(); }
        else if (*step < 15) { surface->selectPage(4); surface->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-%1").arg(*step - 10))->click(); }
        else if (*step == 15) surface->openCollection();
        else if (*step == 16) surface->findChild<QPushButton*>("decklab-search")->click();
        else if (*step == 17 || *step == 18) {
            auto* layout=surface->findChild<QComboBox*>("decklab-deck-layout");
            QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,*step==17?2:0));
            surface->selectPage(0);
        }
        else if (*step >= kSystemFirst && *step < kSystemFirst + kSystemSteps) {
            if (*step == kSystemFirst) timer->setInterval(900);
            systemSmokeStep(*step - kSystemFirst, surface);
        }
        else if (*step == 19 || *step == 20) {
            auto* waves=surface->findChild<QComboBox*>("decklab-waveform-layout");
            waves->setCurrentIndex(1);
            QMetaObject::invokeMethod(waves,"activated",Q_ARG(int,1));
            auto* layout=surface->findChild<QComboBox*>("decklab-deck-layout");
            QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,*step==19?1:2));
            surface->selectPage(0);
        }
        else {
            timer->stop();
            QFile file(QDir(output).filePath("native-smoke.json"));
            const bool opened = file.open(QIODevice::WriteOnly);
            int processors = 0;
            for (int unit = 1; unit <= 2; ++unit)
                processors += ControlObject::get(ConfigKey(QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit), "loaded")) == 1;
            if (opened) file.write(QJsonDocument(QJsonObject{{"screens", *step}, {"failures", *failures},
                    {"audio_started", false}, {"native_rmx_units_loaded", processors}}).toJson());
            qApp->exit(opened && failures->isEmpty() ? 0 : 71); return;
        }
        const bool large = *step >= kLargeFirst && *step < kSystemFirst + kSystemSteps;
        surface->setFixedSize(large ? QSize(1024, 600) : QSize(800, 480));
        QTimer::singleShot(*step >= kSystemFirst ? 600 : 100, surface, [=] {
            if (!large) check();
            if (*step >= kSystemFirst) checkSystemTouchTargets(surface, *failures);
            if (*step == 15 || *step == 16) {
                for (int deck = 1; deck <= 2; ++deck)
                    if (surface->findChild<QPushButton*>(QStringLiteral("decklab-load-%1").arg(deck))->isEnabled())
                        failures->append("Empty collection offers Load without a track selection");
            }
            if (!surface->grab().save(QDir(output).filePath(QStringLiteral("app-screen-%1.png").arg(*step, 2, 10, QLatin1Char('0'))))) failures->append("Screenshot failed");
            ++*step;
        });
    });
    timer->start(350);
}
