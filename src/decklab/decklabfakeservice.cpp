// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabfakeservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <algorithm>
#include <utility>

#include "moc_decklabfakeservice.cpp"

namespace {
QJsonObject parse(const char* json) {
    return QJsonDocument::fromJson(QByteArray(json)).object();
}
QJsonArray parseArray(const char* json) {
    return QJsonDocument::fromJson(QByteArray(json)).array();
}
} // namespace

DeckLabFakeService::DeckLabFakeService(QObject* parent)
        : QObject(parent),
          m_server(new QLocalServer(this)) {
    systemInfo = parse(R"JSON({"hostname": "decklab", "os": "Raspberry Pi OS (Debian 13)",
        "kernel": "6.18.0-v8+", "model": "Raspberry Pi 4 Model B Rev 1.2", "decklab_version": "2026.10",
        "uptime_s": 5420, "temp_c": 58.4, "throttled": "0x0", "undervoltage_now": false, "cpu_mhz": 1500,
        "mem_available_mb": 2810, "ip": ["192.0.2.42"],
        "storage": {"root_free_mb": 18450, "data_free_mb": 18450}})JSON");
    wifiStatus = parse(R"JSON({"enabled": true, "connected": true, "ssid": "Studio-WiFi", "signal": 78,
        "ip": "192.0.2.42", "saved": ["Studio-WiFi", "Backstage-WiFi"]})JSON");
    networks = parseArray(R"JSON([
        {"ssid": "Studio-WiFi", "signal": 78, "security": "wpa", "saved": true, "active": true},
        {"ssid": "Backstage-WiFi", "signal": 54, "security": "wpa3", "saved": true, "active": false},
        {"ssid": "Guest-WiFi", "signal": 41, "security": "open", "saved": false, "active": false},
        {"ssid": "Example-Network", "signal": 22, "security": "wpa", "saved": false, "active": false}])JSON");
    btStatus = parse(R"JSON({"available": true, "powered": true, "devices": [
        {"address": "02:00:00:00:00:01", "name": "Practice Speaker", "paired": true, "connected": false, "audio": true},
        {"address": "02:00:00:00:00:02", "name": "Wireless Headphones", "paired": false, "connected": false, "audio": true}]})JSON");
    display = parse(R"JSON({"available": true, "brightness": 80})JSON");
    timeStatus = parse(R"JSON({"timezone": "Europe/London", "ntp": true, "synced": true,
        "now": "2026-10-22T21:30:00+01:00"})JSON");
    timezones = QStringList{QStringLiteral("Africa/Cairo"),
            QStringLiteral("Africa/Johannesburg"),
            QStringLiteral("Africa/Lagos"),
            QStringLiteral("America/Chicago"),
            QStringLiteral("America/Los_Angeles"),
            QStringLiteral("America/Mexico_City"),
            QStringLiteral("America/New_York"),
            QStringLiteral("America/Sao_Paulo"),
            QStringLiteral("America/Toronto"),
            QStringLiteral("Asia/Dubai"),
            QStringLiteral("Asia/Kolkata"),
            QStringLiteral("Asia/Seoul"),
            QStringLiteral("Asia/Singapore"),
            QStringLiteral("Asia/Tokyo"),
            QStringLiteral("Australia/Melbourne"),
            QStringLiteral("Australia/Sydney"),
            QStringLiteral("Europe/Amsterdam"),
            QStringLiteral("Europe/Berlin"),
            QStringLiteral("Europe/Lisbon"),
            QStringLiteral("Europe/London"),
            QStringLiteral("Europe/Madrid"),
            QStringLiteral("Europe/Paris"),
            QStringLiteral("Europe/Rome"),
            QStringLiteral("Pacific/Auckland"),
            QStringLiteral("UTC")};
    drives = parseArray(R"JSON([
        {"id": "6A3F-1C2B", "label": "DJ USB", "mount": "/media/decklab/6A3F-1C2B", "size_mb": 30528,
         "free_mb": 12402, "fs": "exfat", "read_only": true, "rekordbox": true, "busy": false},
        {"id": "0e9c1d7a-55aa-4b33-9e3f-2f9d0c4a1b77", "label": "MIX SETS",
         "mount": "/media/decklab/0e9c1d7a-55aa-4b33-9e3f-2f9d0c4a1b77", "size_mb": 61056,
         "free_mb": 40220, "fs": "ext4", "read_only": false, "rekordbox": false, "busy": false}])JSON");
    devices = parse(R"JSON({"controllers": [
        {"key": "2b73:0042:A1B2C3", "vid": "2b73", "pid": "0042", "serial": "A1B2C3", "name": "DDJ-FLX4",
         "midi_ports": ["DDJ-FLX4 MIDI 1"], "hid": false,
         "audio_card": {"alsa_id": "DDJFLX4", "name": "DDJ-FLX4", "playback_channels": 4, "capture_channels": 0}}],
        "audio_cards": [
        {"alsa_id": "Headphones", "name": "bcm2835 Headphones", "playback_channels": 2, "capture_channels": 0,
         "usb": false, "builtin": true},
        {"alsa_id": "DDJFLX4", "name": "DDJ-FLX4", "playback_channels": 4, "capture_channels": 0,
         "usb": true, "builtin": false}]})JSON");
    connect(m_server, &QLocalServer::newConnection, this, &DeckLabFakeService::onConnection);
}

DeckLabFakeService::~DeckLabFakeService() {
    m_server->close();
}

bool DeckLabFakeService::listen(const QString& path) {
    QLocalServer::removeServer(path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    return m_server->listen(path);
}

QString DeckLabFakeService::path() const {
    return m_server->fullServerName();
}

int DeckLabFakeService::connectionCount() const {
    int count = 0;
    for (const auto& client : m_clients) {
        count += client && client->state() == QLocalSocket::ConnectedState;
    }
    return count;
}

void DeckLabFakeService::onConnection() {
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        m_clients.append(socket);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QObject::destroyed, this, [this, socket] {
            m_buffers.remove(socket);
        });
    }
}

void DeckLabFakeService::onReadyRead(QLocalSocket* socket) {
    QByteArray& buffer = m_buffers[socket];
    buffer.append(socket->readAll());
    int newline;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        const auto document = QJsonDocument::fromJson(line);
        if (document.isObject()) {
            handle(socket, document.object());
        }
    }
}

void DeckLabFakeService::reply(QLocalSocket* socket, const QJsonValue& id, const QJsonObject& result) {
    if (!socket) {
        return;
    }
    QByteArray line = QJsonDocument(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), result}})
                              .toJson(QJsonDocument::Compact);
    line.append('\n');
    socket->write(line);
}

void DeckLabFakeService::pushEvent(const QString& topic, const QJsonObject& data) {
    QByteArray line = QJsonDocument(QJsonObject{{QStringLiteral("event"), topic}, {QStringLiteral("data"), data}})
                              .toJson(QJsonDocument::Compact);
    line.append('\n');
    for (const auto& client : std::as_const(m_subscribed)) {
        if (client) {
            client->write(line);
        }
    }
}

qint64 DeckLabFakeService::relay(const QString& event, QJsonObject data) {
    const qint64 serial = m_nextSerial++;
    data.insert(QStringLiteral("serial"), serial);
    pushEvent(event, data);
    return serial;
}

QJsonObject DeckLabFakeService::relayReply(qint64 serial) const {
    for (auto it = requests.crbegin(); it != requests.crend(); ++it) {
        const QJsonObject params = it->value(QStringLiteral("params")).toObject();
        if (it->value(QStringLiteral("method")).toString() == QStringLiteral("relay.reply") &&
                params.value(QStringLiteral("serial")).toInteger(-1) == serial) {
            return params;
        }
    }
    return QJsonObject();
}

QJsonObject DeckLabFakeService::lastPreviewReport() const {
    for (auto it = requests.crbegin(); it != requests.crend(); ++it) {
        if (it->value(QStringLiteral("method")).toString() == QStringLiteral("preview.report")) {
            return it->value(QStringLiteral("params")).toObject();
        }
    }
    return QJsonObject();
}

void DeckLabFakeService::sendRaw(const QByteArray& bytes) {
    for (const auto& client : std::as_const(m_clients)) {
        if (client) {
            client->write(bytes);
            client->flush();
        }
    }
}

void DeckLabFakeService::disconnectClients() {
    const auto clients = m_clients;
    m_clients.clear();
    m_subscribed.clear();
    for (const auto& client : clients) {
        if (client) {
            client->disconnectFromServer();
        }
    }
}

void DeckLabFakeService::later(int ms, std::function<void()> action) {
    QTimer::singleShot(ms, this, std::move(action));
}

void DeckLabFakeService::handle(QLocalSocket* socket, const QJsonObject& request) {
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonValue id = request.value(QStringLiteral("id"));
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    calls.append(method);
    requests.append(request);
    emit requestReceived(method);
    if (!respond) {
        return;
    }
    if (failures.contains(method)) {
        QByteArray line = QJsonDocument(QJsonObject{{QStringLiteral("id"), id},
                                                {QStringLiteral("error"),
                                                        QJsonObject{{QStringLiteral("code"), failures.value(method)},
                                                                {QStringLiteral("message"), QStringLiteral("Fake failure")}}}})
                                  .toJson(QJsonDocument::Compact);
        line.append('\n');
        socket->write(line);
        return;
    }
    const QJsonObject started{{QStringLiteral("state"), QStringLiteral("started")}};
    QPointer<QLocalSocket> client(socket);
    if (method == QStringLiteral("hello")) {
        reply(socket, id, QJsonObject{{QStringLiteral("protocol"), 1},
                {QStringLiteral("version"), QStringLiteral("fake-1")},
                {QStringLiteral("features"),
                        QJsonArray{QStringLiteral("wifi"), QStringLiteral("bt"), QStringLiteral("display"),
                                QStringLiteral("power"), QStringLiteral("time"), QStringLiteral("usb"),
                                QStringLiteral("devices"), QStringLiteral("apps"), QStringLiteral("thermal"),
                                QStringLiteral("preview")}}});
    } else if (method == QStringLiteral("events.subscribe")) {
        if (!m_subscribed.contains(client)) {
            m_subscribed.append(client);
        }
        reply(socket, id, QJsonObject{{QStringLiteral("topics"), params.value(QStringLiteral("topics"))}});
    } else if (method == QStringLiteral("system.info")) {
        reply(socket, id, systemInfo);
    } else if (method == QStringLiteral("display.get")) {
        reply(socket, id, display);
    } else if (method == QStringLiteral("display.set")) {
        const int brightness = std::clamp(params.value(QStringLiteral("brightness")).toInt(), 5, 100);
        display.insert(QStringLiteral("brightness"), brightness);
        reply(socket, id, QJsonObject{{QStringLiteral("brightness"), brightness}});
    } else if (method == QStringLiteral("wifi.status")) {
        reply(socket, id, wifiStatus);
    } else if (method == QStringLiteral("wifi.scan")) {
        reply(socket, id, started);
        later(250, [this] { pushEvent(QStringLiteral("wifi.scan_done"), QJsonObject{{QStringLiteral("networks"), networks}}); });
    } else if (method == QStringLiteral("wifi.connect")) {
        reply(socket, id, started);
        const QString ssid = params.value(QStringLiteral("ssid")).toString();
        const bool wrong = params.value(QStringLiteral("password")).toString() == QStringLiteral("wrong");
        later(300, [this, ssid, wrong] {
            QJsonObject status = wifiStatus;
            if (wrong) {
                status.insert(QStringLiteral("error"), QStringLiteral("Wrong password for %1").arg(ssid));
            } else {
                wifiStatus.insert(QStringLiteral("ssid"), ssid);
                wifiStatus.insert(QStringLiteral("connected"), true);
                status = wifiStatus;
            }
            pushEvent(QStringLiteral("wifi.changed"), status);
        });
    } else if (method == QStringLiteral("wifi.forget")) {
        QJsonArray saved;
        for (const auto& value : wifiStatus.value(QStringLiteral("saved")).toArray()) {
            if (value.toString() != params.value(QStringLiteral("ssid")).toString()) {
                saved.append(value);
            }
        }
        wifiStatus.insert(QStringLiteral("saved"), saved);
        reply(socket, id, {});
    } else if (method == QStringLiteral("wifi.enable")) {
        wifiStatus.insert(QStringLiteral("enabled"), params.value(QStringLiteral("on")).toBool());
        reply(socket, id, {});
        later(100, [this] { pushEvent(QStringLiteral("wifi.changed"), wifiStatus); });
    } else if (method == QStringLiteral("bt.status")) {
        reply(socket, id, btStatus);
    } else if (method == QStringLiteral("bt.power")) {
        btStatus.insert(QStringLiteral("powered"), params.value(QStringLiteral("on")).toBool());
        reply(socket, id, {});
        later(100, [this] { pushEvent(QStringLiteral("bt.changed"), btStatus); });
    } else if (method.startsWith(QStringLiteral("bt."))) {
        reply(socket, id, started);
        const QString address = params.value(QStringLiteral("address")).toString();
        later(250, [this, method, address] {
            QJsonArray updated;
            for (const auto& value : btStatus.value(QStringLiteral("devices")).toArray()) {
                QJsonObject device = value.toObject();
                if (device.value(QStringLiteral("address")).toString() == address) {
                    if (method == QStringLiteral("bt.pair")) {
                        device.insert(QStringLiteral("paired"), true);
                    } else if (method == QStringLiteral("bt.connect")) {
                        device.insert(QStringLiteral("connected"), true);
                    } else if (method == QStringLiteral("bt.disconnect")) {
                        device.insert(QStringLiteral("connected"), false);
                    } else if (method == QStringLiteral("bt.forget")) {
                        continue;
                    }
                }
                updated.append(device);
            }
            btStatus.insert(QStringLiteral("devices"), updated);
            pushEvent(QStringLiteral("bt.changed"), btStatus);
        });
    } else if (method == QStringLiteral("time.status")) {
        reply(socket, id, timeStatus);
    } else if (method == QStringLiteral("time.timezones")) {
        reply(socket, id, QJsonObject{{QStringLiteral("timezones"), QJsonArray::fromStringList(timezones)}});
    } else if (method == QStringLiteral("time.set_timezone")) {
        timeStatus.insert(QStringLiteral("timezone"), params.value(QStringLiteral("timezone")).toString());
        reply(socket, id, {});
    } else if (method == QStringLiteral("usb.list")) {
        reply(socket, id, QJsonObject{{QStringLiteral("drives"), drives}});
    } else if (method == QStringLiteral("usb.eject")) {
        reply(socket, id, started);
        const QString driveId = params.value(QStringLiteral("id")).toString();
        later(250, [this, driveId] {
            QJsonArray remaining;
            for (const auto& value : std::as_const(drives)) {
                if (value.toObject().value(QStringLiteral("id")).toString() != driveId) {
                    remaining.append(value);
                }
            }
            drives = remaining;
            pushEvent(QStringLiteral("usb.changed"), QJsonObject{{QStringLiteral("drives"), drives}});
        });
    } else if (method == QStringLiteral("usb.set_writable")) {
        const bool writable = params.value(QStringLiteral("writable")).toBool();
        QJsonArray updated;
        for (const auto& value : std::as_const(drives)) {
            QJsonObject drive = value.toObject();
            if (drive.value(QStringLiteral("id")).toString() == params.value(QStringLiteral("id")).toString()) {
                drive.insert(QStringLiteral("read_only"), !writable);
            }
            updated.append(drive);
        }
        drives = updated;
        reply(socket, id, QJsonObject{{QStringLiteral("read_only"), !writable}});
    } else if (method == QStringLiteral("devices.list")) {
        reply(socket, id, devices);
    } else if (method == QStringLiteral("apps.launch") || method == QStringLiteral("system.power") ||
            method == QStringLiteral("session.restart_mixxx")) {
        reply(socket, id, started);
    } else if (method == QStringLiteral("apps.status") && !openApp.isUndefined()) {
        reply(socket, id, QJsonObject{{QStringLiteral("app"), openApp}});
    } else if (method == QStringLiteral("apps.close") || method == QStringLiteral("relay.reply") ||
            method == QStringLiteral("preview.report")) {
        // v1.2: Mixxx's answers to relayed requests and its preview state (recorded in requests).
        reply(socket, id, {});
    } else {
        // session.desktop and unknown methods
        const QString code = method == QStringLiteral("session.desktop") ? QStringLiteral("not_available")
                                                                          : QStringLiteral("unknown_method");
        QByteArray line = QJsonDocument(QJsonObject{{QStringLiteral("id"), id},
                                                {QStringLiteral("error"),
                                                        QJsonObject{{QStringLiteral("code"), code},
                                                                {QStringLiteral("message"),
                                                                        QStringLiteral("Maintenance desktop is not installed")}}}})
                                  .toJson(QJsonDocument::Compact);
        line.append('\n');
        socket->write(line);
    }
}
