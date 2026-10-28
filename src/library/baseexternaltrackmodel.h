#pragma once

#include <QObject>

#include "library/trackmodel.h"
#include "library/basesqltablemodel.h"

class TrackCollection;
class QModelIndex;
class QString;

class BaseExternalTrackModel : public BaseSqlTableModel {
    Q_OBJECT
  public:
    BaseExternalTrackModel(QObject* parent,
                           TrackCollectionManager* pTrackCollectionManager,
                           const char* settingsNamespace,
                           const QString& trackTable,
                           QSharedPointer<BaseTrackCache> trackSource);
    ~BaseExternalTrackModel() override;

    Capabilities getCapabilities() const override;
    TrackId getTrackId(const QModelIndex& index) const override;
    TrackPointer getTrack(const QModelIndex& index) const override;
    // Deck Lab: getTrack() adds the row to the library (see TrackModel).
    bool resolvesTracksCheaply() const override {
        return false;
    }
    QString getTrackLocation(const QModelIndex& index) const override;
    bool isColumnInternal(int column) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

  protected:
    virtual QString resolveLocation(const QString& nativeLocation) const;

  private:
    TrackId doGetTrackId(const TrackPointer& pTrack) const override;
};
