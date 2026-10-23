// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabsystempages.h"

#include <QBoxLayout>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <algorithm>
#include <tuple>
#include <utility>

#include "control/controlobject.h"
#include "decklab/decklabdclient.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabinstruments.h"
#include "decklab/decklabui.h"
#include "moc_decklabsystempages.cpp"
#include "util/versionstore.h"

using namespace decklab::ui;
using Section = DeckLabSystemPage::Section;

namespace {
constexpr int kHeatTile = 100;

// Public source of this Mixxx fork, for the GPL source offer on About. A fork
// that publishes its own builds points this at its own repository.
const QString kSourceCodeUrl = QStringLiteral("https://github.com/brakoish/mixxx");

QString sectionKey(Section section) {
    switch (section) {
    case Section::Home:
        return QStringLiteral("home");
    case Section::Wifi:
        return QStringLiteral("wifi");
    case Section::Bluetooth:
        return QStringLiteral("bluetooth");
    case Section::Screen:
        return QStringLiteral("screen");
    case Section::Time:
        return QStringLiteral("time");
    case Section::Storage:
        return QStringLiteral("storage");
    case Section::Power:
        return QStringLiteral("power");
    case Section::About:
        return QStringLiteral("about");
    case Section::Maintenance:
        return QStringLiteral("maintenance");
    }
    return QString();
}

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        } else if (QLayout* child = item->layout()) {
            clearLayout(child);
        }
        delete item;
    }
}

QString uptimeText(qint64 seconds) {
    const qint64 days = seconds / 86400;
    const qint64 hours = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (days > 0) {
        return QObject::tr("%1 d %2 h").arg(days).arg(hours);
    }
    if (hours > 0) {
        return QObject::tr("%1 h %2 min").arg(hours).arg(minutes);
    }
    return QObject::tr("%1 min").arg(minutes);
}

// Shared frame of every System page: back header, inline message, a body
// stack (content / offline / page-specific panels).
class SectionPage : public QWidget {
  public:
    SectionPage(DeckLabSystemPage* owner, Section section, const QString& heading)
            : QWidget(owner),
              m_owner(owner),
              m_section(section) {
        setObjectName(QStringLiteral("decklab-system-section-%1").arg(sectionKey(section)));
        auto* root = vbox(this, 0, 0);
        auto* header = new QWidget(this);
        header->setFixedHeight(52);
        auto* headerRow = hbox(8);
        headerRow->setContentsMargins(8, 4, 12, 4);
        header->setLayout(headerRow);
        // One page header (R-15): a 44 px back glyph, the breadcrumb, the title.
        auto* back = new GlyphButton(Glyph::Back, tr("Back to Unit"), header, QStringLiteral("decklab-system-back"));
        connect(back, &QPushButton::clicked, owner, [owner] { owner->showSection(Section::Home); });
        headerRow->addWidget(back);
        auto* crumb = new QLabel(tr("UNIT /"), header);
        crumb->setObjectName(QStringLiteral("decklab-system-crumb"));
        crumb->setStyleSheet(QStringLiteral("font-size:14px;font-weight:600;color:#9AA3AF;"));
        headerRow->addWidget(crumb);
        m_heading = title(heading, header);
        headerRow->addWidget(m_heading, 1);
        m_headerExtra = hbox(8);
        headerRow->addLayout(m_headerExtra);
        root->addWidget(header);
        m_message = new QLabel(this);
        m_message->setObjectName(QStringLiteral("decklab-system-message"));
        m_message->setWordWrap(true);
        m_message->setContentsMargins(14, 2, 14, 6);
        m_message->hide();
        root->addWidget(m_message);
        m_body = new QStackedWidget(this);
        root->addWidget(m_body, 1);
        // 0 = content, 1 = offline, 2+ = full-height panels.
        connect(m_body, &QStackedWidget::currentChanged, owner, [owner](int index) {
            owner->notifyPanel(index >= 2);
        });
        m_content = new QWidget(m_body);
        m_layout = vbox(m_content, 14, 10);
        m_layout->setContentsMargins(14, 4, 14, 14);
        m_body->addWidget(scrollArea(m_content, m_body,
                QStringLiteral("decklab-system-scroll-%1").arg(sectionKey(section))));
        m_offline = new QWidget(m_body);
        auto* offline = vbox(m_offline, 24, 12);
        offline->addStretch(1);
        auto* offlineTitle = title(tr("System service offline"), m_offline);
        offlineTitle->setAlignment(Qt::AlignHCenter);
        offline->addWidget(offlineTitle);
        auto* offlineText = body(tr("Deck Lab keeps working for DJing. These settings come back "
                                    "automatically when the system service restarts."),
                m_offline);
        offlineText->setAlignment(Qt::AlignHCenter);
        offline->addWidget(offlineText);
        m_offlineActions = hbox(10);
        offline->addLayout(m_offlineActions);
        offline->addStretch(2);
        m_body->addWidget(m_offline);
    }

    Section section() const {
        return m_section;
    }

    void setSystem(DeckLabSystem* system) {
        m_system = system;
        if (system) {
            connect(system, &DeckLabSystem::readyChanged, this, [this](bool) { updateOnline(); });
            connect(system->client(), &DeckLabdClient::eventReceived, this, [this](const QString& topic, const QJsonObject& data) {
                if (isVisible()) {
                    onEvent(topic, data);
                }
            });
            connectSystem();
        }
        updateOnline();
    }

    void updateOnline() {
        const bool online = !needsService() || (m_system && m_system->isReady());
        if (!online) {
            m_body->setCurrentWidget(m_offline);
        } else if (m_body->currentWidget() == m_offline) {
            m_body->setCurrentIndex(0);
        }
        if (online && isVisible()) {
            activate();
        }
    }

    virtual void activate() {
    }
    virtual void deactivate() {
    }

  protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        say(QString());
        updateOnline();
    }
    void hideEvent(QHideEvent* event) override {
        deactivate();
        QWidget::hideEvent(event);
    }

    virtual bool needsService() const {
        return true;
    }
    virtual void connectSystem() {
    }
    virtual void onEvent(const QString& topic, const QJsonObject& data) {
        Q_UNUSED(topic);
        Q_UNUSED(data);
    }

    bool online() const {
        return m_system && m_system->isReady();
    }

    void say(const QString& text, bool error = false) {
        m_message->setProperty("role", error ? QStringLiteral("error") : QStringLiteral("value"));
        m_message->style()->unpolish(m_message);
        m_message->style()->polish(m_message);
        m_message->setText(text);
        m_message->setVisible(!text.isEmpty());
    }

    void call(const QString& method,
            const QJsonObject& params,
            std::function<void(const DeckLabdReply&)> done = std::function<void(const DeckLabdReply&)>()) {
        if (!m_system) {
            if (done) {
                DeckLabdReply reply;
                reply.errorCode = QStringLiteral("not_available");
                reply.errorMessage = tr("System service offline");
                done(reply);
            }
            return;
        }
        QPointer<SectionPage> self(this);
        m_system->client()->call(method, params, [self, done](const DeckLabdReply& reply) {
            if (self && done) {
                done(reply);
            }
        });
    }

    QString failure(const DeckLabdReply& reply) const {
        if (reply.errorCode == QStringLiteral("not_available")) {
            return reply.errorMessage.isEmpty() ? tr("Not available on this unit.") : reply.errorMessage;
        }
        return reply.errorMessage.isEmpty() ? tr("Something went wrong (%1).").arg(reply.errorCode)
                                            : reply.errorMessage;
    }

    void addLegacyAction(const QString& text, int tool) {
        auto* legacy = button(text, m_offline, QStringLiteral("decklab-system-legacy"), 48);
        connect(legacy, &QPushButton::clicked, this, [this, tool] {
            if (!m_owner->legacyTool(tool)) {
                say(tr("The companion tools are not running either."), true);
            }
        });
        m_offlineActions->addStretch(1);
        m_offlineActions->addWidget(legacy, 2);
        m_offlineActions->addStretch(1);
    }

    DeckLabSystemPage* m_owner;
    Section m_section;
    QPointer<DeckLabSystem> m_system;
    QLabel* m_heading;
    QHBoxLayout* m_headerExtra;
    QLabel* m_message;
    QStackedWidget* m_body;
    QWidget* m_content;
    QVBoxLayout* m_layout;
    QWidget* m_offline;
    QHBoxLayout* m_offlineActions;
};

// ---------------------------------------------------------------- Wi-Fi --
class WifiPage : public SectionPage {
  public:
    explicit WifiPage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Wifi, tr("Wi-Fi")) {
        m_toggle = button(tr("Turn off"), this, QStringLiteral("decklab-wifi-toggle"), 44);
        m_toggle->setFixedWidth(110);
        m_headerExtra->addWidget(m_toggle);
        connect(m_toggle, &QPushButton::clicked, this, [this] {
            const bool on = !m_status.value(QStringLiteral("enabled")).toBool(true);
            call(QStringLiteral("wifi.enable"), QJsonObject{{QStringLiteral("on"), on}}, [this, on](const DeckLabdReply& reply) {
                if (!reply.ok) {
                    say(failure(reply), true);
                    return;
                }
                refreshStatus();
                if (on) {
                    scan();
                }
            });
        });
        auto* summary = card(m_content);
        auto* summaryLayout = vbox(summary, 14, 2);
        auto* summaryRow = hbox(12);
        m_state = title(QString(), summary);
        m_state->setObjectName(QStringLiteral("decklab-wifi-state"));
        summaryRow->addWidget(m_state, 1);
        summaryLayout->addLayout(summaryRow);
        m_detail = muted(QString(), summary);
        summaryLayout->addWidget(m_detail);
        m_layout->addWidget(summary);
        auto* listHeader = hbox(8);
        listHeader->addWidget(sectionLabel(tr("NETWORKS"), m_content), 1);
        m_scan = button(tr("Scan"), m_content, QStringLiteral("decklab-wifi-scan"), 44);
        m_scan->setFixedWidth(110);
        connect(m_scan, &QPushButton::clicked, this, [this] { scan(); });
        listHeader->addWidget(m_scan);
        m_layout->addLayout(listHeader);
        m_list = new QWidget(m_content);
        m_listLayout = vbox(m_list, 0, 0);
        m_layout->addWidget(m_list);
        m_empty = muted(tr("Scanning for networks…"), m_content);
        m_layout->addWidget(m_empty);
        auto* other = button(tr("Other network…"), m_content, QStringLiteral("decklab-wifi-other"), 48);
        connect(other, &QPushButton::clicked, this, [this] { openJoin(QString(), true, true); });
        m_layout->addWidget(other);
        m_layout->addStretch(1);
        buildJoin();
        addLegacyAction(tr("Open Wi-Fi in companion"), 6);
    }

    void activate() override {
        refreshStatus();
        if (m_networks.isEmpty()) {
            scan();
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::FocusIn) {
            if (watched == m_ssid || watched == m_password) {
                m_keyboard->setTarget(static_cast<QLineEdit*>(watched));
            }
        }
        return SectionPage::eventFilter(watched, event);
    }

    void onEvent(const QString& topic, const QJsonObject& data) override {
        if (topic == QStringLiteral("wifi.changed")) {
            const QString error = data.value(QStringLiteral("error")).toString();
            QJsonObject status = data;
            status.remove(QStringLiteral("error"));
            applyStatus(status);
            if (!error.isEmpty()) {
                say(error, true);
                if (!m_pendingSsid.isEmpty()) {
                    openJoin(m_pendingSsid, true, m_pendingHidden);
                    m_password->clear();
                }
                m_pendingSsid.clear();
            } else if (!m_pendingSsid.isEmpty() && status.value(QStringLiteral("connected")).toBool() &&
                    status.value(QStringLiteral("ssid")).toString() == m_pendingSsid) {
                say(tr("Connected to %1.").arg(m_pendingSsid));
                m_pendingSsid.clear();
                scan();
            }
        } else if (topic == QStringLiteral("wifi.scan_done")) {
            if (!data.value(QStringLiteral("error")).toString().isEmpty()) {
                say(data.value(QStringLiteral("error")).toString(), true);
            }
            m_networks = data.value(QStringLiteral("networks")).toArray();
            m_scanning = false;
            m_scan->setEnabled(true);
            m_scan->setText(tr("Scan"));
            renderNetworks();
        }
    }

  private:
    void refreshStatus() {
        call(QStringLiteral("wifi.status"), {}, [this](const DeckLabdReply& reply) {
            if (reply.ok) {
                applyStatus(reply.result);
            } else {
                say(failure(reply), true);
            }
        });
    }

    void scan() {
        if (m_scanning) {
            return;
        }
        m_scanning = true;
        m_scan->setEnabled(false);
        m_scan->setText(tr("Scanning…"));
        call(QStringLiteral("wifi.scan"), {}, [this](const DeckLabdReply& reply) {
            if (!reply.ok) {
                m_scanning = false;
                m_scan->setEnabled(true);
                m_scan->setText(tr("Scan"));
                say(failure(reply), true);
                return;
            }
            // Results arrive as wifi.scan_done; never leave the button stuck.
            QTimer::singleShot(20000, this, [this] {
                if (m_scanning) {
                    m_scanning = false;
                    m_scan->setEnabled(true);
                    m_scan->setText(tr("Scan"));
                }
            });
        });
    }

    void applyStatus(const QJsonObject& status) {
        m_status = status;
        const bool enabled = status.value(QStringLiteral("enabled")).toBool(true);
        const bool connected = status.value(QStringLiteral("connected")).toBool();
        const QString ssid = status.value(QStringLiteral("ssid")).toString();
        m_toggle->setText(enabled ? tr("Turn off") : tr("Turn on"));
        m_scan->setVisible(enabled);
        if (!enabled) {
            m_state->setText(tr("Wi-Fi is off"));
            m_detail->setText(tr("Turn it on to connect to a network."));
        } else if (connected) {
            m_state->setText(tr("Connected to %1").arg(ssid));
            QStringList details;
            if (status.value(QStringLiteral("signal")).isDouble()) {
                details.append(tr("Signal %1%").arg(status.value(QStringLiteral("signal")).toInt()));
            }
            if (!status.value(QStringLiteral("ip")).toString().isEmpty()) {
                details.append(tr("IP %1").arg(status.value(QStringLiteral("ip")).toString()));
            }
            m_detail->setText(details.join(QStringLiteral("  ·  ")));
        } else {
            m_state->setText(tr("Not connected"));
            m_detail->setText(tr("Choose a network below."));
        }
        renderNetworks();
    }

    void renderNetworks() {
        clearLayout(m_listLayout);
        const bool enabled = m_status.value(QStringLiteral("enabled")).toBool(true);
        QList<QJsonObject> networks;
        for (const auto& value : std::as_const(m_networks)) {
            const QJsonObject network = value.toObject();
            if (!network.value(QStringLiteral("ssid")).toString().isEmpty()) {
                networks.append(network);
            }
        }
        std::stable_sort(networks.begin(), networks.end(), [](const QJsonObject& left, const QJsonObject& right) {
            const auto rank = [](const QJsonObject& network) {
                return (network.value(QStringLiteral("active")).toBool() ? 1000 : 0) +
                        (network.value(QStringLiteral("saved")).toBool() ? 500 : 0) +
                        network.value(QStringLiteral("signal")).toInt();
            };
            return rank(left) > rank(right);
        });
        m_empty->setVisible(enabled && networks.isEmpty());
        m_empty->setText(m_scanning ? tr("Scanning for networks…") : tr("No networks found. Tap Scan."));
        m_list->setVisible(enabled);
        for (const auto& network : std::as_const(networks)) {
            const QString ssid = network.value(QStringLiteral("ssid")).toString();
            const QString security = network.value(QStringLiteral("security")).toString();
            const bool active = network.value(QStringLiteral("active")).toBool();
            const bool saved = network.value(QStringLiteral("saved")).toBool();
            const bool secured = security != QStringLiteral("open");
            const QString securityText = security == QStringLiteral("open") ? tr("Open network")
                    : security == QStringLiteral("wpa3")                    ? tr("Secured · WPA3")
                    : security == QStringLiteral("enterprise")              ? tr("Enterprise (not supported here)")
                                                                            : tr("Secured · WPA2");
            auto* line = new QWidget(m_list);
            auto* lineLayout = hbox(0);
            line->setLayout(lineLayout);
            auto* row = new Row(ssid, securityText, line, QStringLiteral("decklab-wifi-network"));
            row->setRowIcon(Icon::Wifi);
            row->setSignal(network.value(QStringLiteral("signal")).toInt());
            row->setLocked(secured);
            QStringList tags;
            if (active) {
                tags.append(tr("Connected"));
            }
            if (saved) {
                tags.append(tr("Saved"));
            }
            row->setTags(tags);
            row->setSelectedRow(active);
            connect(row, &QPushButton::clicked, this, [this, ssid, secured, saved, active, security] {
                if (active) {
                    say(tr("Already connected to %1.").arg(ssid));
                } else if (security == QStringLiteral("enterprise")) {
                    say(tr("Enterprise networks need the maintenance desktop."), true);
                } else if (!secured || saved) {
                    connectTo(ssid, QString(), false);
                } else {
                    openJoin(ssid, true, false);
                }
            });
            lineLayout->addWidget(row, 1);
            if (saved) {
                // Hold to forget (Signature 4): no modal, no accidental brush.
                auto* forget = new HoldButton(tr("Forget"), line, QStringLiteral("decklab-wifi-forget"), 56);
                forget->setFixedWidth(112);
                connect(forget, &QPushButton::clicked, this, [this] { say(tr("Hold Forget to forget a network.")); });
                connect(forget, &HoldButton::confirmed, this, [this, ssid] {
                    call(QStringLiteral("wifi.forget"), QJsonObject{{QStringLiteral("ssid"), ssid}},
                            [this, ssid](const DeckLabdReply& reply) {
                                say(reply.ok ? tr("Forgot %1. You'll need the password again.").arg(ssid)
                                             : failure(reply),
                                        !reply.ok);
                                refreshStatus();
                                scan();
                            });
                });
                lineLayout->addWidget(forget);
            }
            m_listLayout->addWidget(line);
        }
    }

    void buildJoin() {
        m_join = new QWidget(m_body);
        auto* layout = vbox(m_join, 0, 8);
        layout->setContentsMargins(12, 0, 12, 8);
        auto* top = hbox(8);
        auto* cancel = button(tr("Cancel"), m_join, QStringLiteral("decklab-wifi-cancel"), 44);
        cancel->setFixedWidth(110);
        connect(cancel, &QPushButton::clicked, this, [this] {
            m_body->setCurrentIndex(0);
            say(QString());
        });
        top->addWidget(cancel);
        m_joinTitle = new QLabel(m_join);
        m_joinTitle->setProperty("role", QStringLiteral("value"));
        m_joinTitle->setAlignment(Qt::AlignCenter);
        top->addWidget(m_joinTitle, 1);
        m_connect = primaryButton(tr("Connect"), m_join, QStringLiteral("decklab-wifi-connect"), 44);
        m_connect->setFixedWidth(130);
        connect(m_connect, &QPushButton::clicked, this, [this] {
            const QString ssid = m_hidden ? m_ssid->text().trimmed() : m_joinSsid;
            if (ssid.isEmpty()) {
                say(tr("Type the network name."), true);
                return;
            }
            const QString password = m_password->text();
            if (!password.isEmpty() && (password.size() < 8 || password.size() > 63)) {
                say(tr("Wi-Fi passwords are 8 to 63 characters."), true);
                return;
            }
            connectTo(ssid, password, m_hidden);
        });
        top->addWidget(m_connect);
        layout->addLayout(top);
        auto* fields = hbox(8);
        m_ssid = new QLineEdit(m_join);
        m_ssid->setObjectName(QStringLiteral("decklab-wifi-ssid"));
        m_ssid->setPlaceholderText(tr("Network name"));
        m_ssid->installEventFilter(this);
        fields->addWidget(m_ssid, 1);
        m_password = new QLineEdit(m_join);
        m_password->setObjectName(QStringLiteral("decklab-wifi-password"));
        m_password->setPlaceholderText(tr("Password"));
        m_password->setEchoMode(QLineEdit::Password);
        m_password->installEventFilter(this);
        fields->addWidget(m_password, 1);
        auto* reveal = button(tr("Show"), m_join, QStringLiteral("decklab-wifi-reveal"), 44);
        reveal->setCheckable(true);
        reveal->setFixedWidth(84);
        connect(reveal, &QPushButton::toggled, this, [this, reveal](bool shown) {
            m_password->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
            reveal->setText(shown ? tr("Hide") : tr("Show"));
        });
        fields->addWidget(reveal);
        layout->addLayout(fields);
        m_keyboard = new Keyboard(m_join);
        connect(m_keyboard, &Keyboard::done, m_connect, &QPushButton::click);
        layout->addWidget(m_keyboard);
        layout->addStretch(1);
        m_body->addWidget(m_join);
    }

    void openJoin(const QString& ssid, bool secured, bool hidden) {
        Q_UNUSED(secured);
        m_joinSsid = ssid;
        m_hidden = hidden;
        m_ssid->setVisible(hidden);
        m_ssid->setText(hidden ? ssid : QString());
        m_password->clear();
        m_joinTitle->setText(hidden ? tr("Join a hidden network") : tr("Join “%1”").arg(ssid));
        m_body->setCurrentWidget(m_join);
        QLineEdit* first = hidden ? m_ssid : m_password;
        first->setFocus(Qt::OtherFocusReason);
        m_keyboard->setTarget(first);
    }

    void connectTo(const QString& ssid, const QString& password, bool hidden) {
        QJsonObject params{{QStringLiteral("ssid"), ssid}};
        if (!password.isEmpty()) {
            params.insert(QStringLiteral("password"), password);
        }
        if (hidden) {
            params.insert(QStringLiteral("hidden"), true);
            params.insert(QStringLiteral("security"),
                    password.isEmpty() ? QStringLiteral("open") : QStringLiteral("wpa"));
        }
        m_pendingSsid = ssid;
        m_pendingHidden = hidden;
        call(QStringLiteral("wifi.connect"), params, [this, ssid](const DeckLabdReply& reply) {
            if (!reply.ok) {
                m_pendingSsid.clear();
                say(failure(reply), true);
                return;
            }
            m_body->setCurrentIndex(0);
            say(tr("Connecting to %1…").arg(ssid));
        });
    }

    QPushButton* m_toggle;
    QLabel* m_state;
    QLabel* m_detail;
    QPushButton* m_scan;
    QWidget* m_list;
    QVBoxLayout* m_listLayout;
    QLabel* m_empty;
    QWidget* m_join = nullptr;
    QLabel* m_joinTitle = nullptr;
    QLineEdit* m_ssid = nullptr;
    QLineEdit* m_password = nullptr;
    QPushButton* m_connect = nullptr;
    Keyboard* m_keyboard = nullptr;
    QJsonObject m_status;
    QJsonArray m_networks;
    QString m_joinSsid;
    QString m_pendingSsid;
    bool m_pendingHidden = false;
    bool m_hidden = false;
    bool m_scanning = false;
};

// ------------------------------------------------------------ Bluetooth --
class BluetoothPage : public SectionPage {
  public:
    explicit BluetoothPage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Bluetooth, tr("Bluetooth")) {
        m_toggle = button(tr("Turn off"), this, QStringLiteral("decklab-bt-toggle"), 44);
        m_toggle->setFixedWidth(110);
        m_headerExtra->addWidget(m_toggle);
        connect(m_toggle, &QPushButton::clicked, this, [this] {
            const bool on = !m_status.value(QStringLiteral("powered")).toBool();
            call(QStringLiteral("bt.power"), QJsonObject{{QStringLiteral("on"), on}}, [this](const DeckLabdReply& reply) {
                if (!reply.ok) {
                    say(failure(reply), true);
                }
                refresh();
            });
        });
        auto* warning = new QFrame(m_content);
        warning->setProperty("warning", true);
        warning->setObjectName(QStringLiteral("decklab-bt-latency-warning"));
        auto* warningLayout = vbox(warning, 12, 2);
        auto* warningTitle = new QLabel(tr("Bluetooth audio is high latency – for practice, not mixing."), warning);
        warningTitle->setWordWrap(true);
        warningTitle->setStyleSheet(QStringLiteral("font-weight:600;font-size:16px;"));
        warningLayout->addWidget(warningTitle);
        warningLayout->addWidget(new QLabel(tr("Speakers and headphones lag behind the waveforms, so beatmatching by ear "
                                               "won't line up. Use your controller's outputs for real mixing."),
                warning));
        static_cast<QLabel*>(warningLayout->itemAt(1)->widget())->setWordWrap(true);
        m_layout->addWidget(warning);
        auto* listHeader = hbox(8);
        m_summary = muted(QString(), m_content);
        listHeader->addWidget(m_summary, 1);
        m_scan = button(tr("Find speakers"), m_content, QStringLiteral("decklab-bt-scan"), 44);
        m_scan->setFixedWidth(150);
        connect(m_scan, &QPushButton::clicked, this, [this] {
            m_scan->setEnabled(false);
            m_scan->setText(tr("Searching…"));
            call(QStringLiteral("bt.scan"), QJsonObject{{QStringLiteral("seconds"), 15}}, [this](const DeckLabdReply& reply) {
                if (!reply.ok) {
                    say(failure(reply), true);
                }
                QTimer::singleShot(reply.ok ? 15000 : 0, this, [this] {
                    m_scan->setEnabled(true);
                    m_scan->setText(tr("Find speakers"));
                });
            });
        });
        listHeader->addWidget(m_scan);
        m_layout->addLayout(listHeader);
        m_list = new QWidget(m_content);
        m_listLayout = vbox(m_list, 0, 0);
        m_layout->addWidget(m_list);
        m_hint = muted(tr("To play through a connected speaker, open Deck Setup → Audio and choose "
                          "“System audio (Bluetooth)”."),
                m_content);
        m_layout->addWidget(m_hint);
        m_layout->addStretch(1);
    }

    void activate() override {
        refresh();
    }

  protected:
    void onEvent(const QString& topic, const QJsonObject& data) override {
        if (topic == QStringLiteral("bt.changed")) {
            m_busy.clear();
            const QString error = data.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                say(error, true);
            }
            apply(data);
        }
    }

  private:
    void refresh() {
        call(QStringLiteral("bt.status"), {}, [this](const DeckLabdReply& reply) {
            if (reply.ok) {
                apply(reply.result);
            } else {
                say(failure(reply), true);
            }
        });
    }

    void apply(const QJsonObject& status) {
        m_status = status;
        const bool available = status.value(QStringLiteral("available")).toBool(true);
        const bool powered = status.value(QStringLiteral("powered")).toBool();
        m_toggle->setVisible(available);
        m_toggle->setText(powered ? tr("Turn off") : tr("Turn on"));
        m_scan->setVisible(available && powered);
        clearLayout(m_listLayout);
        if (!available) {
            m_summary->setText(tr("No Bluetooth adapter found."));
            return;
        }
        if (!powered) {
            m_summary->setText(tr("Bluetooth is off."));
            return;
        }
        const QJsonArray devices = status.value(QStringLiteral("devices")).toArray();
        int connected = 0;
        for (const auto& value : devices) {
            const QJsonObject device = value.toObject();
            const QString address = device.value(QStringLiteral("address")).toString();
            const QString name = device.value(QStringLiteral("name")).toString(address);
            const bool paired = device.value(QStringLiteral("paired")).toBool();
            const bool isConnected = device.value(QStringLiteral("connected")).toBool();
            const bool audio = device.value(QStringLiteral("audio")).toBool();
            connected += isConnected;
            auto* line = new QWidget(m_list);
            auto* lineLayout = hbox(0);
            line->setLayout(lineLayout);
            const QString state = isConnected ? tr("Connected") : paired ? tr("Paired") : tr("Available");
            auto* row = new Row(name, audio ? state + tr(" · Audio") : state, line, QStringLiteral("decklab-bt-device"));
            row->setRowIcon(audio ? Icon::Speaker : Icon::Bluetooth);
            row->setSelectedRow(isConnected);
            if (isConnected) {
                row->setTags({tr("Connected")});
            }
            lineLayout->addWidget(row, 1);
            const QString method = !paired ? QStringLiteral("bt.pair")
                    : isConnected          ? QStringLiteral("bt.disconnect")
                                           : QStringLiteral("bt.connect");
            const QString label = !paired ? tr("Pair") : isConnected ? tr("Disconnect") : tr("Connect");
            const bool busy = m_busy.contains(address);
            auto* action = button(busy ? tr("Working…") : label, line, QStringLiteral("decklab-bt-action"), 56);
            action->setFixedWidth(120);
            action->setEnabled(!busy);
            connect(action, &QPushButton::clicked, this, [this, method, address] { run(method, address); });
            // A row tap only shows details: tapping the connected speaker must
            // never disconnect it. The explicit button is the only action.
            connect(row, &QPushButton::clicked, this, [this, name, address, state, audio, busy, label, method] {
                QStringList lines{state + (audio ? tr(" · Audio") : QString())};
                if (audio) {
                    lines.append(tr("Bluetooth sound arrives late. Fine for practice, not for mixing."));
                }
                lines.append(tr("Address: %1").arg(address));
                Confirm::details(window(), name, lines.join(QLatin1Char('\n')), busy ? QString() : label,
                        busy ? std::function<void()>()
                             : std::function<void()>([this, method, address] { run(method, address); }));
            });
            lineLayout->addWidget(action);
            if (paired) {
                // Hold to forget (Signature 4).
                auto* forget = new HoldButton(tr("Forget"), line, QStringLiteral("decklab-bt-forget"), 56);
                forget->setFixedWidth(112);
                connect(forget, &QPushButton::clicked, this, [this] { say(tr("Hold Forget to forget a device.")); });
                connect(forget, &HoldButton::confirmed, this, [this, address] { run(QStringLiteral("bt.forget"), address); });
                lineLayout->addWidget(forget);
            }
            m_listLayout->addWidget(line);
        }
        m_summary->setText(devices.isEmpty() ? tr("No devices yet. Put your speaker in pairing mode, then Find speakers.")
                        : connected == 1 ? tr("1 connected")
                        : connected > 1  ? tr("%1 connected").arg(connected)
                                         : tr("Devices"));
    }

    void run(const QString& method, const QString& address) {
        m_busy.insert(address);
        apply(m_status);
        call(method, QJsonObject{{QStringLiteral("address"), address}}, [this, address](const DeckLabdReply& reply) {
            if (!reply.ok) {
                m_busy.remove(address);
                say(failure(reply), true);
                apply(m_status);
            }
        });
    }

    QPushButton* m_toggle;
    QPushButton* m_scan;
    QLabel* m_summary;
    QLabel* m_hint;
    QWidget* m_list;
    QVBoxLayout* m_listLayout;
    QJsonObject m_status;
    QSet<QString> m_busy;
};

// --------------------------------------------------------------- Screen --
class ScreenPage : public SectionPage {
  public:
    explicit ScreenPage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Screen, tr("Screen")) {
        auto* panel = card(m_content);
        auto* layout = vbox(panel, 16, 12);
        auto* top = hbox(8);
        top->addWidget(title(tr("Brightness"), panel), 1);
        m_value = new QLabel(panel);
        m_value->setProperty("role", QStringLiteral("value"));
        m_value->setObjectName(QStringLiteral("decklab-brightness-value"));
        top->addWidget(m_value);
        layout->addLayout(top);
        auto* controls = hbox(10);
        auto* dimmer = button(QStringLiteral("−"), panel, QStringLiteral("decklab-brightness-down"), 48);
        dimmer->setFixedWidth(60);
        m_slider = new QSlider(Qt::Horizontal, panel);
        m_slider->setObjectName(QStringLiteral("decklab-brightness"));
        m_slider->setRange(5, 100); // never black
        m_slider->setMinimumHeight(48);
        m_slider->setAccessibleName(tr("Screen brightness"));
        auto* brighter = button(QStringLiteral("+"), panel, QStringLiteral("decklab-brightness-up"), 48);
        brighter->setFixedWidth(60);
        controls->addWidget(dimmer);
        controls->addWidget(m_slider, 1);
        controls->addWidget(brighter);
        layout->addLayout(controls);
        layout->addWidget(muted(tr("The minimum is 5% so the screen never goes dark."), panel));
        m_layout->addWidget(panel);
        m_unavailable = body(tr("This screen's brightness can't be changed from Deck Lab."), m_content);
        m_unavailable->hide();
        m_layout->addWidget(m_unavailable);
        m_layout->addStretch(1);
        m_send.setSingleShot(true);
        m_send.setInterval(120);
        connect(&m_send, &QTimer::timeout, this, [this] {
            call(QStringLiteral("display.set"), QJsonObject{{QStringLiteral("brightness"), m_slider->value()}},
                    [this](const DeckLabdReply& reply) {
                        if (!reply.ok) {
                            say(failure(reply), true);
                        }
                    });
        });
        connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
            m_value->setText(QStringLiteral("%1%").arg(value));
            if (!m_loading) {
                m_send.start();
            }
        });
        connect(dimmer, &QPushButton::clicked, this, [this] { m_slider->setValue(std::max(5, m_slider->value() - 10)); });
        connect(brighter, &QPushButton::clicked, this, [this] { m_slider->setValue(std::min(100, m_slider->value() + 10)); });
        m_panel = panel;
    }

    void activate() override {
        call(QStringLiteral("display.get"), {}, [this](const DeckLabdReply& reply) {
            const bool available = reply.ok && reply.result.value(QStringLiteral("available")).toBool();
            m_panel->setVisible(available);
            m_unavailable->setVisible(!available);
            if (available) {
                m_loading = true;
                m_slider->setValue(std::clamp(reply.result.value(QStringLiteral("brightness")).toInt(100), 5, 100));
                m_value->setText(QStringLiteral("%1%").arg(m_slider->value()));
                m_loading = false;
            }
        });
    }

  private:
    QLabel* m_value;
    QSlider* m_slider;
    QWidget* m_panel;
    QLabel* m_unavailable;
    QTimer m_send;
    bool m_loading = false;
};

// ---------------------------------------------------------- Date & time --
class TimePage : public SectionPage {
  public:
    explicit TimePage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Time, tr("Date & time")) {
        auto* clockCard = card(m_content);
        auto* clockLayout = vbox(clockCard, 16, 2);
        m_clock = new QLabel(clockCard);
        m_clock->setObjectName(QStringLiteral("decklab-time-clock"));
        // Hero readout: 36 px Barlow Semi Condensed SemiBold.
        m_clock->setProperty("role", QStringLiteral("hero"));
        clockLayout->addWidget(m_clock);
        m_date = muted(QString(), clockCard);
        clockLayout->addWidget(m_date);
        m_layout->addWidget(clockCard);
        m_zoneRow = new Row(tr("Time zone"), QString(), m_content, QStringLiteral("decklab-time-zone"));
        m_zoneRow->setRowIcon(Icon::Clock);
        m_zoneRow->setChevron(true);
        connect(m_zoneRow, &QPushButton::clicked, this, [this] { openPicker(); });
        m_layout->addWidget(m_zoneRow);
        m_ntpRow = new Row(tr("Internet time"), QString(), m_content, QStringLiteral("decklab-time-ntp"));
        m_ntpRow->setRowIcon(Icon::Wifi);
        m_ntpRow->setStatic(true);
        m_layout->addWidget(m_ntpRow);
        m_layout->addStretch(1);
        m_tick.setInterval(1000);
        connect(&m_tick, &QTimer::timeout, this, [this] { showClock(); });
        buildPicker();
    }

    void activate() override {
        refresh();
        m_tick.start();
    }
    void deactivate() override {
        m_tick.stop();
    }

  private:
    void refresh() {
        call(QStringLiteral("time.status"), {}, [this](const DeckLabdReply& reply) {
            if (!reply.ok) {
                say(failure(reply), true);
                return;
            }
            m_zone = reply.result.value(QStringLiteral("timezone")).toString();
            m_now = QDateTime::fromString(reply.result.value(QStringLiteral("now")).toString(), Qt::ISODate);
            m_since.start();
            const bool ntp = reply.result.value(QStringLiteral("ntp")).toBool();
            const bool synced = reply.result.value(QStringLiteral("synced")).toBool();
            QString city = m_zone.section(QLatin1Char('/'), -1);
            city.replace(QLatin1Char('_'), QLatin1Char(' '));
            m_zoneRow->setTexts(tr("Time zone"), m_zone.isEmpty() ? tr("Not set") : tr("%1  ·  %2").arg(city, m_zone));
            m_ntpRow->setTexts(tr("Internet time"),
                    !ntp ? tr("Off") : synced ? tr("On · clock is synchronized") : tr("On · waiting for the internet"));
            m_ntpRow->setTags(ntp && synced ? QStringList{tr("Synced")} : QStringList());
            showClock();
        });
    }

    void showClock() {
        if (!m_now.isValid()) {
            m_clock->setText(QStringLiteral("--:--"));
            m_date->clear();
            return;
        }
        const QDateTime now = m_now.addMSecs(m_since.elapsed());
        m_clock->setText(now.toString(QStringLiteral("HH:mm")));
        m_date->setText(QLocale().toString(now.date(), QStringLiteral("dddd d MMMM yyyy")));
    }

    void buildPicker() {
        m_picker = new QWidget(m_body);
        auto* layout = vbox(m_picker, 0, 6);
        layout->setContentsMargins(12, 0, 12, 8);
        auto* top = hbox(8);
        auto* back = button(tr("Done"), m_picker, QStringLiteral("decklab-time-picker-done"), 44);
        back->setFixedWidth(110);
        connect(back, &QPushButton::clicked, this, [this] { m_body->setCurrentIndex(0); });
        top->addWidget(muted(tr("Choose your region, then your city."), m_picker), 1);
        top->addWidget(back);
        layout->addLayout(top);
        auto* columns = hbox(8);
        m_regions = new QWidget(m_picker);
        m_regionLayout = vbox(m_regions, 0, 0);
        m_regionLayout->addStretch(1);
        auto* regionScroll = scrollArea(m_regions, m_picker, QStringLiteral("decklab-time-regions"));
        regionScroll->setFixedWidth(200);
        columns->addWidget(regionScroll);
        m_cities = new QWidget(m_picker);
        m_cityLayout = vbox(m_cities, 0, 0);
        columns->addWidget(scrollArea(m_cities, m_picker, QStringLiteral("decklab-time-cities")), 1);
        layout->addLayout(columns, 1);
        m_body->addWidget(m_picker);
    }

    void openPicker() {
        m_body->setCurrentWidget(m_picker);
        if (!m_zones.isEmpty()) {
            renderRegions();
            return;
        }
        call(QStringLiteral("time.timezones"), {}, [this](const DeckLabdReply& reply) {
            if (!reply.ok) {
                say(failure(reply), true);
                m_body->setCurrentIndex(0);
                return;
            }
            m_zones.clear();
            for (const auto& zone : reply.result.value(QStringLiteral("timezones")).toArray()) {
                m_zones.append(zone.toString());
            }
            renderRegions();
        });
    }

    static QString regionOf(const QString& zone) {
        return zone.contains(QLatin1Char('/')) ? zone.section(QLatin1Char('/'), 0, 0) : tr("Other");
    }

    void renderRegions() {
        clearLayout(m_regionLayout);
        QStringList regions;
        for (const auto& zone : std::as_const(m_zones)) {
            const QString region = regionOf(zone);
            if (!regions.contains(region)) {
                regions.append(region);
            }
        }
        std::sort(regions.begin(), regions.end());
        if (m_region.isEmpty() || !regions.contains(m_region)) {
            m_region = regions.contains(regionOf(m_zone)) ? regionOf(m_zone) : regions.value(0);
        }
        for (const auto& region : std::as_const(regions)) {
            auto* row = new Row(region, QString(), m_regions, QStringLiteral("decklab-time-region"));
            row->setSelectedRow(region == m_region);
            connect(row, &QPushButton::clicked, this, [this, region] {
                m_region = region;
                renderRegions();
            });
            m_regionLayout->addWidget(row);
        }
        m_regionLayout->addStretch(1);
        renderCities();
    }

    void renderCities() {
        clearLayout(m_cityLayout);
        for (const auto& zone : std::as_const(m_zones)) {
            if (regionOf(zone) != m_region) {
                continue;
            }
            QString city = zone.contains(QLatin1Char('/')) ? zone.section(QLatin1Char('/'), 1) : zone;
            city.replace(QLatin1Char('_'), QLatin1Char(' '));
            city.replace(QLatin1Char('/'), QStringLiteral(" · "));
            auto* row = new Row(city, QString(), m_cities, QStringLiteral("decklab-time-city"));
            row->setSelectedRow(zone == m_zone);
            if (zone == m_zone) {
                row->setTags({tr("Current")});
            }
            connect(row, &QPushButton::clicked, this, [this, zone] {
                call(QStringLiteral("time.set_timezone"), QJsonObject{{QStringLiteral("timezone"), zone}},
                        [this, zone](const DeckLabdReply& reply) {
                            if (!reply.ok) {
                                say(failure(reply), true);
                                return;
                            }
                            say(tr("Time zone set to %1.").arg(zone));
                            m_body->setCurrentIndex(0);
                            refresh();
                        });
            });
            m_cityLayout->addWidget(row);
        }
        m_cityLayout->addStretch(1);
    }

    QLabel* m_clock;
    QLabel* m_date;
    Row* m_zoneRow;
    Row* m_ntpRow;
    QTimer m_tick;
    QDateTime m_now;
    QElapsedTimer m_since;
    QString m_zone;
    QString m_region;
    QStringList m_zones;
    QWidget* m_picker = nullptr;
    QWidget* m_regions = nullptr;
    QVBoxLayout* m_regionLayout = nullptr;
    QWidget* m_cities = nullptr;
    QVBoxLayout* m_cityLayout = nullptr;
};

// -------------------------------------------------------------- Storage --
class StoragePage : public SectionPage {
  public:
    explicit StoragePage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Storage, tr("Storage")) {
        auto* recording = card(m_content);
        auto* recordingLayout = vbox(recording, 14, 6);
        auto* recordingTop = hbox(10);
        auto* recordingText = new QWidget(recording);
        auto* textLayout = vbox(recordingText, 0, 2);
        textLayout->addWidget(title(tr("Mix recording"), recordingText));
        m_recordingTarget = muted(QString(), recordingText);
        m_recordingTarget->setObjectName(QStringLiteral("decklab-recording-target"));
        textLayout->addWidget(m_recordingTarget);
        recordingTop->addWidget(recordingText, 1);
        m_record = primaryButton(tr("Start recording"), recording, QStringLiteral("decklab-recording-toggle"), 48);
        m_record->setFixedWidth(170);
        connect(m_record, &QPushButton::clicked, this, [] {
            ControlObject::set(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("toggle_recording")), 1);
        });
        recordingTop->addWidget(m_record);
        recordingLayout->addLayout(recordingTop);
        m_layout->addWidget(recording);
        m_layout->addWidget(sectionLabel(tr("USB DRIVES"), m_content));
        m_drives = new QWidget(m_content);
        m_driveLayout = vbox(m_drives, 0, 8);
        m_layout->addWidget(m_drives);
        m_layout->addStretch(1);
        m_poll.setInterval(1000);
        connect(&m_poll, &QTimer::timeout, this, [this] { updateRecording(); });
    }

    void activate() override {
        render();
        m_poll.start();
        if (m_system) {
            call(QStringLiteral("usb.list"), {}, [](const DeckLabdReply&) {});
        }
    }
    void deactivate() override {
        m_poll.stop();
    }

  protected:
    void onEvent(const QString& topic, const QJsonObject& data) override {
        if (topic == QStringLiteral("usb.changed")) {
            const QString error = data.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                say(error, true);
            } else if (data.value(QStringLiteral("op")).toString() == QStringLiteral("eject")) {
                say(tr("Ejected. You can remove the drive now."));
            }
        }
    }
    void connectSystem() override {
        connect(m_system.data(), &DeckLabSystem::usbChanged, this, [this](const QJsonArray&) {
            if (isVisible()) {
                render();
            }
        });
    }

  private:
    void updateRecording() {
        const double status = ControlObject::get(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("status")));
        const bool recording = status > 0;
        const QString directory = m_system ? m_system->recordingDirectory() : QString();
        bool onUsb = false;
        if (m_system) {
            for (const auto& value : m_system->usbDrives()) {
                const QJsonObject drive = value.toObject();
                onUsb |= decklab::isPathOnMount(directory, drive.value(QStringLiteral("mount")).toString()) &&
                        !drive.value(QStringLiteral("read_only")).toBool();
            }
        }
        m_record->setText(recording ? tr("Stop recording") : tr("Start recording"));
        m_record->setEnabled(recording || onUsb);
        m_recordingTarget->setText(recording ? tr("Recording to %1").arg(directory)
                        : onUsb            ? tr("Saves WAV files to %1").arg(directory)
                                           : tr("Choose “Use for recordings” on a USB drive below."));
    }

    void render() {
        clearLayout(m_driveLayout);
        updateRecording();
        if (!m_system) {
            return;
        }
        const QJsonArray drives = m_system->usbDrives();
        if (drives.isEmpty()) {
            auto* empty = body(tr("No USB drive connected. Plug one in to play music from it, use your "
                                  "Rekordbox playlists or record your mix."),
                    m_drives);
            m_driveLayout->addWidget(empty);
            return;
        }
        const QString directory = m_system->recordingDirectory();
        for (const auto& value : drives) {
            const QJsonObject drive = value.toObject();
            const QString id = drive.value(QStringLiteral("id")).toString();
            const QString mount = drive.value(QStringLiteral("mount")).toString();
            QString label = drive.value(QStringLiteral("label")).toString();
            if (label.isEmpty()) {
                label = tr("USB drive");
            }
            const bool readOnly = drive.value(QStringLiteral("read_only")).toBool(true);
            const bool mounted = drive.value(QStringLiteral("mounted")).toBool(true);
            const bool recordingHere = decklab::isPathOnMount(directory, mount) && !readOnly;
            auto* panel = card(m_drives);
            panel->setObjectName(QStringLiteral("decklab-drive"));
            auto* layout = vbox(panel, 12, 8);
            auto* row = new Row(label,
                    tr("%1 free of %2  ·  %3")
                            .arg(formatMegabytes(drive.value(QStringLiteral("free_mb")).toDouble(-1)),
                                    formatMegabytes(drive.value(QStringLiteral("size_mb")).toDouble(-1)),
                                    drive.value(QStringLiteral("fs")).toString().toUpper()),
                    panel,
                    QStringLiteral("decklab-drive-row"));
            row->setRowIcon(Icon::Usb);
            row->setStatic(true);
            QStringList tags;
            if (recordingHere) {
                tags.append(tr("Recordings"));
            }
            if (drive.value(QStringLiteral("rekordbox")).toBool()) {
                tags.append(tr("rekordbox export"));
            }
            tags.append(readOnly ? tr("Read-only") : tr("Writable"));
            if (drive.value(QStringLiteral("busy")).toBool()) {
                tags.append(tr("Busy"));
            }
            if (!mounted) {
                tags.append(tr("Not mounted"));
            }
            row->setTags(tags);
            layout->addWidget(row);
            auto* actions = hbox(8);
            auto* record = button(recordingHere ? tr("Used for recordings") : tr("Use for recordings"), panel,
                    QStringLiteral("decklab-drive-record"), 48);
            record->setEnabled(!recordingHere && mounted);
            connect(record, &QPushButton::clicked, this, [this, drive, label, readOnly] {
                if (!readOnly) {
                    useForRecordings(drive);
                    return;
                }
                // USB drives mount read-only. Never make one writable without
                // saying so; a pulled stick mid-write can damage its export.
                Confirm::ask(window(), tr("Make %1 writable?").arg(label), writableNotice(drive, label),
                        tr("Make writable"), [this, drive] { useForRecordings(drive); }, false);
            });
            actions->addWidget(record, 1);
            // Hold to eject (Signature 4). Refused up front, with the reason,
            // if a loaded track lives here or a recording writes to it.
            auto* eject = new HoldButton(tr("Eject"), panel, QStringLiteral("decklab-drive-eject"), 48);
            eject->setGate([this, drive] { return m_system ? m_system->ejectRefusal(drive) : QString(); });
            connect(eject, &HoldButton::refused, this, [this](const QString& refusal) { say(refusal, true); });
            connect(eject, &QPushButton::clicked, this, [this, drive] {
                const QString refusal = m_system ? m_system->ejectRefusal(drive) : QString();
                say(refusal.isEmpty() ? tr("Hold Eject to eject the drive.") : refusal, !refusal.isEmpty());
            });
            connect(eject, &HoldButton::confirmed, this, [this, drive] {
                m_system->ejectDrive(drive, [self = QPointer<StoragePage>(this)](bool ok, const QString& text) {
                    if (self) {
                        self->say(text, !ok);
                    }
                });
            });
            actions->addWidget(eject, 1);
            layout->addLayout(actions);
            m_driveLayout->addWidget(panel);
            Q_UNUSED(id);
        }
    }

    void useForRecordings(const QJsonObject& drive) {
        if (!m_system) {
            return;
        }
        say(tr("Preparing the drive…"));
        m_system->useDriveForRecordings(drive, [self = QPointer<StoragePage>(this)](bool ok, const QString& text) {
            if (self) {
                self->say(text, !ok);
                self->render();
            }
        });
    }

    // Plain explanation for the confirmation. A drive with a rekordbox
    // export is allowed, but another drive is recommended first.
    QString writableNotice(const QJsonObject& drive, const QString& label) const {
        const bool rekordbox = drive.value(QStringLiteral("rekordbox")).toBool();
        QStringList paragraphs{rekordbox
                        ? tr("Deck Lab will write recordings to %1. Your rekordbox export isn't changed.").arg(label)
                        : tr("Deck Lab will write recordings to %1. Nothing else on it is changed.").arg(label)};
        if (rekordbox) {
            QString other;
            if (m_system) {
                const QString id = drive.value(QStringLiteral("id")).toString();
                for (const auto& value : m_system->usbDrives()) {
                    const QJsonObject candidate = value.toObject();
                    if (candidate.value(QStringLiteral("id")).toString() != id &&
                            candidate.value(QStringLiteral("mounted")).toBool(true) &&
                            !candidate.value(QStringLiteral("read_only")).toBool(true)) {
                        other = candidate.value(QStringLiteral("label")).toString(tr("USB drive"));
                        break;
                    }
                }
            }
            paragraphs.append(other.isEmpty()
                            ? tr("This drive holds your rekordbox export, so another USB drive is safer "
                                 "for recordings.")
                            : tr("This drive holds your rekordbox export, so another drive is safer for "
                                 "recordings: %1 is ready to use.")
                                      .arg(other));
        }
        paragraphs.append(tr("Eject the drive before you unplug it."));
        return paragraphs.join(QStringLiteral("\n\n"));
    }

    QLabel* m_recordingTarget;
    QPushButton* m_record;
    QWidget* m_drives;
    QVBoxLayout* m_driveLayout;
    QTimer m_poll;
};

// ---------------------------------------------------------------- Power --
// r4-system (Signature 4): hold to confirm. Hold 1.2 s while the white fill
// sweeps like a playhead; let go early and nothing happens. While anything
// plays the rows dim and say why. The result appears in place.
class PowerPage : public SectionPage {
  public:
    explicit PowerPage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Power, tr("Power")) {
        m_restart = addHold(Icon::Restart, tr("Restart app"), tr("About 15 s. Settings are kept."),
                QStringLiteral("decklab-power-restart"), QStringLiteral("restart"));
        m_reboot = addHold(Icon::Restart, tr("Restart unit"), tr("About a minute."),
                QStringLiteral("decklab-power-reboot"), QStringLiteral("reboot"));
        m_shutdown = addHold(Icon::Power, tr("Turn off"), tr("Do this before unplugging."),
                QStringLiteral("decklab-power-shutdown"), QStringLiteral("shutdown"));
        m_layout->addStretch(1);
        // While shown: dim the rows the moment a deck starts.
        m_poll.setInterval(1000);
        connect(&m_poll, &QTimer::timeout, this, [this] { refreshReasons(); });
    }

    void activate() override {
        refreshReasons();
        m_poll.start();
    }
    void deactivate() override {
        m_poll.stop();
    }

  protected:
    bool needsService() const override {
        return false;
    }

  private:
    HoldButton* addHold(Icon icon, const QString& name, const QString& detail, const QString& objectName,
            const QString& action) {
        auto* row = new HoldButton(name, m_content, objectName);
        row->setRow(icon, detail);
        row->setGate([this] { return m_owner->playingRefusal(); });
        connect(row, &HoldButton::refused, this, [this](const QString& reason) { say(reason, true); });
        connect(row, &QPushButton::clicked, this, [this] { say(tr("Hold the row to confirm.")); });
        connect(row, &HoldButton::confirmed, this, [this, row, action] {
            const QString refusal = m_owner->power(action);
            if (!refusal.isEmpty()) {
                say(refusal, true);
                return;
            }
            row->setResult(action == QStringLiteral("shutdown")
                            ? tr("Turning off… unplug when the green light stops blinking.")
                            : action == QStringLiteral("reboot") ? tr("Restarting the unit…")
                                                                 : tr("Restarting the app…"));
        });
        m_layout->addWidget(row);
        return row;
    }

    void refreshReasons() {
        const QString playing = m_owner->playingRefusal();
        const bool service = online();
        m_restart->setReason(playing);
        m_reboot->setReason(!playing.isEmpty() ? playing : service ? QString() : tr("Unavailable: system service offline."));
        m_shutdown->setReason(!playing.isEmpty() ? playing : service ? QString() : tr("Unavailable: system service offline."));
    }

    HoldButton* m_restart;
    HoldButton* m_reboot;
    HoldButton* m_shutdown;
    QTimer m_poll;
};

// ---------------------------------------------------------------- About --
// Text rendering of the Deck Lab wordmark: DECK, the red playhead, LAB.
class Wordmark : public QWidget {
  public:
    explicit Wordmark(QWidget* parent)
            : QWidget(parent) {
        setFixedHeight(56);
        setAccessibleName(QStringLiteral("Deck Lab"));
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // Bundled condensed face, never a synthesised system fallback.
        QFont mark = font();
        mark.setFamilies({QStringLiteral("Barlow Semi Condensed"), QStringLiteral("Barlow")});
        mark.setPixelSize(36);
        mark.setWeight(QFont::Bold);
        painter.setFont(mark);
        const QFontMetrics metrics(mark);
        const int cap = metrics.capHeight();
        const int baseline = (height() + cap) / 2;
        int x = 0;
        painter.setPen(QColor(kText));
        const QString deck = QStringLiteral("DECK");
        const QString lab = QStringLiteral("LAB");
        painter.drawText(x, baseline, deck);
        x += metrics.horizontalAdvance(deck) + 7;
        // The playhead is the only red in the mark.
        painter.fillRect(QRect(x, baseline - cap - 5, 4, cap + 10), QColor(kLive));
        x += 4 + 7;
        painter.drawText(x, baseline, lab);
    }
};

class AboutPage : public SectionPage {
  public:
    explicit AboutPage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::About, tr("About")) {
        m_layout->addWidget(new Wordmark(m_content));
        m_layout->addWidget(body(tr("Built on Mixxx, the free and open-source DJ software. Deck Lab is an "
                                    "independent project and isn't affiliated with or endorsed by the Mixxx team."),
                m_content));
        auto* panel = card(m_content);
        m_grid = new QGridLayout(panel);
        m_grid->setContentsMargins(16, 12, 16, 12);
        m_grid->setHorizontalSpacing(18);
        m_grid->setVerticalSpacing(9);
        m_grid->setColumnStretch(1, 1);
        // The GPL source offer: static, so the 5 s refresh never touches it.
        auto* source = card(m_content);
        source->setObjectName(QStringLiteral("decklab-about-source-card"));
        auto* sourceGrid = new QGridLayout(source);
        sourceGrid->setContentsMargins(16, 12, 16, 12);
        sourceGrid->setHorizontalSpacing(18);
        sourceGrid->setVerticalSpacing(9);
        sourceGrid->setColumnStretch(1, 1);
        const auto addSource = [source, sourceGrid](int line, const QString& name, const QString& value,
                                       const QString& objectName) {
            auto* key = muted(name, source);
            key->setWordWrap(false);
            auto* text = new QLabel(value, source);
            text->setObjectName(objectName);
            text->setProperty("role", QStringLiteral("value"));
            text->setWordWrap(true);
            text->setTextFormat(Qt::PlainText);
            text->setTextInteractionFlags(Qt::NoTextInteraction);
            sourceGrid->addWidget(key, line, 0, Qt::AlignTop);
            sourceGrid->addWidget(text, line, 1);
        };
        addSource(0, tr("Source code"), kSourceCodeUrl, QStringLiteral("decklab-about-source"));
        addSource(1, tr("Licence"), tr("GNU GPL, version 2 or later"), QStringLiteral("decklab-about-licence"));
        m_layout->addWidget(source);
        m_layout->addWidget(panel);
        auto* maintenance = new Row(tr("Maintenance"), tr("Logs, reset, desktop"), m_content,
                QStringLiteral("decklab-about-maintenance"));
        maintenance->setRowIcon(Icon::Wrench);
        maintenance->setChevron(true);
        connect(maintenance, &QPushButton::clicked, owner, [owner] { owner->showSection(Section::Maintenance); });
        m_layout->addWidget(maintenance);
        m_layout->addStretch(1);
        m_refresh.setInterval(5000);
        connect(&m_refresh, &QTimer::timeout, this, [this] { refresh(); });
        m_panel = panel;
    }

    void activate() override {
        refresh();
        m_refresh.start();
    }
    void deactivate() override {
        m_refresh.stop();
    }

  protected:
    bool needsService() const override {
        return false;
    }

  private:
    void refresh() {
        if (!online()) {
            render(QJsonObject());
            return;
        }
        call(QStringLiteral("system.info"), {}, [this](const DeckLabdReply& reply) {
            render(reply.ok ? reply.result : QJsonObject());
        });
    }

    void render(const QJsonObject& info) {
        clearLayout(m_grid);
        int line = 0;
        const auto add = [this, &line](const QString& name, const QString& value) {
            auto* key = muted(name, m_panel);
            key->setWordWrap(false);
            auto* text = new QLabel(value, m_panel);
            text->setProperty("role", QStringLiteral("value"));
            text->setWordWrap(true);
            text->setTextInteractionFlags(Qt::NoTextInteraction);
            m_grid->addWidget(key, line, 0, Qt::AlignTop);
            m_grid->addWidget(text, line, 1);
            ++line;
        };
        const QString decklab = info.value(QStringLiteral("decklab_version")).toString();
        add(tr("Deck Lab"), decklab.isEmpty() ? tr("—") : decklab);
        add(tr("DJ engine"), tr("Mixxx %1").arg(VersionStore::version()));
        const bool service = online();
        add(tr("System service"),
                service ? (m_system->client()->isReadOnly()
                                          ? tr("Connected · decklabd %1 · read-only").arg(m_system->client()->serverVersion())
                                          : tr("Connected · decklabd %1").arg(m_system->client()->serverVersion()))
                        : tr("Offline — DJing is unaffected"));
        if (!info.isEmpty()) {
            QStringList addresses;
            for (const auto& address : info.value(QStringLiteral("ip")).toArray()) {
                addresses.append(address.toString());
            }
            add(tr("Network address"), addresses.isEmpty() ? tr("Not connected") : addresses.join(QStringLiteral(", ")));
            const double temperature = info.value(QStringLiteral("temp_c")).toDouble(-1);
            const QString throttled = info.value(QStringLiteral("throttled")).toString();
            QString heat = temperature >= 0 ? tr("%1 °C").arg(temperature, 0, 'f', 1) : tr("—");
            if (!throttled.isEmpty() && throttled != QStringLiteral("0x0")) {
                heat += tr("  ·  throttled (%1)").arg(throttled);
            }
            add(tr("Temperature"), heat);
            add(tr("Running for"), uptimeText(static_cast<qint64>(info.value(QStringLiteral("uptime_s")).toDouble())));
            add(tr("Memory free"), formatMegabytes(info.value(QStringLiteral("mem_available_mb")).toDouble(-1)));
            const QJsonObject storage = info.value(QStringLiteral("storage")).toObject();
            add(tr("Internal storage free"), formatMegabytes(storage.value(QStringLiteral("data_free_mb")).toDouble(-1)));
            add(tr("Device"), tr("%1  ·  %2").arg(info.value(QStringLiteral("model")).toString(),
                                      info.value(QStringLiteral("hostname")).toString()));
            add(tr("System"), tr("%1  ·  Linux %2").arg(info.value(QStringLiteral("os")).toString(),
                                      info.value(QStringLiteral("kernel")).toString()));
        }
    }

    QGridLayout* m_grid;
    QWidget* m_panel;
    QTimer m_refresh;
};

// ---------------------------------------------------------- Maintenance --
class MaintenancePage : public SectionPage {
  public:
    explicit MaintenancePage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Maintenance, tr("Maintenance")) {
        auto* panel = card(m_content);
        auto* layout = vbox(panel, 16, 10);
        layout->addWidget(title(tr("Maintenance desktop"), panel));
        layout->addWidget(body(tr("For servicing only: updates by a technician, file repair or network "
                                  "settings that Deck Lab doesn't cover. Deck Lab closes while the desktop is open."),
                panel));
        auto* open = button(tr("Open maintenance desktop…"), panel, QStringLiteral("decklab-maintenance-desktop"), 48);
        connect(open, &QPushButton::clicked, this, [this] {
            if (m_system && m_system->anyPlaying()) {
                say(tr("Stop all decks and samplers first."), true);
                return;
            }
            Confirm::ask(window(), tr("Open the maintenance desktop?"),
                    tr("Deck Lab will close. Only continue if you know what you're doing; "
                       "restart the unit to return to Deck Lab."),
                    tr("Open desktop"), [this] {
                        call(QStringLiteral("session.desktop"), {}, [this](const DeckLabdReply& reply) {
                            say(reply.ok ? tr("Opening the desktop…") : failure(reply), !reply.ok);
                        });
                    });
        });
        layout->addWidget(open);
        m_layout->addWidget(panel);
        m_layout->addStretch(1);
        addLegacyAction(tr("Open desktop with companion"), 9);
    }
};

} // namespace

DeckLabSystemPage::DeckLabSystemPage(QWidget* parent)
        : QWidget(parent),
          m_stack(new QStackedWidget(this)) {
    setObjectName(QStringLiteral("decklab-system-page"));
    applyStyle(this);
    auto* root = vbox(this, 0, 0);
    root->addWidget(m_stack);
    auto* home = new QWidget(m_stack);
    home->setObjectName(QStringLiteral("decklab-system-home"));
    auto* homeLayout = vbox(home, 16, 8);
    homeLayout->setContentsMargins(8, 4, 16, 16);
    auto* header = hbox(8);
    auto* back = new GlyphButton(Glyph::Back, tr("Back to Settings"), home, QStringLiteral("decklab-system-home-back"));
    connect(back, &QPushButton::clicked, this, &DeckLabSystemPage::backRequested);
    header->addWidget(back);
    auto* crumb = new QLabel(tr("SETTINGS /"), home);
    crumb->setObjectName(QStringLiteral("decklab-system-crumb"));
    crumb->setStyleSheet(QStringLiteral("font-size:14px;font-weight:600;color:#9AA3AF;"));
    header->addWidget(crumb);
    header->addWidget(title(tr("Unit"), home), 1);
    // Said only when something is wrong (Y-01, Y-05).
    m_serviceState = new QLabel(home);
    m_serviceState->setObjectName(QStringLiteral("decklab-system-service-state"));
    m_serviceState->setProperty("role", QStringLiteral("caution"));
    m_serviceState->setWordWrap(false);
    header->addWidget(m_serviceState);
    homeLayout->addLayout(header);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->setContentsMargins(8, 0, 0, 0);
    // The back panel of the unit (Signature 3): eight live tiles, 4 x 2.
    const QList<std::tuple<int, QString, QString>> tiles{
            {static_cast<int>(Section::Wifi), tr("Wi-Fi"), QStringLiteral("wifi")},
            {static_cast<int>(Section::Bluetooth), tr("Bluetooth"), QStringLiteral("bluetooth")},
            {static_cast<int>(Section::Screen), tr("Screen"), QStringLiteral("screen")},
            {static_cast<int>(Section::Time), tr("Time"), QStringLiteral("time")},
            {static_cast<int>(Section::Storage), tr("USB"), QStringLiteral("storage")},
            {kHeatTile, tr("Heat"), QStringLiteral("heat")},
            {static_cast<int>(Section::Power), tr("Power"), QStringLiteral("power")},
            {static_cast<int>(Section::About), tr("About"), QStringLiteral("about")},
    };
    int index = 0;
    for (const auto& [key, name, objectKey] : tiles) {
        auto* tile = new DeckLabLiveTile(name, home, QStringLiteral("decklab-system-tile-%1").arg(objectKey));
        // Heat details live on About.
        const Section target = key == kHeatTile ? Section::About : static_cast<Section>(key);
        connect(tile, &QPushButton::clicked, this, [this, target] { showSection(target); });
        grid->addWidget(tile, index / 4, index % 4);
        m_tiles.insert(key, tile);
        ++index;
    }
    for (Section quiet : {Section::Screen, Section::Power, Section::About}) {
        m_tiles.value(static_cast<int>(quiet))->setLampVisible(false);
    }
    // Screen: brightness inline, snapped to 10s, with booth presets (Y-27, Y-28).
    auto* screen = m_tiles.value(static_cast<int>(Section::Screen));
    auto* steps = hbox(4);
    auto* dimmer = new GlyphButton(Glyph::Minus, tr("Dimmer"), screen, QStringLiteral("decklab-tile-brightness-down"));
    auto* brighter = new GlyphButton(Glyph::Plus, tr("Brighter"), screen, QStringLiteral("decklab-tile-brightness-up"));
    dimmer->setMinimumWidth(44);
    dimmer->setMaximumWidth(QWIDGETSIZE_MAX);
    brighter->setMinimumWidth(44);
    brighter->setMaximumWidth(QWIDGETSIZE_MAX);
    dimmer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    brighter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    steps->addWidget(dimmer);
    steps->addWidget(brighter);
    auto* presets = hbox(4);
    auto* booth = button(tr("Booth"), screen, QStringLiteral("decklab-tile-brightness-booth"), 44);
    auto* day = button(tr("Daylight"), screen, QStringLiteral("decklab-tile-brightness-day"), 44);
    presets->addWidget(booth);
    presets->addWidget(day);
    screen->controls()->addLayout(steps);
    screen->controls()->addLayout(presets);
    connect(dimmer, &QPushButton::clicked, this, [this] {
        setBrightness(m_brightness <= 10 ? 5 : ((m_brightness + 9) / 10 - 1) * 10);
    });
    connect(brighter, &QPushButton::clicked, this, [this] { setBrightness(std::min(100, (m_brightness / 10 + 1) * 10)); });
    connect(booth, &QPushButton::clicked, this, [this] { setBrightness(30); });
    connect(day, &QPushButton::clicked, this, [this] { setBrightness(100); });
    m_brightnessSend.setSingleShot(true);
    m_brightnessSend.setInterval(120);
    connect(&m_brightnessSend, &QTimer::timeout, this, [this] {
        if (m_system && m_system->isReady() && m_brightness >= 5) {
            QPointer<DeckLabSystemPage> self(this);
            const quint32 edit = m_brightnessEdit;
            m_system->client()->call(QStringLiteral("display.set"),
                    QJsonObject{{QStringLiteral("brightness"), m_brightness}}, [self, edit](const DeckLabdReply&) {
                        // Settled once the service has the latest tap.
                        if (self && self->m_brightnessEdit == edit) {
                            self->m_brightnessPending = false;
                        }
                    });
        } else {
            m_brightnessPending = false;
        }
    });
    // Power: hold to restart or turn off, right here.
    auto* powerTile = m_tiles.value(static_cast<int>(Section::Power));
    for (const auto& [label, action, name] : {std::tuple{tr("Restart app"), QStringLiteral("restart"),
                                                      QStringLiteral("decklab-tile-power-restart")},
                 std::tuple{tr("Turn off"), QStringLiteral("shutdown"), QStringLiteral("decklab-tile-power-off")}}) {
        auto* hold = new HoldButton(label, powerTile, name, 44);
        hold->setGate([this] { return playingRefusal(); });
        connect(hold, &HoldButton::refused, this, [hold](const QString& reason) { hold->setResult(reason); });
        connect(hold, &HoldButton::confirmed, this, [this, hold, action = action] {
            const QString refusal = power(action);
            hold->setResult(refusal.isEmpty() ? (action == QStringLiteral("shutdown") ? tr("Turning off…")
                                                                                       : tr("Restarting…"))
                                              : refusal);
        });
        powerTile->controls()->addWidget(hold);
        m_powerHolds.append(hold);
    }
    homeLayout->addLayout(grid, 1);
    m_clockTimer.setSingleShot(true);
    connect(&m_clockTimer, &QTimer::timeout, this, [this] { refreshClock(); });
    m_stack->addWidget(home);
    const QList<QWidget*> sections{new WifiPage(this),
            new BluetoothPage(this),
            new ScreenPage(this),
            new TimePage(this),
            new StoragePage(this),
            new PowerPage(this),
            new AboutPage(this),
            new MaintenancePage(this)};
    for (auto* page : sections) {
        auto* section = static_cast<SectionPage*>(page);
        m_pages.insert(static_cast<int>(section->section()), page);
        m_stack->addWidget(page);
    }
    refreshTiles();
}

DeckLabSystem* DeckLabSystemPage::system() const {
    return m_system.data();
}

QString DeckLabSystemPage::sectionName(Section section) {
    return sectionKey(section);
}

void DeckLabSystemPage::setEmbedded(bool embedded) {
    m_embedded = embedded;
    setObjectName(embedded ? QStringLiteral("decklab-system-page-embedded") : QStringLiteral("decklab-system-page"));
    const auto backs = findChildren<QPushButton*>(QStringLiteral("decklab-system-back"));
    for (auto* back : backs) {
        back->setVisible(!embedded);
    }
    for (auto* crumb : findChildren<QLabel*>(QStringLiteral("decklab-system-crumb"))) {
        crumb->setVisible(!embedded);
    }
}

void DeckLabSystemPage::setSystem(DeckLabSystem* system) {
    m_system = system;
    for (auto* page : std::as_const(m_pages)) {
        static_cast<SectionPage*>(page)->setSystem(system);
    }
    if (system) {
        connect(system, &DeckLabSystem::readyChanged, this, [this](bool) { refreshTiles(); });
        connect(system, &DeckLabSystem::usbChanged, this, [this](const QJsonArray&) { refreshTiles(); });
        connect(system, &DeckLabSystem::thermalChanged, this, [this] { refreshTiles(); });
        connect(system->client(), &DeckLabdClient::eventReceived, this, [this](const QString& topic, const QJsonObject&) {
            if (m_current == Section::Home && isVisible() &&
                    (topic.startsWith(QStringLiteral("wifi.")) || topic.startsWith(QStringLiteral("bt.")) ||
                            topic.startsWith(QStringLiteral("thermal.")))) {
                refreshTiles();
            }
        });
    }
    refreshTiles();
}

void DeckLabSystemPage::showSection(Section section) {
    m_current = section;
    if (section == Section::Home) {
        m_stack->setCurrentIndex(0);
        refreshTiles();
    } else if (auto* page = m_pages.value(static_cast<int>(section))) {
        m_stack->setCurrentWidget(page);
    }
    emit sectionChanged(section);
}

void DeckLabSystemPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshTiles();
}

void DeckLabSystemPage::hideEvent(QHideEvent* event) {
    m_clockTimer.stop();
    QWidget::hideEvent(event);
}

QWidget* DeckLabSystemPage::tile(Section section) const {
    return m_tiles.value(static_cast<int>(section));
}

QString DeckLabSystemPage::playingRefusal() const {
    return m_system && m_system->anyPlaying() ? tr("Stop all decks and samplers first.") : QString();
}

QString DeckLabSystemPage::power(const QString& action) {
    // Checked again here: a controller may have started a deck meanwhile.
    const QString playing = playingRefusal();
    if (!playing.isEmpty()) {
        return playing;
    }
    if (!m_system) {
        return tr("Unavailable: system service offline.");
    }
    if (action == QStringLiteral("restart")) {
        m_system->restartDeckLab();
        return QString();
    }
    if (!m_system->isReady()) {
        return tr("Unavailable: system service offline.");
    }
    m_system->client()->call(QStringLiteral("system.power"), QJsonObject{{QStringLiteral("action"), action}},
            [](const DeckLabdReply&) {});
    return QString();
}

void DeckLabSystemPage::setBrightness(int value) {
    if (m_brightness < 0) {
        return;
    }
    // Never below 5 %, so the screen can't go black.
    m_brightness = std::clamp(value, 5, 100);
    m_brightnessPending = true;
    ++m_brightnessEdit;
    if (auto* screen = m_tiles.value(static_cast<int>(Section::Screen))) {
        screen->set(Lamp::Off, QStringLiteral("%1 %").arg(m_brightness), QString(), {});
    }
    m_brightnessSend.start();
}

void DeckLabSystemPage::refreshClock() {
    auto* time = m_tiles.value(static_cast<int>(Section::Time));
    if (!time) {
        return;
    }
    if (!m_now.isValid()) {
        time->set(Lamp::Off, QStringLiteral("--:--"), QString(), {tr("Time not known yet")});
        return;
    }
    const QDateTime now = m_now.addMSecs(QDateTime::currentMSecsSinceEpoch() - m_nowAt);
    QString city = m_zone.section(QLatin1Char('/'), -1);
    city.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QString area = m_zone.contains(QLatin1Char('/')) ? m_zone.section(QLatin1Char('/'), 0, 0) : QString();
    time->set(time->property("synced").toBool() ? Lamp::Go : Lamp::Caution,
            now.toString(QStringLiteral("HH:mm")),
            QString(),
            {QLocale().toString(now.date(), QStringLiteral("ddd d MMM")),
                    m_zone.isEmpty() ? tr("Time zone not set") : area.isEmpty() ? city : tr("%1 (%2)").arg(city, area)});
    if (isVisible()) {
        // Next minute boundary, only while the home is shown.
        m_clockTimer.start(std::max(1000, 60000 - now.time().second() * 1000 - now.time().msec()));
    }
}

void DeckLabSystemPage::refreshTiles() {
    const bool online = m_system && m_system->isReady();
    m_serviceState->setText(online ? QString() : tr("System service offline · Wi-Fi, Bluetooth and power are paused"));
    m_serviceState->setVisible(!online && m_system);
    const auto tile = [this](int key) { return m_tiles.value(key); };
    const QString unavailable = tr("Unavailable");
    if (!online) {
        for (Section section : {Section::Wifi, Section::Bluetooth, Section::Screen, Section::Time}) {
            tile(static_cast<int>(section))->set(Lamp::Caution, QString(), QString(), {unavailable});
        }
        tile(static_cast<int>(Section::Wifi))->setSignal(-1);
        tile(kHeatTile)->set(Lamp::Off, QString(), QString(), {unavailable});
    }
    for (auto* hold : std::as_const(m_powerHolds)) {
        hold->setResult(QString());
        hold->setReason(!online && hold->objectName().endsWith(QStringLiteral("off")) ? tr("Unavailable") : QString());
    }
    tile(static_cast<int>(Section::Power))->set(Lamp::Off, QString(), QString(), {});
    tile(static_cast<int>(Section::About))->set(Lamp::Off, QString(), tr("Deck Lab"), {tr("Source & licences")});
    // USB: a capacity bar per drive, the recording drive marked.
    const QJsonArray drives = online ? m_system->usbDrives() : QJsonArray();
    QList<DeckLabLiveTile::Bar> bars;
    const QString recording = m_system ? m_system->recordingDirectory() : QString();
    for (const auto& value : drives) {
        if (bars.size() == 2) {
            break;
        }
        const QJsonObject drive = value.toObject();
        DeckLabLiveTile::Bar bar;
        bar.name = drive.value(QStringLiteral("label")).toString(tr("USB drive"));
        const double size = drive.value(QStringLiteral("size_mb")).toDouble(0);
        const double free = drive.value(QStringLiteral("free_mb")).toDouble(0);
        bar.used = size > 0 ? 1.0 - free / size : 0;
        bar.locked = drive.value(QStringLiteral("read_only")).toBool(true);
        bar.record = !bar.locked && decklab::isPathOnMount(recording, drive.value(QStringLiteral("mount")).toString());
        // WAV, 44.1 kHz stereo: about 606 MB an hour.
        bar.detail = bar.record ? tr("REC · %1 h free").arg(std::floor(free / 606.0))
                : bar.locked    ? tr("Read-only · %1 free").arg(formatMegabytes(free))
                                : tr("%1 free").arg(formatMegabytes(free));
        bars.append(bar);
    }
    auto* usb = tile(static_cast<int>(Section::Storage));
    usb->setBars(bars);
    usb->set(drives.isEmpty() ? Lamp::Off : Lamp::Go, QString(), QString(),
            drives.isEmpty() ? QStringList{online ? tr("No USB drive") : unavailable}
                    : drives.size() > 2 ? QStringList{tr("+%1 more").arg(drives.size() - 2)}
                                        : QStringList{});
    if (!online || !isVisible()) {
        return;
    }
    // Heat: from the cached thermal state, refined by system.info below.
    const QJsonObject thermal = m_system->thermal();
    const auto heat = [this, tile](double temperature, const QString& level, bool lowPower) {
        tile(kHeatTile)->set(lowPower || level == QStringLiteral("hot") || level == QStringLiteral("warm") ? Lamp::Caution
                                                                                                           : Lamp::Go,
                temperature >= 0 ? tr("%1 °C").arg(std::round(temperature)) : QString(),
                QString(),
                {lowPower                         ? tr("Low power · check the supply")
                                : level == QStringLiteral("hot")  ? tr("Hot · give it air")
                                : level == QStringLiteral("warm") ? tr("Warm")
                                                                  : tr("Fine")});
    };
    heat(thermal.value(QStringLiteral("temp_c")).toDouble(-1), thermal.value(QStringLiteral("level")).toString(),
            m_system->undervoltage());
    QPointer<DeckLabSystemPage> self(this);
    auto* client = m_system->client();
    client->call(QStringLiteral("wifi.status"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        const auto& status = reply.result;
        auto* wifi = self->m_tiles.value(static_cast<int>(Section::Wifi));
        if (!status.value(QStringLiteral("enabled")).toBool(true)) {
            wifi->setSignal(-1);
            wifi->set(Lamp::Off, QString(), tr("Off"), {});
        } else if (status.value(QStringLiteral("connected")).toBool()) {
            const int signal = status.value(QStringLiteral("signal")).toInt(-1);
            wifi->setSignal(std::max(0, signal));
            wifi->set(Lamp::Go, QString(), status.value(QStringLiteral("ssid")).toString(),
                    {signal >= 0 ? tr("Signal %1 %").arg(signal) : tr("Connected")});
        } else {
            wifi->setSignal(0);
            wifi->set(Lamp::Off, QString(), tr("Not connected"), {tr("Tap to choose a network")});
        }
    });
    client->call(QStringLiteral("bt.status"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        auto* bluetooth = self->m_tiles.value(static_cast<int>(Section::Bluetooth));
        if (!reply.result.value(QStringLiteral("available")).toBool(true)) {
            bluetooth->set(Lamp::Off, QString(), tr("None"), {tr("This unit has no Bluetooth")});
            return;
        }
        if (!reply.result.value(QStringLiteral("powered")).toBool()) {
            bluetooth->set(Lamp::Off, QString(), tr("Off"), {});
            return;
        }
        QString connected;
        for (const auto& device : reply.result.value(QStringLiteral("devices")).toArray()) {
            if (device.toObject().value(QStringLiteral("connected")).toBool()) {
                connected = device.toObject().value(QStringLiteral("name")).toString();
            }
        }
        // "On · high latency" with nothing connected read like a fault (Y-03).
        bluetooth->set(connected.isEmpty() ? Lamp::On : Lamp::Go, QString(),
                connected.isEmpty() ? tr("On") : connected,
                {connected.isEmpty() ? tr("Nothing connected") : tr("Connected · arrives late")});
    });
    client->call(QStringLiteral("display.get"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        auto* screen = self->m_tiles.value(static_cast<int>(Section::Screen));
        const bool available = reply.result.value(QStringLiteral("available")).toBool();
        for (auto* control : screen->findChildren<QPushButton*>()) {
            control->setVisible(available);
        }
        if (!available) {
            self->m_brightness = -1;
            screen->set(Lamp::Off, QString(), tr("Fixed"), {tr("This screen sets its own brightness")});
            return;
        }
        if (self->m_brightnessPending) {
            // round4: this reply predates a Dimmer or Brighter tap still on
            // its way; the old value must not overwrite (and then resend) it.
            return;
        }
        self->m_brightness = std::clamp(reply.result.value(QStringLiteral("brightness")).toInt(100), 5, 100);
        screen->set(Lamp::Off, QStringLiteral("%1 %").arg(self->m_brightness), QString(), {});
    });
    client->call(QStringLiteral("time.status"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        self->m_zone = reply.result.value(QStringLiteral("timezone")).toString();
        self->m_now = QDateTime::fromString(reply.result.value(QStringLiteral("now")).toString(), Qt::ISODate);
        self->m_nowAt = QDateTime::currentMSecsSinceEpoch();
        self->m_tiles.value(static_cast<int>(Section::Time))
                ->setProperty("synced",
                        reply.result.value(QStringLiteral("ntp")).toBool() &&
                                reply.result.value(QStringLiteral("synced")).toBool());
        self->refreshClock();
    });
    client->call(QStringLiteral("system.info"), {}, [self, heat](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        const QString version = reply.result.value(QStringLiteral("decklab_version")).toString();
        if (!version.isEmpty()) {
            self->m_tiles.value(static_cast<int>(Section::About))
                    ->set(Lamp::Off, QString(), version, {tr("Source & licences")});
        }
        const QJsonObject thermal = self->m_system ? self->m_system->thermal() : QJsonObject();
        heat(reply.result.value(QStringLiteral("temp_c")).toDouble(-1),
                thermal.value(QStringLiteral("level")).toString(),
                reply.result.value(QStringLiteral("undervoltage_now")).toBool() ||
                        (self->m_system && self->m_system->undervoltage()));
    });
}
