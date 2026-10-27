// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabmedia.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QSqlQuery>
#include <QtDebug>
#include <algorithm>

#include "library/queryutil.h"
#include "moc_decklabmedia.cpp"

namespace decklab {

namespace {

const QString kAwayTable = QStringLiteral("decklab_media_away");

bool usableSegment(const QString& segment) {
    return !segment.isEmpty() && segment != QStringLiteral(".") && segment != QStringLiteral("..");
}

// /media/<a>/<b> and /run/media/<user>/<name>: every location under it
// belongs to it. /media/<a> holds only the files right inside it.
bool isDeepRoot(const QString& root) {
    return (root.startsWith(QStringLiteral("/media/")) && root.count(QLatin1Char('/')) == 3) ||
            root.startsWith(QStringLiteral("/run/media/"));
}

void ensureAwayTable(const QSqlDatabase& database) {
    // Per connection and never written to disk; IF NOT EXISTS keeps the
    // schema (and prepared statements) as they are once it is there.
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("CREATE TEMP TABLE IF NOT EXISTS %1 (location_id INTEGER PRIMARY KEY)")
                            .arg(kAwayTable))) {
        LOG_FAILED_QUERY(query);
    }
}

} // namespace

QString mediaRootOf(const QString& location) {
    if (!location.startsWith(QLatin1Char('/'))) {
        return QString();
    }
    // parts[0] is empty (the leading slash); the last part is the file.
    const QStringList parts = location.split(QLatin1Char('/'));
    if (parts.value(1) == QStringLiteral("media")) {
        if (parts.size() >= 5 && usableSegment(parts[2]) && usableSegment(parts[3])) {
            return QStringLiteral("/media/") + parts[2] + QLatin1Char('/') + parts[3];
        }
        if (parts.size() == 4 && usableSegment(parts[2])) {
            return QStringLiteral("/media/") + parts[2];
        }
        return QString();
    }
    if (parts.value(1) == QStringLiteral("run") && parts.value(2) == QStringLiteral("media") &&
            parts.size() >= 6 && usableSegment(parts[3]) && usableSegment(parts[4])) {
        return QStringLiteral("/run/media/") + parts[3] + QLatin1Char('/') + parts[4];
    }
    return QString();
}

bool mediaRootIsIn(const QString& root) {
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return false;
    }
    QDirIterator entries(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    return entries.hasNext();
}

QString mediaShownCondition(const QSqlDatabase& database, const QString& locationColumn) {
    return QStringLiteral("NOT ") + mediaAwayCondition(database, locationColumn);
}

QString mediaAwayCondition(const QSqlDatabase& database, const QString& locationColumn) {
    ensureAwayTable(database);
    // An indexed lookup per row; a row without a location is never away.
    return QStringLiteral("EXISTS (SELECT 1 FROM temp.%1 WHERE location_id=%2)")
            .arg(kAwayTable, locationColumn);
}

MediaPresence& MediaPresence::instance() {
    static MediaPresence presence;
    return presence;
}

MediaPresence::MediaPresence() {
    m_settle.setInterval(kSettleCheckMs);
    connect(&m_settle, &QTimer::timeout, this, &MediaPresence::checkSettled);
}

void MediaPresence::setShown(QObject* model) {
    m_shown = model;
    emit shownChanged(model);
}

void MediaPresence::checkSettled() {
    if (deferring()) {
        return;
    }
    m_settle.stop();
    emit settled();
}

bool MediaPresence::refresh(QSqlDatabase database, bool force) {
    ensureAwayTable(database);
    QHash<QString, QList<qint64>> byRoot;
    {
        QSqlQuery query(database);
        query.setForwardOnly(true);
        // Ranges on the unique location index: /media/... and /run/media/...
        if (!query.exec(QStringLiteral(
                    "SELECT id, location FROM track_locations "
                    "WHERE (location >= '/media/' AND location < '/media0') "
                    "OR (location >= '/run/media/' AND location < '/run/media0')"))) {
            LOG_FAILED_QUERY(query);
            return false;
        }
        // Rows come in location order: the root of the row before usually
        // holds this one too, without splitting the path again.
        QString lastRoot;
        QString lastPrefix;
        while (query.next()) {
            const QString location = query.value(1).toString();
            QString root;
            if (!lastPrefix.isEmpty() && location.startsWith(lastPrefix)) {
                root = lastRoot;
            } else {
                root = mediaRootOf(location);
                lastRoot = root;
                lastPrefix = isDeepRoot(root) ? root + QLatin1Char('/') : QString();
            }
            if (!root.isEmpty()) {
                byRoot[root].append(query.value(0).toLongLong());
            }
        }
    }
    QSet<qint64> away;
    QStringList awayRoots;
    for (auto it = byRoot.cbegin(); it != byRoot.cend(); ++it) {
        const bool in = m_probe ? m_probe(it.key()) : mediaRootIsIn(it.key());
        if (!in) {
            awayRoots.append(it.key());
            for (const qint64 id : it.value()) {
                away.insert(id);
            }
        }
    }
    std::sort(awayRoots.begin(), awayRoots.end());
    const bool different = away != m_away || awayRoots != m_awayRoots;
    // r7 fix: written only when other tracks are away than the table holds
    // (it belongs to this connection; only the temporary database is
    // touched: no journal sync, no lock on the library file).
    if (different || m_written != database.connectionName()) {
        const bool own = database.transaction();
        QSqlQuery clear(database);
        bool ok = clear.exec(QStringLiteral("DELETE FROM temp.%1").arg(kAwayTable));
        if (!ok) {
            LOG_FAILED_QUERY(clear);
        }
        // A whole stick in one statement (a range on the location index);
        // the files right inside /media/<a> one by one.
        QSqlQuery range(database);
        range.prepare(QStringLiteral("INSERT OR IGNORE INTO temp.%1 (location_id) SELECT id FROM "
                                     "track_locations WHERE location >= :from AND location < :to")
                              .arg(kAwayTable));
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral("INSERT OR IGNORE INTO temp.%1 (location_id) VALUES (:id)").arg(kAwayTable));
        for (const QString& root : std::as_const(awayRoots)) {
            if (!ok) {
                break;
            }
            if (isDeepRoot(root)) {
                range.bindValue(QStringLiteral(":from"), QString(root + QLatin1Char('/')));
                range.bindValue(QStringLiteral(":to"), QString(root + QLatin1Char('0')));
                if (!range.exec()) {
                    LOG_FAILED_QUERY(range);
                    ok = false;
                }
                continue;
            }
            for (const qint64 id : byRoot.value(root)) {
                insert.bindValue(QStringLiteral(":id"), id);
                if (!insert.exec()) {
                    LOG_FAILED_QUERY(insert);
                    ok = false;
                    break;
                }
            }
        }
        if (own) {
            ok = database.commit() && ok;
        }
        m_written = ok ? database.connectionName() : QString();
    }
    m_away = away;
    m_awayRoots = awayRoots;
    if (different) {
        qInfo() << "Deck Lab media:" << away.size() << "library tracks hidden on" << awayRoots.size()
                << "USB stick folders that are out" << awayRoots;
    }
    if (different || force) {
        emit changed();
        // What waited reads again once it is quiet.
        if (deferring() && !m_settle.isActive()) {
            m_settle.start();
        }
    }
    return different;
}

} // namespace decklab
