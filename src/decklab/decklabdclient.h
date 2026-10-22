// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <functional>

class QLocalSocket;

/// One answer from decklabd, or a local failure that never reached it.
struct DeckLabdReply {
    bool ok = false;
    QJsonObject result;
    QString errorCode; // contract codes plus local "not_available"/"timeout"
    QString errorMessage;
};

/// Client for the decklabd protocol v1 (newline-delimited JSON over a Unix
/// socket). DJing never depends on it: while decklabd is missing every call
/// fails fast with "not_available" and the client keeps reconnecting with
/// backoff in the background.
class DeckLabdClient : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const DeckLabdReply&)>;
    static constexpr int kProtocol = 1;
    static constexpr int kMaxLineBytes = 64 * 1024;
    static constexpr int kDefaultTimeoutMs = 2500;

    explicit DeckLabdClient(QObject* parent = nullptr, const QString& socketPath = QString());
    ~DeckLabdClient() override;

    /// DECKLAB_SOCKET, else $XDG_RUNTIME_DIR/decklab/decklabd.sock.
    static QString defaultSocketPath();

    void start();
    void stop();
    /// Reconnects to another socket (developer captures and tests).
    void setSocketPath(const QString& path);
    /// Connected and past hello + events.subscribe.
    bool isReady() const { return m_ready; }
    QString socketPath() const { return m_path; }
    QString serverVersion() const { return m_serverVersion; }
    QStringList features() const { return m_features; }
    /// decklabd started with --read-only (state changes are denied).
    bool isReadOnly() const { return m_readOnly; }
    bool hasFeature(const QString& feature) const { return m_features.contains(feature); }
    /// Test hooks; defaults follow the contract (0.5 s to 5 s).
    void setReconnectBackoff(int minimumMs, int maximumMs);

    /// Sends a request. The callback always runs exactly once, on the
    /// client's thread, never synchronously from inside call().
    quint64 call(const QString& method,
            const QJsonObject& params = QJsonObject(),
            Callback callback = Callback(),
            int timeoutMs = kDefaultTimeoutMs);

    static QStringList subscribedTopics();

  signals:
    void readyChanged(bool ready);
    void eventReceived(const QString& topic, const QJsonObject& data);

  private:
    struct Pending {
        Callback callback;
        qint64 deadline = 0;
    };
    void connectNow();
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void handleLine(const QByteArray& line);
    void scheduleReconnect();
    void dropConnection(const QString& reason);
    void failAll(const QString& code, const QString& message);
    void finish(quint64 id, const DeckLabdReply& reply);
    void checkTimeouts();
    quint64 send(const QString& method, const QJsonObject& params, Callback callback, int timeoutMs);
    void setReady(bool ready);

    QString m_path;
    QPointer<QLocalSocket> m_socket;
    QByteArray m_buffer;
    QHash<quint64, Pending> m_pending;
    quint64 m_nextId = 1;
    QTimer m_reconnect;
    QTimer m_timeouts;
    QElapsedTimer m_clock;
    int m_backoffMin = 500;
    int m_backoffMax = 5000;
    int m_backoff = 500;
    bool m_running = false;
    bool m_ready = false;
    QString m_serverVersion;
    bool m_readOnly = false;
    QStringList m_features;
};
