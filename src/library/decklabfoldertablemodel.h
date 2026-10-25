// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>

#include "library/basesqltablemodel.h"

namespace decklab {

/// r7-browse: the song lists' filter term. The Library's track list (Tracks,
/// All tracks, song search) leaves samples out: tracks inside
/// ~/Music/Deck Lab Samples. They stay in the library, so the SAMPLES list
/// and the pads still load them. An SQL condition, " AND ..." (empty when
/// there is no music folder), for the library view's WHERE clause.
QString songListSqlFilter();

} // namespace decklab

/// r7-browse: library tracks in an order Deck Lab gives: the DOWNLOADS lists
/// (newest first, or a playlist folder in playlist order) and SAMPLES. The
/// rows are Mixxx's own library rows, so BPM, key, covers and analysis come
/// from the library, and every load path (decks, pads, preview) works as in
/// any track table. Files the library does not know yet are left out. The
/// order is fixed (no header sorting); the position column numbers the files
/// as given. r7: hidden rows (mixxx_deleted) never show, and after a USB
/// merge or rename pass each file's row is found again by its location.
class DeckLabFolderTableModel final : public BaseSqlTableModel {
    Q_OBJECT
  public:
    DeckLabFolderTableModel(QObject* parent, TrackCollectionManager* pTrackCollectionManager);
    ~DeckLabFolderTableModel() override;

    /// Shows the library's tracks at these locations in this order. key names
    /// the list (where the table keeps its place). Returns the rows shown.
    int setLocations(const QStringList& locations, const QString& key);
    QStringList locations() const {
        return m_locations;
    }
    QString listKey() const {
        return m_key;
    }

    bool isColumnInternal(int column) override;
    Capabilities getCapabilities() const override;
    QString modelKey(bool noSearch) const override;

  protected:
    void initSortColumnMapping() override;
    void refreshForMedia() override;

  private:
    // The view of the library rows at m_locations, in that order.
    bool createView();

    const QString m_viewName;
    QStringList m_locations;
    QString m_key;
};
