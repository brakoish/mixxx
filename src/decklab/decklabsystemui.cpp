// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabsystemui.h"

#include <QBoxLayout>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "decklab/decklabdclient.h"
#include "decklab/decklabutility.h"
#include "decklab/decklaboverlay.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabui.h"
#include "moc_decklabsystemui.cpp"
#include "util/cmdlineargs.h"

using namespace decklab::ui;

DeckLabSystemUi::DeckLabSystemUi(QWidget* surface, Hooks hooks)
        : QObject(surface),
          m_surface(surface),
          m_hooks(std::move(hooks)) {
    // An unknown controller is offered for 30 s: a notice, then a NEW lamp
    // on the rail's controller slot.
    m_offerTimer.setSingleShot(true);
    connect(&m_offerTimer, &QTimer::timeout, this, [this] {
        m_offerKey.clear();
        m_offerName.clear();
        refreshRail();
    });
}

DeckLabSystemUi::~DeckLabSystemUi() = default;

DeckLabSystem* DeckLabSystemUi::system() const {
    return m_system;
}

DeckLabSetupWizard* DeckLabSystemUi::wizard() const {
    return m_wizard;
}

bool DeckLabSystemUi::toolsConnected() const {
    return m_system && m_system->isReady();
}

DeckLabSystemPage* DeckLabSystemUi::createSystemPage(QWidget* parent) {
    auto* page = new DeckLabSystemPage(parent);
    page->setLegacyTool([this](int id) { return m_hooks.legacyTool && m_hooks.legacyTool(id); });
    m_systemPage = page;
    if (m_system) {
        page->setSystem(m_system);
    }
    return page;
}

QWidget* DeckLabSystemUi::createSetupPanel(QWidget* parent) {
    // Controller & audio as utility rows (S-21): the state on the right,
    // Run Deck Setup as the action.
    auto* panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("decklab-setup-panel"));
    auto* layout = vbox(panel, 0, 0);
    m_setupController = new decklab::ui::ActionRow(tr("Controller"), panel, QStringLiteral("decklab-setup-controller-state"));
    connect(m_setupController.data(), &QPushButton::clicked, this, [this] { openWizard(0); });
    layout->addWidget(m_setupController);
    m_setupAudio = new decklab::ui::ActionRow(tr("Sound"), panel, QStringLiteral("decklab-setup-audio-state"));
    connect(m_setupAudio.data(), &QPushButton::clicked, this, [this] { openWizard(1); });
    layout->addWidget(m_setupAudio);
    m_setupButton = new decklab::ui::ActionRow(tr("Run Deck Setup"), panel, QStringLiteral("decklab-deck-setup"));
    m_setupButton->setHint(tr("Plug in any Mixxx-supported controller."));
    m_setupButton->setDisabledHint(tr("Unavailable: system service offline"));
    m_setupButton->setEnabled(m_system != nullptr);
    connect(m_setupButton.data(), &QPushButton::clicked, this, [this] { openWizard(0); });
    layout->addWidget(m_setupButton);
    updateSetupPanel();
    return panel;
}

void DeckLabSystemUi::attach(DeckLabSystem* system) {
    if (m_system == system) {
        return;
    }
    m_system = system;
    if (m_systemPage) {
        m_systemPage->setSystem(system);
    }
    if (!system) {
        return;
    }
    connect(system, &DeckLabSystem::readyChanged, this, [this](bool ready) {
        emit toolsConnectedChanged(ready);
        if (ready && m_system) {
            QPointer<DeckLabSystemUi> self(this);
            m_system->client()->call(QStringLiteral("wifi.status"), {}, [self](const DeckLabdReply& reply) {
                if (self && reply.ok) {
                    self->m_wifi = reply.result;
                    self->refreshRail();
                }
            });
        }
        refreshRail();
    });
    connect(system, &DeckLabSystem::thermalChanged, this, [this] { refreshRail(); });
    connect(system, &DeckLabSystem::usbChanged, this, [this](const QJsonArray&) { refreshRail(); });
    connect(system, &DeckLabSystem::devicesChanged, this, [this] { refreshRail(); });
    connect(system->client(), &DeckLabdClient::eventReceived, this, [this](const QString& topic, const QJsonObject& data) {
        if (topic == QStringLiteral("wifi.changed")) {
            m_wifi = data;
            m_wifi.remove(QStringLiteral("error"));
            refreshRail();
        } else if (topic == QStringLiteral("power.undervoltage")) {
            refreshRail();
        }
    });
    connect(system, &DeckLabSystem::notice, this, [this](const QString& text) { showToast(text, 6000); });
    connect(system, &DeckLabSystem::controllerArrived, this, [this](const QString& key, const QString& name, bool known) {
        if (!known) {
            offerController(key, name);
        }
    });
    connect(system, &DeckLabSystem::quitRequested, this, [this](const QString&) {
        showToast(tr("Restarting Deck Lab…"), 20000);
    });
    connect(system, &DeckLabSystem::gracefulQuitRequested, this, [this] {
        if (m_surface && m_surface->window()) {
            m_surface->window()->close();
        }
    });
    connect(system, &DeckLabSystem::layoutRequested, this, [this](int decks) {
        if (m_hooks.applyLayout) {
            m_hooks.applyLayout(decks);
        }
    });
    connect(system, &DeckLabSystem::controllersChanged, this, [this] {
        updateSetupPanel();
        refreshRail();
    });
    connect(system, &DeckLabSystem::audioChanged, this, [this] {
        updateSetupPanel();
        refreshRail();
    });
    if (m_setupButton) {
        m_setupButton->setEnabled(true);
    }
    emit toolsConnectedChanged(system->isReady());
    updateSetupPanel();
    refreshRail();
    bool probe = false;
    const int probeSeconds = qEnvironmentVariableIntValue("DECKLAB_OVERLAY_PROBE", &probe);
    if (probe && probeSeconds >= 0) {
        // Real-display check of the overlays over the GL waveforms: banners
        // first, then the first-run flow (not marked done unless finished).
        QTimer::singleShot(probeSeconds * 1000, this, [this] { showOverlayProbe(); });
        QTimer::singleShot((probeSeconds + 30) * 1000, this, [this] {
            if (!m_wizard) {
                openFirstRun();
            }
        });
    }
    const bool smoke = !qEnvironmentVariableIsEmpty("DECKLAB_SMOKE_DIR");
    if (!system->firstRunDone() && !CmdlineArgs::Instance().getSafeMode() && !smoke) {
        // First run: welcome, Wi-Fi, time zone and Deck Setup once the
        // surface has settled.
        QTimer::singleShot(1500, this, [this] {
            if (!m_wizard && m_system && !m_system->firstRunDone()) {
                openFirstRun();
            }
        });
    }
}

void DeckLabSystemUi::setAudioOutputConfigured(bool configured) {
    m_outputConfigured = configured;
    updateSetupPanel();
}

void DeckLabSystemUi::notify(const QString& text, decklab::ui::Lamp lamp, std::function<void()> action, int milliseconds) {
    if (!m_hooks.notify || text.isEmpty()) {
        return;
    }
    decklab::rail::Notice notice;
    notice.text = text;
    notice.lamp = lamp;
    notice.action = std::move(action);
    notice.milliseconds = milliseconds;
    m_hooks.notify(notice);
}

void DeckLabSystemUi::showOverlayProbe() {
    // Real-display check: news goes to the rail, nothing covers the waveforms.
    showToast(tr("Overlay check: rail notice"), 30000);
    offerController(QStringLiteral("overlay-check"), tr("Overlay check"));
}

void DeckLabSystemUi::refreshRail() {
    using decklab::rail::Inputs;
    Inputs rail;
    rail.serviceAttached = m_system != nullptr;
    rail.serviceOnline = m_system && m_system->isReady();
    if (m_system) {
        // USB drives, rekordbox exports and the recording drive.
        const QJsonArray drives = rail.serviceOnline ? m_system->usbDrives() : QJsonArray();
        const QString directory = m_system->recordingDirectory();
        QStringList names;
        for (const auto& value : drives) {
            const QJsonObject drive = value.toObject();
            const QString label = drive.value(QStringLiteral("label")).toString(tr("USB drive"));
            const bool readOnly = drive.value(QStringLiteral("read_only")).toBool(true);
            rail.usbRekordbox |= drive.value(QStringLiteral("rekordbox")).toBool();
            if (!readOnly && decklab::isPathOnMount(directory, drive.value(QStringLiteral("mount")).toString())) {
                rail.usbRecordTarget = true;
                rail.recordTarget = label;
                rail.recordHoursFree = drive.value(QStringLiteral("free_mb")).toDouble(-1) / 606.0;
            }
            names.append(readOnly ? tr("%1 (read-only)").arg(label) : label);
        }
        rail.usbDrives = static_cast<int>(drives.size());
        rail.usbSummary = names.join(QStringLiteral(" · "));
        // Wi-Fi.
        if (rail.serviceOnline && !m_wifi.isEmpty()) {
            if (!m_wifi.value(QStringLiteral("enabled")).toBool(true)) {
                rail.wifi = Inputs::Wifi::Off;
            } else if (m_wifi.value(QStringLiteral("connected")).toBool()) {
                rail.wifi = Inputs::Wifi::Connected;
                rail.ssid = m_wifi.value(QStringLiteral("ssid")).toString();
                rail.signal = m_wifi.value(QStringLiteral("signal")).toInt(-1);
            } else {
                rail.wifi = Inputs::Wifi::Disconnected;
            }
        }
        // Controllers: working beats found.
        for (const auto& info : m_system->controllers()) {
            if (info.open) {
                rail.controller = Inputs::Controller::Working;
                rail.controllerName = info.name;
                break;
            }
            if (rail.controller == Inputs::Controller::None) {
                rail.controller = Inputs::Controller::Found;
                rail.controllerName = info.name;
            }
        }
        rail.controllerNew = !m_offerKey.isEmpty();
        if (rail.controllerNew && !m_offerName.isEmpty()) {
            rail.controllerName = m_offerName;
        }
        // Sound routing summary.
        const auto plan = m_system->currentAudioPlan();
        if (plan.main.isValid()) {
            rail.soundSummary = tr("%1 %2").arg(decklab::prettyAudioName(plan.main.device),
                    decklab::channelPairLabel(plan.main.channelBase));
            if (plan.headphones.isValid()) {
                rail.soundSummary += tr(" · phones %1").arg(decklab::channelPairLabel(plan.headphones.channelBase));
            }
        }
        // Heat and power: a notice once when it gets hot, the sheet always.
        const QJsonObject thermal = m_system->thermal();
        rail.temperature = thermal.value(QStringLiteral("temp_c")).toDouble(-1);
        const QString level = thermal.value(QStringLiteral("level")).toString();
        rail.heat = level == QStringLiteral("hot") || level == QStringLiteral("warm") ? level : QString();
        rail.undervoltage = m_system->undervoltage();
        if (rail.heat == QStringLiteral("hot") && m_lastHeat != rail.heat) {
            notify(tr("Hot · %1 °C · give it air").arg(std::round(rail.temperature)), decklab::ui::Lamp::Caution);
        }
        m_lastHeat = rail.heat;
    }
    if (rail == m_rail) {
        return;
    }
    m_rail = rail;
    if (m_hooks.statusChanged) {
        m_hooks.statusChanged();
    }
}

void DeckLabSystemUi::fillRailInputs(decklab::rail::Inputs& inputs) const {
    const bool output = inputs.outputWorking;
    const bool pending = inputs.outputPending;
    const bool recording = inputs.recording;
    const qint64 seconds = inputs.recordingSeconds;
    const bool dropouts = inputs.recentDropouts;
    const int count = inputs.dropouts;
    inputs = m_rail;
    inputs.outputWorking = output;
    inputs.outputPending = pending;
    inputs.recording = recording;
    inputs.recordingSeconds = seconds;
    inputs.recentDropouts = dropouts;
    inputs.dropouts = count;
}

void DeckLabSystemUi::readiness(QString* value, decklab::ui::Lamp* lamp) const {
    using decklab::ui::Lamp;
    const bool sound = m_system ? m_system->currentAudioPlan().main.isValid() && m_outputConfigured : m_outputConfigured;
    const bool controller = m_rail.controller == decklab::rail::Inputs::Controller::Working;
    if (!sound) {
        *value = tr("No sound");
        *lamp = Lamp::LiveOutline;
    } else if (!controller) {
        *value = tr("Sound ready · no controller");
        *lamp = Lamp::On;
    } else {
        *value = tr("Ready");
        *lamp = Lamp::Go;
    }
}

void DeckLabSystemUi::showToast(const QString& text, int milliseconds) {
    notify(text, decklab::ui::Lamp::On, {}, milliseconds);
}

void DeckLabSystemUi::offerController(const QString& key, const QString& name) {
    if (m_wizard) {
        return;
    }
    m_offerKey = key;
    m_offerName = name.isEmpty() ? tr("USB device") : name;
    // The whole notice is the button (A-06, A-07).
    notify(tr("New controller: %1 · set up").arg(m_offerName), decklab::ui::Lamp::On, [this, key] {
        m_offerKey.clear();
        m_offerName.clear();
        refreshRail();
        openWizard(0, key);
    });
    m_offerTimer.start(30000);
    refreshRail();
}

DeckLabSetupWizard* DeckLabSystemUi::openWizard(int step, const QString& controllerKey) {
    if (!m_system || !m_surface) {
        return nullptr;
    }
    if (m_hooks.beforeTool) {
        m_hooks.beforeTool();
    }
    if (!m_wizard) {
        m_wizard = new DeckLabSetupWizard(m_system, m_surface);
        connect(m_wizard.data(), &DeckLabSetupWizard::finished, this, [this](bool) {
            m_wizard = nullptr;
            QTimer::singleShot(0, this, [this] {
                updateSetupPanel();
                refreshRail();
            });
        });
        connect(m_wizard.data(), &DeckLabSetupWizard::layoutChosen, this, [this](int decks) {
            if (m_hooks.applyLayout) {
                m_hooks.applyLayout(decks);
            }
        });
        connect(m_wizard.data(), &DeckLabSetupWizard::playRequested, this, [this] {
            if (m_hooks.showDecks) {
                m_hooks.showDecks();
            }
        });
    }
    // The offered controller is what Deck Setup opens on.
    const QString key = controllerKey.isEmpty() && step == 0 ? m_offerKey : controllerKey;
    m_offerKey.clear();
    m_offerName.clear();
    m_offerTimer.stop();
    using Step = DeckLabSetupWizard::Step;
    const QList<Step> steps{Step::Controller, Step::Audio, Step::Layout, Step::Done};
    m_wizard->start(steps.at(std::clamp(step, 0, 3)), key);
    refreshRail();
    return m_wizard;
}

DeckLabSetupWizard* DeckLabSystemUi::openFirstRun() {
    if (!m_system || !m_surface) {
        return nullptr;
    }
    if (m_hooks.beforeTool) {
        m_hooks.beforeTool();
    }
    auto* wizard = openWizard(0);
    if (wizard) {
        wizard->startFirstRun();
    }
    return wizard;
}

void DeckLabSystemUi::updateSetupPanel() {
    if (!m_setupController || !m_setupAudio) {
        return;
    }
    using decklab::ui::Lamp;
    if (!m_system) {
        m_setupController->setValue(tr("Not set up"));
        m_setupController->setLamp(Lamp::Off);
        m_setupAudio->setValue(m_outputConfigured ? tr("Playing out") : tr("No sound output"));
        m_setupAudio->setLamp(m_outputConfigured ? Lamp::Go : Lamp::LiveOutline);
        return;
    }
    QStringList working;
    bool found = false;
    for (const auto& info : m_system->controllers()) {
        found = true;
        if (info.open) {
            const QString mapping = info.mapping.isEmpty() ? QString() : MappingInfo(info.mapping).getName();
            working.append(mapping.isEmpty() || mapping == info.name ? info.name : tr("%1 · %2").arg(info.name, mapping));
        }
    }
    m_setupController->setValue(!working.isEmpty() ? working.join(QStringLiteral(", "))
                    : found                       ? tr("Found · not working yet")
                                                  : tr("No controller set up"));
    m_setupController->setLamp(!working.isEmpty() ? Lamp::Go : found ? Lamp::Caution : Lamp::Off);
    const auto plan = m_system->currentAudioPlan();
    if (!plan.main.isValid()) {
        m_setupAudio->setValue(tr("No sound output"));
        m_setupAudio->setLamp(Lamp::LiveOutline);
        return;
    }
    QString text = tr("%1 %2").arg(decklab::prettyAudioName(plan.main.device), decklab::channelPairLabel(plan.main.channelBase));
    text += plan.headphones.isValid() ? tr(" · phones %1").arg(decklab::channelPairLabel(plan.headphones.channelBase))
                                      : tr(" · phones off");
    m_setupAudio->setValue(text);
    m_setupAudio->setLamp(m_outputConfigured ? Lamp::Go : Lamp::LiveOutline);
}

bool DeckLabSystemUi::handleTool(int id) {
    if (id == 5 && m_system) {
        openWizard(0);
        return true;
    }
    if (!toolsConnected()) {
        return false;
    }
    const auto show = [this](DeckLabSystemPage::Section section) {
        if (m_hooks.beforeTool) {
            m_hooks.beforeTool();
        }
        if (m_hooks.showSystem) {
            m_hooks.showSystem(section);
        }
    };
    switch (id) {
    case 1:
    case 4:
    case 10:
        show(DeckLabSystemPage::Section::Storage);
        return true;
    case 2:
        launchApp(QStringLiteral("get-music"), tr("Get Music"));
        return true;
    case 3:
        launchApp(QStringLiteral("sample-maker"), tr("Sample Maker"));
        return true;
    case 6:
        show(DeckLabSystemPage::Section::Wifi);
        return true;
    case 9:
        show(DeckLabSystemPage::Section::Maintenance);
        return true;
    default:
        return false;
    }
}

void DeckLabSystemUi::launchApp(const QString& app, const QString& name) {
    if (m_hooks.beforeTool) {
        m_hooks.beforeTool();
    }
    showToast(tr("Opening %1…").arg(name), 4000);
    QPointer<DeckLabSystemUi> self(this);
    m_system->client()->call(QStringLiteral("apps.launch"), QJsonObject{{QStringLiteral("app"), app}},
            [self, name, app](const DeckLabdReply& reply) {
                if (!self || reply.ok) {
                    return;
                }
                if (reply.errorCode == QStringLiteral("not_available")) {
                    // Optional add-on not installed: stop offering it.
                    if (self->m_surface) {
                        if (auto* button = self->m_surface->findChild<QPushButton*>(
                                    QStringLiteral("decklab-tool-") + app)) {
                            button->hide();
                        }
                    }
                    self->showToast(tr("%1 isn't installed on this unit").arg(name), 7000);
                    return;
                }
                self->showToast(tr("Couldn't open %1").arg(name), 7000);
            });
}
