// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QImage>
#include <QWidget>

#include "decklab/decklabphrase.h"
#include "widget/decklabdeckwidgets.h"

// The deck strip's phrase readout at `seconds` into the track: the current
// phrase and its "bars.beats" countdown. Empty without phrase data and
// outside every phrase, so the strip shows nothing extra.
DeckLabPhraseReadout deckLabPhraseReadout(const decklab::PhraseMap* map, double seconds);

// Colored phrase bands along the bottom edge of a whole-track overview, like
// a club player's phrase strip. A child of the overview widget that follows
// its size, ignores touches (needle search still reaches the overview) and
// hides itself without phrase data. It repaints only when the phrases change
// or the playhead moves to another pixel.
class DeckLabPhraseBands final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int kHeight = 8;
    explicit DeckLabPhraseBands(QWidget* overview);
    // position: play position 0..1; durationSeconds: track length.
    void setPlayback(const decklab::PhraseMapPointer& map, double durationSeconds, double position);
    const decklab::PhraseMapPointer& phrases() const {
        return m_map;
    }
    int playheadX() const {
        return m_playhead;
    }
    // Left edge of each band, for verification.
    QList<int> bandEdges() const;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void place();
    void renderBands();
    int xFor(double seconds) const;
    decklab::PhraseMapPointer m_map;
    double m_duration = 0;
    int m_playhead = -1;
    QImage m_bands; // cached at the current size
};
