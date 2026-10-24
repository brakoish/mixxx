// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// r6-keys (tests): a USB folder's rows as Mixxx's file browser hands them to
// ProxyTrackModel: generated titles and each file's key tag as text (as a
// rekordbox export or a tag writer stores it), sorted by Qt::UserRole.
// Column 0 is the title, 1 the key, 2 the BPM. No database, no files.

#include <QHash>
#include <QSqlDatabase>
#include <QStandardItemModel>
#include <QStringList>

#include "library/trackmodel.h"

class DeckLabGeneratedFolder final : public QStandardItemModel, public TrackModel {
  public:
    explicit DeckLabGeneratedFolder(const QStringList& keys, double bpm = 122.0)
            : QStandardItemModel(static_cast<int>(keys.size()), 3),
              TrackModel(QSqlDatabase(), "mixxx.db.model.decklab.keys.folder") {
        for (int row = 0; row < keys.size(); ++row) {
            auto* title = new QStandardItem(QStringLiteral("Generated Tone %1").arg(row));
            title->setData(title->text(), Qt::UserRole);
            setItem(row, 0, title);
            auto* key = new QStandardItem(keys[row]);
            key->setData(keys[row], Qt::UserRole);
            setItem(row, 1, key);
            auto* tempo = new QStandardItem();
            tempo->setData(bpm, Qt::DisplayRole);
            tempo->setData(bpm, Qt::UserRole);
            setItem(row, 2, tempo);
        }
        setHorizontalHeaderLabels({QStringLiteral("Title"), QStringLiteral("Key"), QStringLiteral("BPM")});
    }
    Capabilities getCapabilities() const override {
        return Capability::Sorting; // as BrowseTableModel
    }
    // Header state and column choices stay in memory (no settings database).
    QString getModelSetting(const QString& name) override {
        return m_settings.value(name);
    }
    bool setModelSetting(const QString& name, const QVariant& value) override {
        m_settings.insert(name, value.toString());
        return true;
    }
    TrackPointer getTrack(const QModelIndex&) const override {
        return {};
    }
    TrackPointer getTrackByRef(const TrackRef&) const override {
        return {};
    }
    QUrl getTrackUrl(const QModelIndex&) const override {
        return {};
    }
    QString getTrackLocation(const QModelIndex& index) const override {
        // A generated path per file (never one of the decks' tracks).
        return QStringLiteral("/media/decklab/GENERATED/%1.wav").arg(index.sibling(index.row(), 0).data().toString());
    }
    TrackId getTrackId(const QModelIndex&) const override {
        return {};
    }
    CoverInfo getCoverInfo(const QModelIndex&) const override {
        return {};
    }
    const QVector<int> getTrackRows(TrackId) const override {
        return {};
    }
    void search(const QString&) override {
    }
    const QString currentSearch() const override {
        return {};
    }
    bool isColumnInternal(int) override {
        return false;
    }
    bool isColumnHiddenByDefault(int) override {
        return false;
    }
    SortColumnId sortColumnIdFromColumnIndex(int index) const override {
        switch (index) {
        case 0:
            return SortColumnId::Title;
        case 1:
            return SortColumnId::Key;
        case 2:
            return SortColumnId::Bpm;
        default:
            return SortColumnId::Invalid;
        }
    }
    int columnIndexFromSortColumnId(SortColumnId column) const override {
        switch (column) {
        case SortColumnId::Title:
            return 0;
        case SortColumnId::Key:
            return 1;
        case SortColumnId::Bpm:
            return 2;
        default:
            return -1;
        }
    }
    QString modelKey(bool) const override {
        return QStringLiteral("decklab-keys-folder");
    }
    bool updateTrackGenre(Track*, const QString&) const override {
        return false;
    }
#if defined(__EXTRA_METADATA__)
    bool updateTrackMood(Track*, const QString&) const override {
        return false;
    }
#endif

  private:
    QHash<QString, QString> m_settings;
};
