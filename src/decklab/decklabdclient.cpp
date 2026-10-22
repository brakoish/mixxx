// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabdclient.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QtDebug>
#include <algorithm>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "moc_decklabdclient.cpp"

namespace {
DeckLabdReply failure(const QString& code, const QString& message) {
    DeckLabdReply reply;
    reply.errorCode = code;
    reply.errorMessage = message;
    return reply;
}
} // namespace

DeckLabdClient::DeckLabdClient(QObject* parent, const QString& socketPath)
        : QObject(parent),
          m_path(socketPath.isEmpty() ? defaultSocketPath() : socketPath) {
    m_clock.start();
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &DeckLabdClient::connectNow);
    m_timeouts.setInterval(50);
    connect(&m_timeouts, &QTimer::timeout, this, &DeckLabdClient::checkTimeouts);
}

DeckLabdClient::~DeckLabdClient() {
    // Owners are being destroyed too; never call back into them now.
    m_pending.clear();
    m_running = false;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

QString DeckLabdClient::defaultSocketPath() {
    const QString configured = qEnvironmentVariable("DECKLAB_SOCKET");
    if (!configured.isEmpty()) {
        return configured;
    }
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
#ifdef Q_OS_UNIX
    if (runtime.isEmpty()) {
        runtime = QStringLiteral("/run/user/%1").arg(static_cast<qulonglong>(::getuid()));
    }
#endif
    return QDir(runtime).filePath(QStringLiteral("decklab/decklabd.sock"));
}

QStringList DeckLabdClient::subscribedTopics() {
    return {QStringLiteral("usb"),
            QStringLiteral("devices"),
            QStringLiteral("thermal"),
            QStringLiteral("wifi"),
            QStringLiteral("bt"),
            QStringLiteral("power"),
            QStringLiteral("session"),
            QStringLiteral("apps")};
}

void DeckLabdClient::setReconnectBackoff(int minimumMs, int maximumMs) {
    m_backoffMin = std::max(1, minimumMs);
    m_backoffMax = std::max(m_backoffMin, maximumMs);
    m_backoff = m_backoffMin;
}

void DeckLabdClient::start() {
    if (m_running) {
        return;
    }
    m_running = true;
    m_backoff = m_backoffMin;
    connectNow();
}

void DeckLabdClient::stop() {
    m_running = false;
    m_reconnect.stop();
    if (m_socket) {
        auto* socket = m_socket.data();
        m_socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_buffer.clear();
    setReady(false);
    failAll(QStringLiteral("not_available"), tr("System service stopped"));
}

void DeckLabdClient::setSocketPath(const QString& path) {
    const bool running = m_running;
    stop();
    m_path = path;
    if (running) {
        start();
    }
}

void DeckLabdClient::connectNow() {
    if (!m_running || m_socket) {
        return;
    }
    auto* socket = new QLocalSocket(this);
    m_socket = socket;
    connect(socket, &QLocalSocket::connected, this, &DeckLabdClient::onConnected);
    connect(socket, &QLocalSocket::disconnected, this, &DeckLabdClient::onDisconnected);
    connect(socket, &QLocalSocket::readyRead, this, &DeckLabdClient::onReadyRead);
    connect(socket, &QLocalSocket::errorOccurred, this, [this, socket](QLocalSocket::LocalSocketError) {
        // A failed connect never emits disconnected(); treat both the same.
        if (socket == m_socket && socket->state() != QLocalSocket::ConnectedState) {
            dropConnection(socket->errorString());
        }
    });
    socket->connectToServer(m_path);
}

void DeckLabdClient::onConnected() {
    m_buffer.clear();
    QJsonObject hello{{QStringLiteral("client"), QStringLiteral("mixxx")},
            {QStringLiteral("protocol"), kProtocol}};
    send(QStringLiteral("hello"), hello, [this](const DeckLabdReply& reply) {
        if (!reply.ok || reply.result.value(QStringLiteral("protocol")).toInt() != kProtocol) {
            qWarning() << "decklabd hello refused:" << reply.errorCode << reply.errorMessage;
            dropConnection(QStringLiteral("hello"));
            return;
        }
        m_serverVersion = reply.result.value(QStringLiteral("version")).toString();
        m_readOnly = reply.result.value(QStringLiteral("read_only")).toBool(false);
        m_features.clear();
        const auto features = reply.result.value(QStringLiteral("features")).toArray();
        for (const auto& feature : features) {
            m_features.append(feature.toString());
        }
        QJsonObject topics{{QStringLiteral("topics"), QJsonArray::fromStringList(subscribedTopics())}};
        send(QStringLiteral("events.subscribe"), topics, [this](const DeckLabdReply& subscribed) {
            if (!subscribed.ok) {
                qWarning() << "decklabd events.subscribe failed:" << subscribed.errorCode;
                dropConnection(QStringLiteral("subscribe"));
                return;
            }
            m_backoff = m_backoffMin;
            setReady(true);
        },
                kDefaultTimeoutMs);
    },
            kDefaultTimeoutMs);
}

void DeckLabdClient::onDisconnected() {
    dropConnection(QStringLiteral("disconnected"));
}

void DeckLabdClient::dropConnection(const QString& reason) {
    if (m_socket) {
        auto* socket = m_socket.data();
        m_socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_buffer.clear();
    const bool wasReady = m_ready;
    setReady(false);
    failAll(QStringLiteral("not_available"), tr("System service unavailable"));
    if (wasReady) {
        qInfo() << "decklabd connection closed:" << reason;
    }
    scheduleReconnect();
}

void DeckLabdClient::scheduleReconnect() {
    if (!m_running || m_reconnect.isActive()) {
        return;
    }
    m_reconnect.start(m_backoff);
    m_backoff = std::min(m_backoffMax, m_backoff * 2);
}

void DeckLabdClient::onReadyRead() {
    if (!m_socket) {
        return;
    }
    m_buffer.append(m_socket->readAll());
    while (true) {
        const int newline = m_buffer.indexOf('\n');
        if (newline < 0) {
            break;
        }
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.size() > kMaxLineBytes) {
            dropConnection(QStringLiteral("line too long"));
            return;
        }
        handleLine(line.trimmed());
        if (!m_socket) {
            return; // a handler dropped the connection
        }
    }
    if (m_buffer.size() > kMaxLineBytes) {
        dropConnection(QStringLiteral("line too long"));
    }
}

void DeckLabdClient::handleLine(const QByteArray& line) {
    if (line.isEmpty()) {
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "decklabd sent an invalid line:" << error.errorString();
        return;
    }
    const QJsonObject message = document.object();
    if (message.value(QStringLiteral("event")).isString()) {
        emit eventReceived(message.value(QStringLiteral("event")).toString(),
                message.value(QStringLiteral("data")).toObject());
        return;
    }
    const double rawId = message.value(QStringLiteral("id")).toDouble(-1);
    if (rawId < 1) {
        return;
    }
    DeckLabdReply reply;
    if (message.contains(QStringLiteral("error"))) {
        const QJsonObject error = message.value(QStringLiteral("error")).toObject();
        reply.errorCode = error.value(QStringLiteral("code")).toString(QStringLiteral("failed"));
        reply.errorMessage = error.value(QStringLiteral("message")).toString();
    } else {
        reply.ok = true;
        reply.result = message.value(QStringLiteral("result")).toObject();
    }
    finish(static_cast<quint64>(rawId), reply);
}

quint64 DeckLabdClient::call(const QString& method,
        const QJsonObject& params,
        Callback callback,
        int timeoutMs) {
    if (!m_ready) {
        const quint64 id = m_nextId++;
        if (callback) {
            QTimer::singleShot(0, this, [callback] {
                callback(failure(QStringLiteral("not_available"),
                        tr("System service offline")));
            });
        }
        return id;
    }
    return send(method, params, std::move(callback), timeoutMs);
}

quint64 DeckLabdClient::send(const QString& method,
        const QJsonObject& params,
        Callback callback,
        int timeoutMs) {
    const quint64 id = m_nextId++;
    QJsonObject request{{QStringLiteral("id"), static_cast<qint64>(id)},
            {QStringLiteral("method"), method},
            {QStringLiteral("params"), params}};
    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (line.size() >= kMaxLineBytes || !m_socket) {
        if (callback) {
            const QString code = m_socket ? QStringLiteral("bad_request") : QStringLiteral("not_available");
            QTimer::singleShot(0, this, [callback, code] {
                callback(failure(code, tr("Request could not be sent")));
            });
        }
        return id;
    }
    line.append('\n');
    m_pending.insert(id, Pending{std::move(callback), m_clock.elapsed() + std::max(50, timeoutMs)});
    if (!m_timeouts.isActive()) {
        m_timeouts.start();
    }
    m_socket->write(line);
    return id;
}

void DeckLabdClient::finish(quint64 id, const DeckLabdReply& reply) {
    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return; // late answer after a timeout
    }
    Callback callback = std::move(it->callback);
    m_pending.erase(it);
    if (m_pending.isEmpty()) {
        m_timeouts.stop();
    }
    if (callback) {
        callback(reply);
    }
}

void DeckLabdClient::checkTimeouts() {
    const qint64 now = m_clock.elapsed();
    QList<quint64> expired;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it->deadline <= now) {
            expired.append(it.key());
        }
    }
    std::sort(expired.begin(), expired.end());
    for (quint64 id : std::as_const(expired)) {
        finish(id, failure(QStringLiteral("timeout"), tr("System service did not answer")));
    }
    if (m_pending.isEmpty()) {
        m_timeouts.stop();
    }
}

void DeckLabdClient::failAll(const QString& code, const QString& message) {
    if (m_pending.isEmpty()) {
        return;
    }
    QList<quint64> ids = m_pending.keys();
    std::sort(ids.begin(), ids.end());
    for (quint64 id : std::as_const(ids)) {
        finish(id, failure(code, message));
    }
}

void DeckLabdClient::setReady(bool ready) {
    if (m_ready == ready) {
        return;
    }
    m_ready = ready;
    if (!ready) {
        m_features.clear();
    }
    emit readyChanged(ready);
}
