// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabutilities.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtDebug>
#include <utility>

#include "moc_decklabutilities.cpp"

namespace {
QString normalized(const QString& path) {
    QString clean = QDir::cleanPath(path);
    while (clean.size() > 1 && clean.endsWith(QLatin1Char('/'))) {
        clean.chop(1);
    }
    return clean;
}
} // namespace

DeckLabUtilities::DeckLabUtilities(Hooks hooks, QObject* parent, const QString& stateDirectory)
        : QObject(parent),
          m_hooks(std::move(hooks)),
          m_stateDirectory(stateDirectory.isEmpty() ? defaultStateDirectory() : stateDirectory) {
    m_publish.setInterval(kPublishIntervalMs);
    connect(&m_publish, &QTimer::timeout, this, [this] { publishNow(); });
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &DeckLabUtilities::tryRefresh);
}

DeckLabUtilities::~DeckLabUtilities() {
    if (!m_app.isEmpty()) {
        removeStateFile();
    }
}

QString DeckLabUtilities::defaultStateDirectory() {
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        runtime = QDir::tempPath();
    }
    return QDir(runtime).filePath(QStringLiteral("decklab"));
}

QString DeckLabUtilities::stateFile() const {
    return QDir(m_stateDirectory).filePath(kStateFileName);
}

void DeckLabUtilities::setStateDirectory(const QString& directory) {
    if (!m_app.isEmpty()) {
        removeStateFile();
    }
    m_stateDirectory = directory;
    if (!m_app.isEmpty()) {
        publishNow();
    }
}

void DeckLabUtilities::setHooks(Hooks hooks) {
    m_hooks = std::move(hooks);
}

void DeckLabUtilities::onAppsChanged(const QJsonObject& data) {
    const QJsonValue app = data.value(QStringLiteral("app"));
    if (app.isString() && !app.toString().isEmpty()) {
        m_app = app.toString().left(32);
        publishNow();
        m_publish.start();
        return;
    }
    // decklabd sends {"app": null} once when the utility window has gone,
    // whether it was closed from the page, by apps.close or by itself.
    m_publish.stop();
    if (!m_app.isEmpty()) {
        removeStateFile();
    }
    m_app.clear();
    requestRefresh();
    emit closed();
}

void DeckLabUtilities::requestStartupScan() {
    request(false, false);
}

void DeckLabUtilities::onActivity() {
    const bool busy = (m_hooks.playing && m_hooks.playing()) || (m_hooks.recording && m_hooks.recording());
    if (!busy || !m_hooks.scanning || !m_hooks.scanning() || !m_hooks.interruptScan) {
        return;
    }
    // It would read the stick and write the library all set long. It runs
    // again after the next quiet spell; folders it finished are not read
    // again.
    qInfo() << "Deck Lab: playback started; the library scan stops and runs again once it is quiet";
    m_hooks.interruptScan();
    const bool announced = m_scanOwned && m_scanAnnounced;
    m_scanOwned = false;
    m_scanAnnounced = false;
    request(false, announced);
}

bool DeckLabUtilities::publishNow() {
    if (m_app.isEmpty()) {
        return false;
    }
    const bool playing = m_hooks.playing ? m_hooks.playing() : true; // unknown counts as playing
    const bool recording = m_hooks.recording ? m_hooks.recording() : false;
    const QJsonObject state{{QStringLiteral("schema"), 1},
            {QStringLiteral("writer"), QStringLiteral("mixxx")},
            {QStringLiteral("updated_at"), QDateTime::currentMSecsSinceEpoch() / 1000.0},
            {QStringLiteral("playing"), playing},
            {QStringLiteral("recording"), recording},
            {QStringLiteral("app"), m_app}};
    QDir directory(m_stateDirectory);
    if (!directory.exists()) {
        // The kiosk launcher creates it (mode 0700); keep that mode if we must.
        if (!QDir().mkpath(m_stateDirectory)) {
            return false;
        }
        QFile::setPermissions(m_stateDirectory,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    QSaveFile file(stateFile());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(state).toJson(QJsonDocument::Compact));
    file.write("\n");
    return file.commit();
}

void DeckLabUtilities::removeStateFile() {
    QFile::remove(stateFile());
}

QStringList DeckLabUtilities::missingRoots(const QStringList& folders, const QStringList& roots) {
    QStringList cleanRoots;
    for (const QString& root : roots) {
        if (!root.isEmpty()) {
            cleanRoots.append(normalized(root));
        }
    }
    QStringList missing;
    for (const QString& folder : folders) {
        if (folder.isEmpty()) {
            continue;
        }
        const QString clean = normalized(folder);
        bool covered = false;
        for (const QString& root : std::as_const(cleanRoots)) {
            const QString prefix = root.endsWith(QLatin1Char('/')) ? root : QString(root + QLatin1Char('/'));
            if (clean == root || clean.startsWith(prefix)) {
                covered = true;
                break;
            }
        }
        if (!covered && !missing.contains(clean)) {
            missing.append(clean);
        }
    }
    return missing;
}

void DeckLabUtilities::requestRefresh() {
    request(true, true);
}

void DeckLabUtilities::request(bool addRoots, bool announce) {
    m_addRoots = m_addRoots || addRoots;
    m_announce = m_announce || announce;
    if (m_pending) {
        return; // one refresh at a time; it reads every folder anyway
    }
    m_pending = true;
    m_waiting.start();
    tryRefresh();
}

void DeckLabUtilities::tryRefresh() {
    if (!m_pending) {
        return;
    }
    // r7: a scan writes the library for as long as it reads every folder:
    // never while anything plays or records, another scan runs or a utility
    // is open, nor right after (a quiet spell). New tracks may show up a
    // little later.
    if (m_hooks.quiet && !m_hooks.quiet()) {
        m_waiting.restart(); // the bounded wait for a loading deck starts when it is quiet
        m_retry.start(kQuietRetryMs);
        return;
    }
    // Loading reads the track file and its metadata; never compete with it.
    // The wait is bounded so a stuck load cannot block new tracks forever.
    if (m_hooks.loading && m_hooks.loading() && m_waiting.elapsed() < kLoadingMaxWaitMs) {
        m_retry.start(kLoadingRetryMs);
        return;
    }
    m_pending = false;
    const bool announce = std::exchange(m_announce, false);
    if (std::exchange(m_addRoots, false) && m_hooks.utilityFolders && m_hooks.libraryRoots &&
            m_hooks.addLibraryRoot) {
        const QStringList missing = missingRoots(m_hooks.utilityFolders(), m_hooks.libraryRoots());
        for (const QString& folder : missing) {
            if (m_hooks.addLibraryRoot(folder)) {
                qInfo() << "Deck Lab: added utility folder to the library:" << folder;
            } else {
                qWarning() << "Deck Lab: could not add utility folder to the library:" << folder;
            }
        }
    }
    if (m_hooks.startQuietScan && m_hooks.startQuietScan()) {
        m_scanOwned = true;
        m_scanAnnounced = announce;
        emit refreshStarted();
        if (announce) {
            emit notice(tr("Looking for new tracks…"));
        }
    }
}

void DeckLabUtilities::onScanSummary(int newTracks, bool quiet, bool canceled) {
    if (!quiet || !m_scanOwned) {
        return;
    }
    m_scanOwned = false;
    if (canceled || !std::exchange(m_scanAnnounced, false)) {
        return; // canceled: it runs again; the start-up scan says nothing
    }
    if (newTracks == 1) {
        emit notice(tr("1 new track is in your library."));
    } else if (newTracks > 1) {
        emit notice(tr("%1 new tracks are in your library.").arg(newTracks));
    } else {
        emit notice(tr("No new tracks found."));
    }
}
