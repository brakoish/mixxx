// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class QLocalServer;
class QLocalSocket;

/// A canned decklabd (protocol v1) on a private socket. Used only by native
/// tests and the developer offscreen captures; ordinary launches never
/// create it. It never touches the host.
class DeckLabFakeService : public QObject {
    Q_OBJECT
  public:
    explicit DeckLabFakeService(QObject* parent = nullptr);
    ~DeckLabFakeService() override;

    bool listen(const QString& path);
    QString path() const;
    int connectionCount() const;

    void pushEvent(const QString& topic, const QJsonObject& data);
    /// Writes raw bytes to every client (framing tests).
    void sendRaw(const QByteArray& bytes);
    void disconnectClients();

    // Canned host state; tests may edit it.
    QJsonObject systemInfo;
    QJsonObject wifiStatus;
    QJsonArray networks;
    QJsonObject btStatus;
    QJsonObject display;
    QJsonObject timeStatus;
    QStringList timezones;
    QJsonArray drives;
    QJsonObject devices;
    /// Methods received, in order.
    QStringList calls;
    QList<QJsonObject> requests;
    /// When false, requests are recorded but never answered.
    bool respond = true;
    /// Methods that answer with {"error": {"code": "...", ...}}.
    QHash<QString, QString> failures;

  signals:
    void requestReceived(const QString& method);

  private:
    void onConnection();
    void onReadyRead(QLocalSocket* socket);
    void handle(QLocalSocket* socket, const QJsonObject& request);
    void reply(QLocalSocket* socket, const QJsonValue& id, const QJsonObject& result);
    void later(int ms, std::function<void()> action);

    QLocalServer* m_server;
    QList<QPointer<QLocalSocket>> m_clients;
    QHash<QLocalSocket*, QByteArray> m_buffers;
    QList<QPointer<QLocalSocket>> m_subscribed;
};
