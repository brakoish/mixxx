// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab native Browse helpers: harmonic/tempo matching, breadcrumb
// elision, category values and the painted touch widgets used by
// the Browse page. Pure functions are kept free of widgets for unit tests.

#include <QColor>
#include <QDate>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>

#include "preferences/usersettings.h" // r8-browse: Easy Mode's [DeckLab] keys
#include "proto/keys.pb.h"
#include "track/track_decl.h"

class QAbstractItemModel;
class QListWidget;
class QModelIndex;
class QPainter;
class TrackModel;

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

// Which side of the reference tempo counts as a match (the Match bar's
// slower / both / faster segments).
enum class TempoDirection {
    Slower,
    Both,
    Faster,
};
// The BPM window around the reference: 6% below and/or above it.
struct TempoWindow {
    double low = 0;
    double high = 0;
    bool isValid() const {
        return low > 0 && high >= low;
    }
};
TempoWindow tempoWindow(double referenceBpm, TempoDirection direction, double tolerance = 0.06);
bool tempoInWindow(double trackBpm, const TempoWindow& window);
// "115–129" (whole BPM, en dash).
QString tempoWindowText(const TempoWindow& window);
// Signed difference to the reference: "▲1.0" (faster), "▼2.0" (slower), "±0".
QString bpmDeltaText(double trackBpm, double referenceBpm);
// "Same key", "Relative 8A→8B", "Key +1 (up)", "Key −1 (down)"; empty otherwise.
QString keyRelationText(ChromaticKey track, ChromaticKey reference);
// r8-browse: Easy Mode, the unit's name for matching (the Match bar, row
// highlights, ONLY). What it matches on, which keys count and how wide the
// BPM window is are Settings › Library › Easy Mode, saved as [DeckLab] keys.
enum class MatchOn {
    KeyAndBpm,
    KeyOnly,
    BpmOnly,
};
enum class KeyRule {
    Wide,     // same key, relative and one step on the wheel (round 4's rule)
    Relative, // same key and relative
    Same,     // same key only
};
struct EasyMode {
    bool on = true;
    MatchOn matchOn = MatchOn::KeyAndBpm;
    KeyRule keys = KeyRule::Wide;
    int rangePercent = 6;
    bool operator==(const EasyMode&) const = default;
};
// The BPM ranges Settings offers: 3, 6 (the default), 10 and 16 percent.
QVector<int> easyModeRanges();
EasyMode loadEasyMode(const UserSettingsPointer& config);
void saveEasyMode(const UserSettingsPointer& config, const EasyMode& mode);
// Whether rule counts a relation (never None).
bool keyRuleAllows(KeyRule rule, HarmonicMatch match);

// Lancelot names that mix with the reference: same, relative, -1 and +1
// (r8-browse: fewer under a narrower rule).
QStringList compatibleLancelotKeys(ChromaticKey reference, KeyRule rule = KeyRule::Wide);

// Parses any key notation shown by Mixxx or an export (8A, 1m, Am, A♭m...).
// Cached, GUI thread only. r6-keys: decklab::parseKey (decklab/decklabkeys.h).
ChromaticKey keyFromText(const QString& text);
QColor keyWheelColor(ChromaticKey key);

// The deck Browse compares rows with. Updated from deck controls by the
// native surface; read while painting rows. GUI thread only.
struct MatchReference {
    bool enabled = true;
    int deck = 0;
    bool automatic = true; // false when the DJ pinned the deck on the Match bar
    ChromaticKey key = mixxx::track::io::key::INVALID;
    double bpm = 0;
    TempoDirection direction = TempoDirection::Both;
    // r8-browse: Easy Mode's settings. Every reader (row highlights, the
    // filter, the summary, the count, Mixes) asks through these.
    MatchOn matchOn = MatchOn::KeyAndBpm;
    KeyRule keys = KeyRule::Wide;
    double tolerance = 0.06;
    bool active() const {
        return enabled && deck > 0;
    }
    bool usesKey() const {
        return matchOn != MatchOn::BpmOnly;
    }
    bool usesTempo() const {
        return matchOn != MatchOn::KeyOnly;
    }
    // Invalid (any tempo) when BPM is not matched.
    TempoWindow window() const {
        return usesTempo() ? tempoWindow(bpm, direction, tolerance) : TempoWindow();
    }
    // Every key passes: keys are not matched, or the deck has none.
    bool anyKey() const {
        return !usesKey() || key == mixxx::track::io::key::INVALID;
    }
    // The track's relation as Easy Mode counts it: None when keys are not
    // matched or the key rule leaves the relation out.
    HarmonicMatch keyMatch(ChromaticKey track) const;
    bool sameAs(const MatchReference& other) const;
};
const MatchReference& matchReference();
void setMatchReference(const MatchReference& reference);
// One matcher for highlight and filter: the native search query (an OR of
// AND terms, Mixxx's "|" syntax) that keeps only tracks in a compatible key
// and inside the BPM window. extra (a category filter or typed words) is
// required in every alternative. Empty when there is no reference.
QString matchFilterQuery(const MatchReference& reference, const QString& extra = QString());

// USB sticks mounted for the appliance (/media/decklab/<FS-UUID>) or by a
// desktop (/media/$USER/<label>, /run/media/$USER/<label>). Linux only.
struct UsbStick {
    QString path;  // mount folder, no trailing slash
    QString label; // filesystem label, or "USB stick (3A1F)"
    bool rekordbox = false; // holds a rekordbox USB export
};
QList<UsbStick> findUsbSticks();
// Tests point the scan at generated folders (no mount-point check). Empty
// restores the real mount roots.
void setUsbStickRootsForTesting(const QStringList& roots);
bool hasRekordboxExport(const QString& path);
// Audio files directly inside folder (not recursive), and its subfolders
// without system folders ("System Volume Information", "PIONEER", dot folders).
QStringList stickAudioFiles(const QString& folder);
QStringList stickSubfolders(const QString& folder);

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
    // r7-recent: the library's tracks added in the last 30 days, newest
    // first. Listed after All Tracks (the order is the list's, not this enum's).
    RecentlyAdded,
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
// r6-keys: a shown Key values list after a key format change: each row
// relabelled and the rows in the new wheel's order, the highlight kept.
void relabelKeyValues(QListWidget& list);
QString exactTextQuery(const QString& field, const QString& value);
QString bpmBucketQuery(int bpm);
// r7-recent: Recently Added is the library's tracks added since local
// midnight kRecentlyAddedDays days before today: Mixxx's own date added
// filter, in the date format its parser reads (the locale's short date,
// quoted: some locales put spaces in it).
constexpr int kRecentlyAddedDays = 30;
QDate recentlyAddedSince(const QDate& today);
QString recentlyAddedQuery(const QDate& today);
// How many rows of an existing track model were added on or after since, from
// its date added column (the local date). No database access.
int countAddedSince(const QAbstractItemModel& model, int column, const QDate& since);
// The day a Recently Added row came in: "Today", "Yesterday", else the
// day and month as the History's days read them ("25 Oct"; a year back,
// "25 Oct 2025").
QString recentDayLabel(const QDate& day, const QDate& today);
// A track model asks its table for day dividers (Recently Added, newest
// first) with this dynamic property set (WTrackTableView's Deck Lab layout).
// r8-browse: a rule and the day's name on its first row, no column.
inline constexpr const char* kDayDividersProperty = "decklabDayDividers";
// r8-browse: a divider's day: the row's (from its model's date added
// column) when the row above came in on another day, else invalid; the
// label it shows ("TODAY", "YESTERDAY", "25 OCT") and that label's width.
QDate dayDividerOf(const QModelIndex& index, int dayColumn);
QString dayDividerText(const QDate& day, const QDate& today);
int dayDividerWidth(const QFont& base, const QString& text);
// r8-browse: Browse's track rows (36 px: the list takes the old pane's
// width; ▶ and i on the highlighted row stay 44 x 36 targets).
constexpr int kTrackRowHeight = 36;

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
    // qw-library: one glyph per concept on the source list and Match bar.
    Mic,
    Sources,
    AutoDj,
    Analyze,
    ArrowDown,
    ArrowUp,
    ArrowBoth,
    Close,
    Warning,
    Retry,
    // r4-browse: "Mixes with deck N" (two overlapping key circles).
    Mixes,
    // r7-browse: the DOWNLOADS and SAMPLES tiles.
    Download,
    Sample,
    // r7-recent: Recently Added.
    Recent,
    // r8-browse: the preview strip and the row's info button.
    Headphones,
    Info,
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
        Selected, // a chosen segment or toggle: raised fill, white outline
    };
    explicit DeckLabActionButton(const QString& text, QWidget* parent = nullptr);
    /// r5-polish: `shortDetail` is shown instead when `detail` would not fit
    /// ("37" for "1,234 tracks"), so nothing is cut off.
    void setDetail(const QString& detail, const QString& shortDetail = QString());
    void setTone(Tone tone);
    void setGlyph(decklab::Glyph glyph);
    // Deck identity (LOAD targets): a 6 px bar and a numbered badge in the
    // deck's colour, kept (dimmed) while the target is disabled. 0 = none.
    void setDeck(int deck);
    QString detail() const {
        return m_detail;
    }
    Tone tone() const {
        return m_tone;
    }
    int deck() const {
        return m_deck;
    }
    /// r5-polish: every text it paints fits uncut at its current size.
    bool textFits() const;
    /// The detail as painted: the short one when the long one doesn't fit.
    QString shownDetail() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void paintDeckTarget(QPainter& painter, const QColor& primary, const QColor& secondary);

    QString m_detail;
    QString m_shortDetail;
    Tone m_tone = Tone::Neutral;
    decklab::Glyph m_glyph = decklab::Glyph::None;
    int m_deck = 0;
};

// The Match bar's reference: the deck colour, its number, AUTO when it follows
// the playing deck, and its key and tempo. A tap cycles Auto / 1 / 2 / 3 / 4.
class DeckLabDeckChip : public QPushButton {
  public:
    explicit DeckLabDeckChip(QWidget* parent = nullptr);
    void setReference(int deck, bool automatic, const QString& detail);
    int deck() const {
        return m_deck;
    }
    bool automatic() const {
        return m_automatic;
    }
    QString detail() const {
        return m_detail;
    }
    /// r5-polish: every text it paints fits uncut at its current size.
    bool textFits() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    int m_deck = 0;
    bool m_automatic = true;
    QString m_detail;
};

// Painted two-line readout (Match bar summary): value line and label line.
class DeckLabTwoLineLabel : public QWidget {
  public:
    explicit DeckLabTwoLineLabel(QWidget* parent = nullptr);
    void setLines(const QString& primary, const QString& secondary, bool active);
    QString primary() const {
        return m_primary;
    }
    QString secondary() const {
        return m_secondary;
    }
    bool active() const {
        return m_active;
    }
    /// r5-polish: both lines fit uncut at its current width.
    bool textFits() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString m_primary;
    QString m_secondary;
    bool m_active = false;
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

// What a track list's row says about its track, read from the row's own
// columns (no file, no database). An external library's highlighted row
// (a rekordbox export) is shown from it: resolving that row to a Track
// would add it to the library on the GUI thread (TrackModel).
struct DeckLabTrackRow {
    QString location;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString comment;
    QString keyText; // as the KEY column shows it ("8A", "Am", a tag)
    double bpm = 0;
    double duration = 0; // seconds
    int rating = 0;
    QColor color;
};
DeckLabTrackRow deckLabTrackRow(const TrackModel& tracks, const QModelIndex& index);

// Details pane for the highlighted track or Browse item. Fixed geometry and
// custom painting: a selection change only repaints this widget.
class DeckLabTrackCard : public QWidget {
  public:
    explicit DeckLabTrackCard(QWidget* parent = nullptr);
    // Covers come from Mixxx's asynchronous CoverArtCache. Enable only where
    // the application created that cache (never in widget-only tests).
    void enableCovers();
    /// r6-keys: storedKeyText is the key its row stores (a rekordbox
    /// export's "8A", a USB file's tag), shown when Mixxx has no key for the
    /// track itself.
    void showTrack(const TrackPointer& track, const QString& storedKeyText = QString());
    /// r7: a row that is not resolved to a track (DeckLabTrackRow). No
    /// cover: reading it would mean the file.
    void showRow(const DeckLabTrackRow& row);
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
    /// The file of the track or row shown (empty for an item).
    QString location() const {
        return m_location;
    }
    /// r6-keys: the KEY chip's text, in the chosen key format.
    QString keyText() const;
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
    QString m_keyText; // r6-keys: as stored (a key Mixxx could not read)
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

// r8-browse: the headphone preview, shown above the deck dock only while it
// plays: a headphone glyph, the title, the track's overview (its waveform
// summary, drawn once per track or summary), the time played and a stop.
// A touch or drag on the overview seeks the preview, as on a CDJ.
class DeckLabPreviewStrip : public QWidget {
  public:
    static constexpr int kHeight = 40;
    explicit DeckLabPreviewStrip(QWidget* parent = nullptr);
    void setTrack(const TrackPointer& track);
    TrackPointer track() const {
        return m_track;
    }
    QString title() const {
        return m_title;
    }
    // fraction 0..1 of the track; seconds played.
    void setPosition(double fraction, double seconds);
    double position() const {
        return m_position;
    }
    void setSeek(std::function<void(double)> seek) {
        m_seek = std::move(seek);
    }
    void setStop(std::function<void()> stop) {
        m_stop = std::move(stop);
    }
    // Where a touch seeks and where it stops (strip coordinates).
    QRect overviewRect() const;
    QRect stopRect() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void seekAt(int x);
    void renderOverview();

    TrackPointer m_track;
    QMetaObject::Connection m_summaryConnection;
    QString m_title;
    double m_position = 0;
    double m_seconds = 0;
    bool m_seeking = false;
    bool m_stopDown = false;
    QPixmap m_overview; // the waveform summary at the overview's size
    std::function<void(double)> m_seek;
    std::function<void()> m_stop;
};

// Transient, non-modal message over the surface (for example a refused load).
class DeckLabToast : public QLabel {
  public:
    explicit DeckLabToast(QWidget* parent);
    // live: the message concerns something on air (red edge, brand "live").
    // anchor (surface coordinates, r4-browse): sit just above it, for
    // example over the LOAD target that refused, instead of over the list.
    void showMessage(const QString& text,
            int milliseconds = 2600,
            bool live = false,
            const QRect& anchor = QRect());

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
        // qw-library: two-line source rows (what is inside) and row actions.
        SubtitleRole,
        KeyRole,
        // r4-browse: source home tiles and chips (decklabbrowsehome.h).
        TileRole,     // int: 1 = tile, 2 = chip on the "Also" row
        StateRole,    // int: decklab::SourceState
        DetailRole,   // QString: a tile's second content line
        ReadOnlyRole, // bool: the stick is mounted read-only
        AlsoLeadRole, // bool: the first chip, painted after the "Also" label
        // r4-browse: rows of "Mixes with deck N" (decklabbrowsemix.h).
        MixRole,         // bool
        MixKeyRole,      // QString: the key (painted in the chosen key format)
        MixBpmRole,      // double
        MixDeltaRole,    // QString: "▼1.0"
        MixRelationRole, // QString: "same key, ▼1 BPM"
        MixMatchRole,    // int: decklab::HarmonicMatch
        MixIndexRole,    // QPersistentModelIndex of the row in the source's model
        MixArtistRole,   // QString
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
