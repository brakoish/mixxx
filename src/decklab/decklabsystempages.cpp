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
#include "decklab/decklabui.h"
#include "moc_decklabsystempages.cpp"
#include "util/versionstore.h"

using namespace decklab::ui;
using Section = DeckLabSystemPage::Section;

namespace {

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
        headerRow->setContentsMargins(4, 4, 12, 4);
        header->setLayout(headerRow);
        auto* back = button(tr("‹  System"), header, QStringLiteral("decklab-system-back"), 44);
        back->setProperty("quiet", true);
        back->setFixedWidth(120);
        connect(back, &QPushButton::clicked, owner, [owner] { owner->showSection(Section::Home); });
        headerRow->addWidget(back);
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
        listHeader->addWidget(muted(tr("NETWORKS"), m_content), 1);
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
                auto* forget = button(tr("Forget"), line, QStringLiteral("decklab-wifi-forget"), 56);
                forget->setFixedWidth(96);
                connect(forget, &QPushButton::clicked, this, [this, ssid] {
                    Confirm::ask(window(), tr("Forget %1?").arg(ssid),
                            tr("Deck Lab won't join this network automatically any more."),
                            tr("Forget"), [this, ssid] {
                                call(QStringLiteral("wifi.forget"), QJsonObject{{QStringLiteral("ssid"), ssid}},
                                        [this, ssid](const DeckLabdReply& reply) {
                                            say(reply.ok ? tr("Forgot %1.").arg(ssid) : failure(reply), !reply.ok);
                                            refreshStatus();
                                            scan();
                                        });
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
        warningTitle->setStyleSheet(QStringLiteral("font-weight:600;font-size:14px;"));
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
            auto* action = button(m_busy.contains(address) ? tr("Working…") : label, line,
                    QStringLiteral("decklab-bt-action"), 56);
            action->setFixedWidth(120);
            action->setEnabled(!m_busy.contains(address));
            connect(action, &QPushButton::clicked, this, [this, method, address] { run(method, address); });
            connect(row, &QPushButton::clicked, action, &QPushButton::click);
            lineLayout->addWidget(action);
            if (paired) {
                auto* forget = button(tr("Forget"), line, QStringLiteral("decklab-bt-forget"), 56);
                forget->setFixedWidth(96);
                connect(forget, &QPushButton::clicked, this, [this, address, name] {
                    Confirm::ask(window(), tr("Forget %1?").arg(name),
                            tr("You'll need to pair it again to use it."), tr("Forget"),
                            [this, address] { run(QStringLiteral("bt.forget"), address); });
                });
                lineLayout->addWidget(forget);
            }
            m_listLayout->addWidget(line);
        }
        m_summary->setText(devices.isEmpty() ? tr("No devices yet. Put your speaker in pairing mode, then Find speakers.")
                        : connected      ? tr("%n connected", nullptr, connected)
                                         : tr("DEVICES"));
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
        m_clock->setStyleSheet(QStringLiteral("font-size:40px;font-weight:300;color:#ffffff;"));
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
        m_ntpRow->setEnabled(false);
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
        m_layout->addWidget(muted(tr("USB DRIVES"), m_content));
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
            row->setEnabled(false);
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
            connect(record, &QPushButton::clicked, this, [this, drive] {
                say(tr("Preparing the drive…"));
                m_system->useDriveForRecordings(drive, [self = QPointer<StoragePage>(this)](bool ok, const QString& text) {
                    if (self) {
                        self->say(text, !ok);
                        self->render();
                    }
                });
            });
            actions->addWidget(record, 1);
            auto* eject = button(tr("Eject"), panel, QStringLiteral("decklab-drive-eject"), 48);
            connect(eject, &QPushButton::clicked, this, [this, drive] {
                // Refuse up front, with the reason, if a loaded track lives here.
                const QString refusal = m_system->ejectRefusal(drive);
                if (!refusal.isEmpty()) {
                    say(refusal, true);
                    return;
                }
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

    QLabel* m_recordingTarget;
    QPushButton* m_record;
    QWidget* m_drives;
    QVBoxLayout* m_driveLayout;
    QTimer m_poll;
};

// ---------------------------------------------------------------- Power --
class PowerPage : public SectionPage {
  public:
    explicit PowerPage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::Power, tr("Power")) {
        m_restart = addPowerAction(Icon::Decks, tr("Restart Deck Lab"),
                tr("Reload the DJ software. Takes about 15 seconds."), QStringLiteral("decklab-power-restart"),
                [this] {
                    ask(tr("Restart Deck Lab?"), tr("The DJ software closes and opens again. Your settings are saved."),
                            tr("Restart"), [this] {
                                if (m_system) {
                                    m_system->restartDeckLab();
                                }
                            });
                });
        m_reboot = addPowerAction(Icon::Power, tr("Reboot"), tr("Restart the whole unit."),
                QStringLiteral("decklab-power-reboot"), [this] {
                    ask(tr("Reboot Deck Lab?"), tr("The unit restarts. This takes about a minute."), tr("Reboot"),
                            [this] { power(QStringLiteral("reboot")); });
                });
        m_shutdown = addPowerAction(Icon::Power, tr("Shut down"), tr("Turn Deck Lab off safely before unplugging it."),
                QStringLiteral("decklab-power-shutdown"), [this] {
                    ask(tr("Shut down Deck Lab?"),
                            tr("The screen goes dark. Wait until the green light stops blinking, then unplug the power."),
                            tr("Shut down"), [this] { power(QStringLiteral("shutdown")); });
                });
        m_layout->addWidget(muted(tr("These are refused while any deck or sampler is playing."), m_content));
        m_layout->addStretch(1);
    }

    void activate() override {
        const bool service = online();
        m_reboot->setEnabled(service);
        m_shutdown->setEnabled(service);
        m_reboot->setTexts(tr("Reboot"), service ? tr("Restart the whole unit.") : tr("Needs the system service."));
        m_shutdown->setTexts(tr("Shut down"),
                service ? tr("Turn Deck Lab off safely before unplugging it.") : tr("Needs the system service."));
    }

  protected:
    bool needsService() const override {
        return false;
    }

  private:
    Row* addPowerAction(Icon icon, const QString& name, const QString& detail, const QString& objectName,
            std::function<void()> action) {
        auto* row = new Row(name, detail, m_content, objectName);
        row->setRowIcon(icon);
        row->setChevron(true);
        connect(row, &QPushButton::clicked, this, std::move(action));
        m_layout->addWidget(row);
        return row;
    }

    void ask(const QString& heading, const QString& text, const QString& confirm, std::function<void()> action) {
        if (m_system && m_system->anyPlaying()) {
            say(tr("Stop all decks and samplers first."), true);
            return;
        }
        Confirm::ask(window(), heading, text, confirm, [this, action] {
            // Check again: a controller may have started a deck meanwhile.
            if (m_system && m_system->anyPlaying()) {
                say(tr("Stop all decks and samplers first."), true);
                return;
            }
            action();
        });
    }

    void power(const QString& action) {
        call(QStringLiteral("system.power"), QJsonObject{{QStringLiteral("action"), action}},
                [this, action](const DeckLabdReply& reply) {
                    say(reply.ok ? (action == QStringLiteral("reboot") ? tr("Rebooting…") : tr("Shutting down…"))
                                 : failure(reply),
                            !reply.ok);
                });
    }

    Row* m_restart;
    Row* m_reboot;
    Row* m_shutdown;
};

// ---------------------------------------------------------------- About --
class AboutPage : public SectionPage {
  public:
    explicit AboutPage(DeckLabSystemPage* owner)
            : SectionPage(owner, Section::About, tr("About")) {
        auto* panel = card(m_content);
        m_grid = new QGridLayout(panel);
        m_grid->setContentsMargins(16, 12, 16, 12);
        m_grid->setHorizontalSpacing(18);
        m_grid->setVerticalSpacing(9);
        m_grid->setColumnStretch(1, 1);
        m_layout->addWidget(panel);
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
    auto* homeLayout = vbox(home, 14, 10);
    homeLayout->setContentsMargins(14, 10, 14, 14);
    auto* header = hbox(8);
    header->addWidget(title(tr("System"), home), 1);
    m_serviceState = muted(QString(), home);
    m_serviceState->setObjectName(QStringLiteral("decklab-system-service-state"));
    m_serviceState->setWordWrap(false);
    header->addWidget(m_serviceState);
    homeLayout->addLayout(header);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    const QList<std::tuple<Section, Icon, QString>> tiles{
            {Section::Wifi, Icon::Wifi, tr("Wi-Fi")},
            {Section::Bluetooth, Icon::Bluetooth, tr("Bluetooth")},
            {Section::Screen, Icon::Screen, tr("Screen")},
            {Section::Time, Icon::Clock, tr("Date & time")},
            {Section::Storage, Icon::Usb, tr("Storage")},
            {Section::Power, Icon::Power, tr("Power")},
            {Section::About, Icon::Info, tr("About")},
            {Section::Maintenance, Icon::Wrench, tr("Maintenance")},
    };
    int index = 0;
    for (const auto& [section, icon, name] : tiles) {
        auto* tile = new Tile(icon, name, home, QStringLiteral("decklab-system-tile-%1").arg(sectionKey(section)));
        const Section target = section;
        connect(tile, &QPushButton::clicked, this, [this, target] { showSection(target); });
        grid->addWidget(tile, index / 2, index % 2);
        m_tiles.insert(static_cast<int>(section), tile);
        ++index;
    }
    homeLayout->addLayout(grid);
    homeLayout->addStretch(1);
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
}

void DeckLabSystemPage::setSystem(DeckLabSystem* system) {
    m_system = system;
    for (auto* page : std::as_const(m_pages)) {
        static_cast<SectionPage*>(page)->setSystem(system);
    }
    if (system) {
        connect(system, &DeckLabSystem::readyChanged, this, [this](bool) { refreshTiles(); });
        connect(system, &DeckLabSystem::usbChanged, this, [this](const QJsonArray&) { refreshTiles(); });
        connect(system->client(), &DeckLabdClient::eventReceived, this, [this](const QString& topic, const QJsonObject&) {
            if (m_current == Section::Home && isVisible() &&
                    (topic.startsWith(QStringLiteral("wifi.")) || topic.startsWith(QStringLiteral("bt.")))) {
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
    QWidget::hideEvent(event);
}

void DeckLabSystemPage::refreshTiles() {
    const bool online = m_system && m_system->isReady();
    m_serviceState->setText(online ? tr("System service connected") : tr("System service offline"));
    const QString offline = tr("Offline");
    const auto tile = [this](Section section) { return m_tiles.value(static_cast<int>(section)); };
    for (Section section : {Section::Wifi, Section::Bluetooth, Section::Screen, Section::Time, Section::Maintenance}) {
        tile(section)->setSubtitle(offline);
    }
    tile(Section::Power)->setSubtitle(tr("Restart, reboot or shut down"));
    tile(Section::About)->setSubtitle(tr("Mixxx %1").arg(VersionStore::version()));
    if (!online) {
        tile(Section::Storage)->setSubtitle(offline);
        return;
    }
    tile(Section::Maintenance)->setSubtitle(tr("Desktop for servicing"));
    const int drives = m_system->usbDrives().size();
    tile(Section::Storage)->setSubtitle(drives == 0 ? tr("No USB drive") : tr("%n USB drive(s)", nullptr, drives));
    if (!isVisible()) {
        return;
    }
    QPointer<DeckLabSystemPage> self(this);
    auto* client = m_system->client();
    client->call(QStringLiteral("wifi.status"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        const auto& status = reply.result;
        auto* wifi = self->m_tiles.value(static_cast<int>(Section::Wifi));
        if (!status.value(QStringLiteral("enabled")).toBool(true)) {
            wifi->setSubtitle(tr("Off"));
        } else if (status.value(QStringLiteral("connected")).toBool()) {
            wifi->setSubtitle(tr("Connected · %1").arg(status.value(QStringLiteral("ssid")).toString()));
        } else {
            wifi->setSubtitle(tr("Not connected"));
        }
    });
    client->call(QStringLiteral("bt.status"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        auto* bluetooth = self->m_tiles.value(static_cast<int>(Section::Bluetooth));
        if (!reply.result.value(QStringLiteral("available")).toBool(true)) {
            bluetooth->setSubtitle(tr("No adapter"));
            return;
        }
        if (!reply.result.value(QStringLiteral("powered")).toBool()) {
            bluetooth->setSubtitle(tr("Off"));
            return;
        }
        QString connected;
        for (const auto& device : reply.result.value(QStringLiteral("devices")).toArray()) {
            if (device.toObject().value(QStringLiteral("connected")).toBool()) {
                connected = device.toObject().value(QStringLiteral("name")).toString();
            }
        }
        bluetooth->setSubtitle(connected.isEmpty() ? tr("On · high latency") : tr("On · %1").arg(connected));
    });
    client->call(QStringLiteral("display.get"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        auto* screen = self->m_tiles.value(static_cast<int>(Section::Screen));
        screen->setSubtitle(reply.result.value(QStringLiteral("available")).toBool()
                        ? tr("Brightness %1%").arg(reply.result.value(QStringLiteral("brightness")).toInt())
                        : tr("Fixed brightness"));
    });
    client->call(QStringLiteral("time.status"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        QString zone = reply.result.value(QStringLiteral("timezone")).toString();
        zone.replace(QLatin1Char('_'), QLatin1Char(' '));
        self->m_tiles.value(static_cast<int>(Section::Time))->setSubtitle(zone.isEmpty() ? tr("Time zone not set") : zone);
    });
    client->call(QStringLiteral("system.info"), {}, [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) {
            return;
        }
        const QString version = reply.result.value(QStringLiteral("decklab_version")).toString();
        if (!version.isEmpty()) {
            self->m_tiles.value(static_cast<int>(Section::About))->setSubtitle(tr("Deck Lab %1").arg(version));
        }
    });
}
