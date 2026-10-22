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

#include "control/controlproxy.h"
#include "decklab/decklabdclient.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabui.h"
#include "moc_decklabsystemui.cpp"
#include "util/cmdlineargs.h"

using namespace decklab::ui;

namespace {
enum class BannerKind {
    Alert,
    Warning,
    Info,
};

// Compact pill over the top of the surface. Informational banners pass
// touches through to the controls beneath; only actionable ones take taps.
class Banner : public QPushButton {
  public:
    Banner(BannerKind kind, Icon icon, bool actionable, QWidget* parent, const QString& name)
            : QPushButton(parent),
              m_kind(kind),
              m_icon(icon),
              m_actionable(actionable) {
        setObjectName(name);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents, !actionable);
        setFixedHeight(40);
        hide();
    }
    void setMessage(const QString& text) {
        setText(text);
        setAccessibleName(text);
        QFont bold = font();
        bold.setPixelSize(13);
        bold.setWeight(QFont::DemiBold);
        const int textWidth = QFontMetrics(bold).horizontalAdvance(text);
        m_width = 16 + 22 + 10 + textWidth + (m_actionable ? 30 : 16);
        update();
    }
    int preferredWidth() const {
        return m_width;
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor background("#1c1c1c");
        QColor border("#4a4a4a");
        QColor accent("#e6e6e6");
        if (m_kind == BannerKind::Alert) {
            background = QColor(isDown() ? "#4a1a1d" : "#2d1416");
            border = QColor(kAccent);
            accent = QColor("#ff8f92");
        } else if (m_kind == BannerKind::Warning) {
            background = QColor("#2a2312");
            border = QColor("#a07a22");
            accent = QColor("#ffc84a");
        }
        background.setAlpha(240);
        painter.setPen(QPen(border, 1.2));
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.6, 0.6, -0.6, -0.6), 4, 4);
        paintIcon(painter, QRectF(14, (height() - 20) / 2.0, 20, 20), m_icon, accent);
        QFont bold = font();
        bold.setPixelSize(13);
        bold.setWeight(QFont::DemiBold);
        painter.setFont(bold);
        painter.setPen(QColor("#f6f6f6"));
        const int right = m_actionable ? 30 : 14;
        painter.drawText(QRect(44, 0, std::max(0, width() - 44 - right), height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(bold).elidedText(text(), Qt::ElideRight, std::max(0, width() - 44 - right)));
        if (m_actionable) {
            QFont chevron = font();
            chevron.setPixelSize(20);
            painter.setFont(chevron);
            painter.setPen(accent);
            painter.drawText(QRect(width() - 26, 0, 16, height()), Qt::AlignCenter, QStringLiteral("›"));
        }
    }

  private:
    BannerKind m_kind;
    Icon m_icon;
    bool m_actionable;
    int m_width = 200;
};

Banner* asBanner(QPushButton* button) {
    return static_cast<Banner*>(button);
}
} // namespace

DeckLabSystemUi::DeckLabSystemUi(QWidget* surface, Hooks hooks)
        : QObject(surface),
          m_surface(surface),
          m_hooks(std::move(hooks)) {
    m_audioBanner = new Banner(BannerKind::Alert, Icon::Speaker, true, surface, QStringLiteral("decklab-banner-audio"));
    asBanner(m_audioBanner)->setMessage(tr("No audio output  ·  Set up"));
    connect(m_audioBanner, &QPushButton::clicked, this, [this] { openWizard(1); });
    m_offerBanner = new Banner(BannerKind::Info, Icon::Controller, true, surface, QStringLiteral("decklab-banner-offer"));
    connect(m_offerBanner, &QPushButton::clicked, this, [this] {
        const QString key = m_offerKey;
        m_offerKey.clear();
        m_offerBanner->hide();
        openWizard(0, key);
    });
    m_thermalBanner = new Banner(BannerKind::Warning, Icon::Warning, false, surface, QStringLiteral("decklab-banner-thermal"));
    m_toast = new Banner(BannerKind::Info, Icon::Info, false, surface, QStringLiteral("decklab-banner-toast"));
    m_toastTimer.setSingleShot(true);
    connect(&m_toastTimer, &QTimer::timeout, this, [this] {
        m_toast->hide();
        layoutBanners();
    });
    m_offerTimer.setSingleShot(true);
    connect(&m_offerTimer, &QTimer::timeout, this, [this] {
        m_offerKey.clear();
        m_offerBanner->hide();
        layoutBanners();
    });
    // Startup opens devices after the skin; don't flash the banner meanwhile.
    m_audioDebounce.setSingleShot(true);
    m_audioDebounce.setInterval(900);
    connect(&m_audioDebounce, &QTimer::timeout, this, [this] { updateAudioBanner(); });
    m_tab = new ControlProxy(QStringLiteral("[Tab]"), QStringLiteral("current"), this, ControlFlag::NoWarnIfMissing);
    m_tab->connectValueChanged(this, [this](double) { updateAudioBanner(); }, Qt::QueuedConnection);
    m_soundStatus = new ControlProxy(QStringLiteral("[SoundManager]"), QStringLiteral("status"), this,
            ControlFlag::NoWarnIfMissing);
    m_soundStatus->connectValueChanged(this, [this](double) { m_audioDebounce.start(); }, Qt::QueuedConnection);
    surface->installEventFilter(this);
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

bool DeckLabSystemUi::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_surface && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        layoutBanners();
    }
    return QObject::eventFilter(watched, event);
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
    auto* panel = card(parent);
    panel->setObjectName(QStringLiteral("decklab-setup-panel"));
    applyStyle(panel);
    auto* layout = vbox(panel, 14, 6);
    auto* top = hbox(12);
    auto* text = new QWidget(panel);
    auto* textLayout = vbox(text, 0, 2);
    textLayout->addWidget(title(tr("Deck Setup"), text));
    m_setupController = muted(tr("Plug in any Mixxx-supported controller and play."), text);
    m_setupController->setObjectName(QStringLiteral("decklab-setup-controller-state"));
    textLayout->addWidget(m_setupController);
    m_setupAudio = muted(QString(), text);
    m_setupAudio->setObjectName(QStringLiteral("decklab-setup-audio-state"));
    textLayout->addWidget(m_setupAudio);
    top->addWidget(text, 1);
    m_setupButton = primaryButton(tr("Deck Setup"), panel, QStringLiteral("decklab-deck-setup"), 52);
    m_setupButton->setFixedWidth(160);
    m_setupButton->setEnabled(false);
    connect(m_setupButton, &QPushButton::clicked, this, [this] { openWizard(0); });
    top->addWidget(m_setupButton);
    layout->addLayout(top);
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
        layoutBanners();
    });
    connect(system, &DeckLabSystem::thermalChanged, this, [this] { layoutBanners(); });
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
    connect(system, &DeckLabSystem::controllersChanged, this, [this] { updateSetupPanel(); });
    connect(system, &DeckLabSystem::audioChanged, this, [this] {
        updateSetupPanel();
        m_audioDebounce.start();
    });
    if (m_setupButton) {
        m_setupButton->setEnabled(true);
    }
    emit toolsConnectedChanged(system->isReady());
    updateSetupPanel();
    m_audioDebounce.start();
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
    m_audioDebounce.start();
}

void DeckLabSystemUi::updateAudioBanner() {
    const int page = static_cast<int>(m_tab->get());
    const bool working = m_outputConfigured && m_soundStatus->get() == 2;
    // Browse needs its full height for search; Settings → Setup has the
    // same action inline. Everywhere else the banner stays until fixed.
    const bool show = m_system && !working && page != 1 && page != 5 && !m_wizard;
    m_audioBanner->setVisible(show);
    layoutBanners();
}

void DeckLabSystemUi::layoutBanners() {
    if (!m_surface) {
        return;
    }
    const QString thermal = m_system ? m_system->thermalWarning() : QString();
    asBanner(m_thermalBanner)->setMessage(thermal);
    m_thermalBanner->setVisible(!thermal.isEmpty());
    if (m_wizard) {
        m_audioBanner->hide();
        m_offerBanner->hide();
    }
    int top = 46;
    for (QPushButton* banner : {m_audioBanner, m_offerBanner, m_thermalBanner, m_toast}) {
        if (!banner->isVisibleTo(m_surface)) {
            continue;
        }
        const int width = std::min(asBanner(banner)->preferredWidth(), m_surface->width() - 32);
        banner->setGeometry((m_surface->width() - width) / 2, top, width, banner->height());
        banner->raise();
        top += banner->height() + 6;
    }
}

void DeckLabSystemUi::showToast(const QString& text, int milliseconds) {
    if (text.isEmpty()) {
        return;
    }
    asBanner(m_toast)->setMessage(text);
    m_toast->show();
    m_toastTimer.start(milliseconds);
    layoutBanners();
}

void DeckLabSystemUi::offerController(const QString& key, const QString& name) {
    if (m_wizard) {
        return;
    }
    m_offerKey = key;
    asBanner(m_offerBanner)->setMessage(tr("New controller: %1  ·  Set up").arg(name.isEmpty() ? tr("USB device") : name));
    m_offerBanner->show();
    m_offerTimer.start(30000);
    layoutBanners();
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
            QTimer::singleShot(0, this, [this] {
                updateAudioBanner();
                updateSetupPanel();
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
    m_offerKey.clear();
    m_offerBanner->hide();
    using Step = DeckLabSetupWizard::Step;
    const QList<Step> steps{Step::Controller, Step::Audio, Step::Layout, Step::Done};
    m_wizard->start(steps.at(std::clamp(step, 0, 3)), controllerKey);
    layoutBanners();
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
        layoutBanners();
    }
    return wizard;
}

void DeckLabSystemUi::updateSetupPanel() {
    if (!m_setupController || !m_setupAudio) {
        return;
    }
    if (!m_system) {
        m_setupAudio->setText(QString());
        return;
    }
    QStringList working;
    for (const auto& info : m_system->controllers()) {
        if (info.open) {
            const QString mapping = info.mapping.isEmpty() ? QString() : MappingInfo(info.mapping).getName();
            working.append(mapping.isEmpty() ? info.name : tr("%1 · %2").arg(info.name, mapping));
        }
    }
    m_setupController->setText(working.isEmpty() ? tr("No controller is set up yet.")
                                                 : tr("Controller: %1").arg(working.join(QStringLiteral(", "))));
    const auto plan = m_system->currentAudioPlan();
    if (!plan.main.isValid()) {
        m_setupAudio->setText(tr("No audio output. Tap Deck Setup to choose one."));
        return;
    }
    QString text = tr("Main: %1 %2").arg(decklab::prettyAudioName(plan.main.device),
            decklab::channelPairLabel(plan.main.channelBase));
    text += plan.headphones.isValid()
            ? tr("   ·   Headphones: %1 %2")
                      .arg(decklab::prettyAudioName(plan.headphones.device),
                              decklab::channelPairLabel(plan.headphones.channelBase))
            : tr("   ·   Headphones off");
    m_setupAudio->setText(text);
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
                    self->showToast(tr("%1 isn't installed on this unit.").arg(name), 7000);
                    return;
                }
                self->showToast(reply.errorMessage.isEmpty() ? tr("%1 could not be opened.").arg(name)
                                                             : reply.errorMessage,
                        7000);
            });
}
