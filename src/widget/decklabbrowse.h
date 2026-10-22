// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab native Browse helpers: harmonic/tempo matching, breadcrumb
// elision, category values and the painted touch widgets used by
// the Browse page. Pure functions are kept free of widgets for unit tests.

#include <QColor>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>

#include "proto/keys.pb.h"
#include "track/track_decl.h"

class QAbstractItemModel;
class QPainter;

namespace decklab {

using ChromaticKey = mixxx::track::io::key::ChromaticKey;

// Lancelot key wheel position: 1..12, A = minor, B = major.
struct LancelotPosition {
    int number = 0;
    bool major = false;
    bool isValid() const {
        return number > 0;
    }
};
LancelotPosition lancelot(ChromaticKey key);
QString lancelotText(ChromaticKey key);

// Same key, relative major/minor (same number) or one step on the wheel
// (same letter). Diagonal moves are deliberately not highlighted.
enum class HarmonicMatch {
    None,
    Same,
    Relative,
    Adjacent,
};
HarmonicMatch harmonicMatch(ChromaticKey track, ChromaticKey reference);

// Direct tempo match within +/- tolerance (6% = a common pitch fader range).
// Half/double tempo is not considered a match.
bool tempoMatches(double trackBpm, double referenceBpm, double tolerance = 0.06);
double tempoDifferencePercent(double trackBpm, double referenceBpm);

// Parses any key notation shown by Mixxx or an export (8A, 1m, Am, A♭m...).
// Cached, GUI thread only.
ChromaticKey keyFromText(const QString& text);
QColor keyWheelColor(ChromaticKey key);

// The deck Browse compares rows with. Updated from deck controls by the
// native surface; read while painting rows. GUI thread only.
struct MatchReference {
    bool enabled = true;
    int deck = 0;
    ChromaticKey key = mixxx::track::io::key::INVALID;
    double bpm = 0;
    bool active() const {
        return enabled && deck > 0;
    }
    bool sameAs(const MatchReference& other) const;
};
const MatchReference& matchReference();
void setMatchReference(const MatchReference& reference);

// Keeps the first and the deepest segments, dropping middle ones (shown as a
// single "…") until the path fits. The last segment is never dropped.
QStringList elideBreadcrumb(const QStringList& segments,
        int width,
        const std::function<int(const QString&)>& measure,
        const QString& separator);
QString ellipsis();

enum class Category {
    AllTracks,
    Playlists,
    Crates,
    History,
    Artist,
    Album,
    Bpm,
    Key,
};
QString categoryName(Category category);

struct CategoryValue {
    QString label;
    QString query;
    int count = 0;
    QColor accent;
};
// Distinct values of one column in an existing track model, with the native
// search query that filters that model to them. No database access.
QVector<CategoryValue> collectCategoryValues(
        const QAbstractItemModel& model, int column, Category category);
QString exactTextQuery(const QString& field, const QString& value);
QString bpmBucketQuery(int bpm);

enum class Glyph {
    None,
    Tracks,
    Playlist,
    Crate,
    History,
    Artist,
    Album,
    Bpm,
    Key,
    Usb,
    Folder,
    Collection,
    Play,
    Stop,
    Back,
    Chevron,
    Search,
    More,
};
void paintGlyph(QPainter& painter, const QRectF& box, Glyph glyph, const QColor& color);

// Deck Lab brand tokens (brand/README.md).
namespace colors {
extern const QColor ground; // background
extern const QColor panel;  // panels and cards
extern const QColor raise;  // pressed and selected fills
extern const QColor edge;   // dividers and outlines ("line")
extern const QColor ink;    // primary text ("text")
extern const QColor muted;  // secondary text, 15 px and larger only
extern const QColor dim;    // disabled text, 16 px and larger
extern const QColor accent; // selection edge: white, never red
extern const QColor keyMatch;
extern const QColor tempoMatch;
extern const QColor onAir; // "live": on air, playing, refused loads
} // namespace colors

// On-device type scale for the 800x480 panel: nothing below 14 px, no 400
// weight below 18 px, no muted text below 15 px, no italics. Barlow when it
// is installed, otherwise the surface font.
QFont titleFont(const QFont& base);     // 18 px SemiBold
QFont bodyFont(const QFont& base);      // 16 px Medium
QFont strongFont(const QFont& base);    // 16 px SemiBold
QFont secondaryFont(const QFont& base); // 15 px Medium (muted)
QFont labelFont(const QFont& base);     // 14 px SemiBold uppercase
// Barlow Semi Condensed SemiBold, tabular figures: BPM, key, time.
QFont numberFont(const QFont& base, int pixels);

} // namespace decklab

// Two-line touch button: large primary label, small state line underneath.
class DeckLabActionButton : public QPushButton {
  public:
    enum class Tone {
        Neutral,
        Ready,
        OnAir,
        Active,
    };
    explicit DeckLabActionButton(const QString& text, QWidget* parent = nullptr);
    void setDetail(const QString& detail);
    void setTone(Tone tone);
    void setGlyph(decklab::Glyph glyph);
    QString detail() const {
        return m_detail;
    }
    Tone tone() const {
        return m_tone;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString m_detail;
    Tone m_tone = Tone::Neutral;
    decklab::Glyph m_glyph = decklab::Glyph::None;
};

// Path of the current Browse location. Painted, so changing it never
// relayouts the page.
class DeckLabBreadcrumb : public QWidget {
  public:
    explicit DeckLabBreadcrumb(QWidget* parent = nullptr);
    void setSegments(const QStringList& segments);
    QStringList segments() const {
        return m_segments;
    }
    QStringList displayedSegments() const;
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QStringList m_segments;
};

// Details pane for the highlighted track or Browse item. Fixed geometry and
// custom painting: a selection change only repaints this widget.
class DeckLabTrackCard : public QWidget {
  public:
    explicit DeckLabTrackCard(QWidget* parent = nullptr);
    // Covers come from Mixxx's asynchronous CoverArtCache. Enable only where
    // the application created that cache (never in widget-only tests).
    void enableCovers();
    void showTrack(const TrackPointer& track);
    void showItem(decklab::Glyph glyph,
            const QString& title,
            const QString& subtitle,
            const QString& hint,
            const QColor& accent = QColor());
    void refreshMatch();
    TrackPointer track() const {
        return m_track;
    }
    QString title() const {
        return m_title;
    }
    QString matchSummary() const;
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void paintTrack(QPainter& painter);
    void paintItem(QPainter& painter);
    void paintArtwork(QPainter& painter, const QRect& box);
    void requestCover();

    bool m_covers = false;
    bool m_itemMode = true;
    TrackPointer m_track;
    QString m_location;
    QString m_title;
    QString m_artist;
    QString m_album;
    QString m_genre;
    QString m_comment;
    QString m_keyText;
    decklab::ChromaticKey m_key = mixxx::track::io::key::INVALID;
    double m_bpm = 0;
    double m_duration = 0;
    int m_rating = 0;
    QColor m_color;
    QPixmap m_cover;
    QPixmap m_coverScaled;
    decklab::Glyph m_glyph = decklab::Glyph::Folder;
    QString m_subtitle;
    QString m_hint;
    QColor m_accent;
};

// Transient, non-modal message over the surface (for example a refused load).
class DeckLabToast : public QLabel {
  public:
    explicit DeckLabToast(QWidget* parent);
    // live: the message concerns something on air (red edge, brand "live").
    void showMessage(const QString& text, int milliseconds = 2600, bool live = false);

  private:
    QTimer m_timer;
};

// Rows of the category and value lists.
class DeckLabListDelegate : public QStyledItemDelegate {
  public:
    enum Role {
        CountRole = Qt::UserRole + 1,
        GlyphRole,
        ChevronRole,
        AccentRole,
        QueryRole,
        CategoryRole,
    };
    explicit DeckLabListDelegate(QObject* parent = nullptr);
    // Source lists (the native sidebar) supply a glyph per row; every row
    // opens something, so it gets a chevron. Item roles above are not read.
    void setGlyphProvider(std::function<decklab::Glyph(const QModelIndex&)> provider) {
        m_glyphProvider = std::move(provider);
    }
    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  private:
    std::function<decklab::Glyph(const QModelIndex&)> m_glyphProvider;
};
