// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklablabfx.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QWindow>
#include <algorithm>
#include <cmath>

#include "moc_decklablabfx.cpp"
#include "util/fpclassify.h"

namespace {
// Brand tokens. Live red only appears in the wordmark's playhead and on an
// end-of-track clock; selection is the raise fill with a white edge, "on" is
// a white fill with ink text.
const QColor kGround(0x0d, 0x0f, 0x12);
const QColor kPanel(0x16, 0x19, 0x1e);
const QColor kRaise(0x20, 0x24, 0x2b);
const QColor kLine(0x2c, 0x31, 0x3a);
const QColor kMuted(0x9a, 0xa3, 0xaf);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kInk(0x0d, 0x0f, 0x12);
const QColor kDisabledText(0x4b, 0x52, 0x5c);
const QColor kDisabledFill(0x11, 0x14, 0x18);
const QColor kDisabledLine(0x1e, 0x22, 0x28);
const QColor kPlay(0x2f, 0xd2, 0x7a);
const QColor kLive(0xff, 0x3d, 0x2e);

QColor deckColor(int deck) {
    switch (deck) {
    case 1:
        return QColor(0x35, 0xc8, 0xf0);
    case 2:
        return QColor(0xf5, 0xa5, 0x24);
    case 3:
        return QColor(0xb5, 0xe6, 0x1d);
    case 4:
        return QColor(0xc7, 0x7d, 0xff);
    default:
        return kText;
    }
}

// Free helper for the painted parts (Key and Pad are QObjects with their own
// tr() and context); every Lab FX string shares the "DeckLabLabFx" context.
QString trFx(const char* text) {
    return QCoreApplication::translate("DeckLabLabFx", text);
}

// On-device type scale: titles 18, buttons 16, secondary 15, labels 14
// SemiBold uppercase. Nothing smaller.
QFont uiFont(int pixels, QFont::Weight weight = QFont::DemiBold) {
    QFont font;
    font.setFamilies({QStringLiteral("Barlow")});
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}
QFont labelFont() {
    QFont font = uiFont(14);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    return font;
}
QFont numeralFont(int pixels) {
    QFont font;
    font.setFamilies({QStringLiteral("Barlow Semi Condensed"), QStringLiteral("Barlow")});
    font.setPixelSize(pixels);
    font.setWeight(QFont::DemiBold);
    return font;
}

// The DSP's beat fraction runs from 1/16 to 1 beat; Mixxx's beatlooproll
// sizes run on past one beat.
constexpr std::array<double, DeckLabLabFx::kBeats> kDelayBeats{0.0625, 0.125, 0.25, 0.5, 0.75, 1.0};
constexpr std::array<double, DeckLabLabFx::kBeats> kRollBeats{0.0625, 0.125, 0.25, 0.5, 1.0, 2.0};

QString beatText(double beats) {
    if (beats >= 1) {
        return QString::number(beats, 'g', 3);
    }
    if (std::abs(beats - 0.75) < 1e-9) {
        return QStringLiteral("3/4");
    }
    return QStringLiteral("1/%1").arg(qRound(1 / beats));
}

// Qt posts an accessibility event for every setAccessibleName, changed or
// not; the tick must not.
void setAccessibleNameIf(QWidget* widget, const QString& name) {
    if (widget->accessibleName() != name) {
        widget->setAccessibleName(name);
    }
}

const QString kPlayItem = QStringLiteral("play");
const QString kLoadedItem = QStringLiteral("track_loaded");
const QString kBpmItem = QStringLiteral("bpm");
const QString kLocalBpmItem = QStringLiteral("local_bpm");
const QString kPhaseItem = QStringLiteral("beat_distance");
const QString kLeaderItem = QStringLiteral("sync_leader");
const QString kEnabledItem = QStringLiteral("enabled");
const QString kMixItem = QStringLiteral("mix");

QString deckGroup(int deck) {
    static const std::array<QString, 4> groups{QStringLiteral("[Channel1]"),
            QStringLiteral("[Channel2]"),
            QStringLiteral("[Channel3]"),
            QStringLiteral("[Channel4]")};
    return groups[std::clamp(deck, 1, 4) - 1];
}
QString samplerGroup(int slot) {
    static const std::array<QString, 16> groups = [] {
        std::array<QString, 16> names;
        for (int n = 0; n < 16; ++n) {
            names[n] = QStringLiteral("[Sampler%1]").arg(n + 1);
        }
        return names;
    }();
    return groups[std::clamp(slot, 1, 16) - 1];
}

// The Lab FX wordmark from the brand kit (logo/labfx-wordmark.svg): LAB,
// the red playhead, FX.
const char kWordmarkSvg[] = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 3339 899">
<path d="M51.63 792.33V104.33H247.63V628.17H499.32V792.33Z" fill="#F2F4F7"/>
<path d="M535.42 792.33 711.42 104.33H967.76L1143.76 792.33H936.74L917.6 692.7H754.89L736.42 792.33ZM784.45 540.01H889.05L863.95 401.07Q861.92 391.04 859.22 375.36Q856.53 359.67 853.5 341Q850.47 322.33 847.12 303.83Q843.76 285.33 841.08 270.67H832.42Q829.74 289.33 825.87 312.66Q822 335.99 817.96 359.17Q813.92 382.36 809.55 401.07Z" fill="#F2F4F7"/>
<path d="M1218.63 792.33V104.33H1544.39Q1597.71 104.33 1639.07 124.19Q1680.42 144.04 1704.12 181.05Q1727.82 218.07 1727.82 271.36Q1727.82 316.75 1713.79 348.42Q1699.76 380.09 1676.07 400.57Q1652.37 421.04 1622.68 433.33V437.33Q1655.39 447.3 1681.93 465.82Q1708.47 484.33 1724.5 516.7Q1740.53 549.07 1740.53 600.12Q1740.53 658.41 1714.18 701.71Q1687.84 745.01 1642.18 768.67Q1596.53 792.33 1538.55 792.33ZM1412 632.33H1501.05Q1520.1 632.33 1532.13 616.33Q1544.16 600.33 1544.16 568.33Q1544.16 553.33 1538.64 540.83Q1533.13 528.33 1523.28 521.33Q1513.42 514.33 1501.05 514.33H1412ZM1412 372.33H1491.63Q1505 372.33 1514.68 364.83Q1524.37 357.33 1529.55 343.33Q1534.74 329.33 1534.74 308.33Q1534.74 286.33 1522.37 270.83Q1510 255.33 1491.63 255.33H1412Z" fill="#F2F4F7"/>
<path d="M2302.63 792.33V104.33H2727.71V264.72H2498.63V391.28H2687.95V550.59H2498.63V792.33Z" fill="#F2F4F7"/>
<path d="M2739.84 792.33 2914.13 428.99 2758.58 104.33H2977.55L3046.68 284.94H3050.68L3120.15 104.33H3326.37L3171.13 429.33L3344.1 792.33H3124.13L3037.92 573.72H3033.92L2947.05 792.33Z" fill="#F2F4F7"/>
<polygon points="1911,0 2101,0 2006,121" fill="#FF3D2E"/>
<rect x="1982" y="60.5" width="48" height="838.16" fill="#FF3D2E"/>
</svg>)SVG";

} // namespace

namespace decklab::labfx {

// Brand wordmark, 24 px cap height (the brand's on-screen minimum).
class Wordmark final : public QWidget {
  public:
    explicit Wordmark(QWidget* parent)
            : QWidget(parent) {
        setAccessibleName(QStringLiteral("Lab FX"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    static QSize preferredSize() {
        return {117, 32};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        static QSvgRenderer renderer{QByteArray(kWordmarkSvg)};
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QSizeF view = renderer.viewBoxF().size();
        const double scale = std::min(width() / view.width(), height() / view.height());
        const QSizeF size(view.width() * scale, view.height() * scale);
        renderer.render(&painter, QRectF(QPointF(0, (height() - size.height()) / 2), size));
    }
};

// A painted key. Every Lab FX control is one of these: tiles, beat cells,
// targets, band lamps, ON and the footer actions. Repaints only when a
// shown value changes; acts on touch-down (QPushButton::pressed).
class Key final : public QPushButton {
  public:
    enum class Kind {
        Tile,
        Beat,
        Arrow,
        Target,
        Band,
        Action,
        On,
    };
    Key(Kind kind, QWidget* parent)
            : QPushButton(parent),
              m_kind(kind) {
        // Touch keys, like every Deck Lab button: focus never moves here.
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void setLabel(const QString& label) {
        if (label == m_label) {
            return;
        }
        m_label = label;
        setText(label);
        update();
    }
    const QString& label() const {
        return m_label;
    }
    void setCaption(const QString& caption) {
        if (caption != m_caption) {
            m_caption = caption;
            update();
        }
    }
    const QString& caption() const {
        return m_caption;
    }
    void setIcon(int icon) {
        m_icon = icon;
    }
    void setDeck(int deck) {
        m_deck = deck;
    }
    void setDirection(int direction) {
        m_direction = direction;
    }
    void setSelected(bool selected) {
        if (selected != m_selected) {
            m_selected = selected;
            update();
        }
    }
    bool isSelected() const {
        return m_selected;
    }
    void setLit(bool lit) {
        if (lit != m_lit) {
            m_lit = lit;
            update();
        }
    }
    bool isLit() const {
        return m_lit;
    }
    // Targets: an empty deck. Its colour stays (identity), only the number dims.
    void setDim(bool dim) {
        if (dim != m_dim) {
            m_dim = dim;
            update();
        }
    }
    // ON while the tail rings: TAIL in a white outline that follows the beat.
    void setTail(bool tail) {
        if (tail != m_tail) {
            m_tail = tail;
            update();
        }
    }
    void setOutlineBright(bool bright) {
        if (bright != m_outlineBright) {
            m_outlineBright = bright;
            if (m_tail) {
                update();
            }
        }
    }
    void setActive(bool enabled) {
        if (enabled != isEnabled()) {
            setEnabled(enabled);
        }
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool enabled = isEnabled();
        QColor fill = kPanel, border = kLine, ink = kText, caption = kMuted;
        if (m_kind == Kind::Band && !m_selected) {
            ink = kMuted;
        }
        if (m_kind == Kind::On && !m_lit) {
            ink = m_tail ? kText : kMuted; // dry: a quiet outline
        }
        if (m_selected && (m_kind == Kind::Tile || m_kind == Kind::Beat || m_kind == Kind::Band)) {
            fill = kRaise;
        }
        if (isDown()) {
            fill = kRaise;
        }
        if (m_lit) {
            fill = kText;
            border = kText;
            ink = kInk;
            caption = kInk;
        }
        if (m_kind == Kind::Target && m_selected) {
            fill = m_deck ? deckColor(m_deck) : kText;
            border = fill;
            ink = kInk;
        }
        if (!enabled) {
            fill = kDisabledFill;
            border = kDisabledLine;
            ink = kDisabledText;
            caption = kDisabledText;
        }
        painter.fillRect(rect(), kGround);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(border, 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(box, 3, 3);
        if (m_kind == Kind::On && !m_lit && m_tail && enabled) {
            // TAIL: a white outline that follows the beat.
            painter.setPen(QPen(m_outlineBright ? kText : kLine, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 3, 3);
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
        if (m_selected && enabled && !m_lit) {
            if (m_kind == Kind::Tile) {
                painter.fillRect(QRect(0, 0, 3, height()), kText);
            } else if (m_kind == Kind::Beat) {
                painter.fillRect(QRect(0, height() - 3, width(), 3), kText);
            }
        }
        switch (m_kind) {
        case Kind::Tile:
            paintTile(painter, ink);
            break;
        case Kind::Beat:
            painter.setFont(numeralFont(18));
            painter.setPen(ink);
            painter.drawText(rect(), Qt::AlignCenter, m_label);
            break;
        case Kind::Arrow:
            paintArrow(painter, ink);
            break;
        case Kind::Target:
            paintTarget(painter, ink, enabled);
            break;
        case Kind::Band:
            paintBand(painter, ink, caption, enabled);
            break;
        case Kind::Action:
        case Kind::On:
            paintTwoLine(painter, ink, caption);
            break;
        }
    }

  private:
    void paintTile(QPainter& painter, const QColor& ink) {
        const bool roomy = width() >= 104;
        const int iconSize = 24;
        const QRect iconBox(roomy ? 14 : (width() - iconSize) / 2,
                roomy ? (height() - iconSize) / 2 : 6,
                iconSize,
                iconSize);
        if (roomy || height() >= 64) {
            paintIcon(painter, iconBox, ink);
        }
        const QRect text = roomy ? rect().adjusted(iconBox.right() + 12, 0, -6, 0)
                                 : rect().adjusted(4, height() >= 64 ? 30 : 0, -4, 0);
        painter.setFont(uiFont(16));
        if (QFontMetrics(painter.font()).horizontalAdvance(m_label) > text.width()) {
            painter.setFont(labelFont());
        }
        painter.setPen(ink);
        painter.drawText(text,
                (roomy ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter,
                QFontMetrics(painter.font()).elidedText(m_label, Qt::ElideRight, text.width()));
    }
    // 24 px effect icons that say what each effect does: fading bars, a
    // wash, a slope, repeats, a pump, a clipped wave.
    void paintIcon(QPainter& painter, const QRect& box, const QColor& ink) {
        painter.save();
        painter.translate(box.topLeft());
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor faded = ink;
        switch (m_icon) {
        case 0: { // ECHO
            const std::array<int, 4> heights{20, 15, 10, 6};
            for (int n = 0; n < 4; ++n) {
                faded.setAlphaF(1.0f - 0.22f * n);
                painter.fillRect(QRectF(1 + n * 6, 22 - heights[n], 3.5, heights[n]), faded);
            }
            break;
        }
        case 1: { // REVERB
            painter.fillRect(QRectF(1, 2, 3.5, 20), ink);
            QPainterPath wash;
            wash.moveTo(6, 4);
            wash.cubicTo(10, 6, 14, 16, 23, 21);
            wash.lineTo(23, 22);
            wash.lineTo(6, 22);
            wash.closeSubpath();
            faded.setAlphaF(0.45f);
            painter.fillPath(wash, faded);
            painter.setPen(QPen(ink, 1.5));
            painter.drawLine(QPointF(6, 22), QPointF(23, 22));
            break;
        }
        case 2: { // FILTER
            QPainterPath slope;
            slope.moveTo(1, 7);
            slope.lineTo(11, 7);
            slope.cubicTo(16, 7, 18, 14, 23, 21);
            painter.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(slope);
            break;
        }
        case 3: { // ROLL
            for (int n = 0; n < 3; ++n) {
                painter.fillRect(QRectF(2 + n * 7, 3, 5, 9), ink);
            }
            painter.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            QPainterPath back;
            back.moveTo(20, 15);
            back.lineTo(20, 20);
            back.lineTo(4, 20);
            back.lineTo(4, 16);
            painter.drawPath(back);
            QPainterPath head;
            head.moveTo(1, 17);
            head.lineTo(4, 13.5);
            head.lineTo(7, 17);
            painter.drawPath(head);
            break;
        }
        case 4: { // DUCK
            const std::array<QPointF, 9> pump{QPointF(1, 6),
                    QPointF(5, 6),
                    QPointF(7, 19),
                    QPointF(11, 8),
                    QPointF(13, 6),
                    QPointF(15, 6),
                    QPointF(17, 19),
                    QPointF(21, 8),
                    QPointF(23, 6)};
            painter.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPolyline(pump.data(), int(pump.size()));
            break;
        }
        case 5: { // DRIVE
            const std::array<QPointF, 8> clipped{QPointF(1, 12),
                    QPointF(4, 5),
                    QPointF(8, 5),
                    QPointF(12, 12),
                    QPointF(15, 19),
                    QPointF(19, 19),
                    QPointF(23, 12),
                    QPointF(23, 12)};
            painter.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPolyline(clipped.data(), int(clipped.size()));
            break;
        }
        default:
            break;
        }
        painter.restore();
    }
    void paintArrow(QPainter& painter, const QColor& ink) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPointF centre = QRectF(rect()).center();
        QPainterPath triangle;
        const double w = 9, h = 14;
        if (m_direction < 0) {
            triangle.moveTo(centre.x() + w / 2, centre.y() - h / 2);
            triangle.lineTo(centre.x() + w / 2, centre.y() + h / 2);
            triangle.lineTo(centre.x() - w / 2, centre.y());
        } else {
            triangle.moveTo(centre.x() - w / 2, centre.y() - h / 2);
            triangle.lineTo(centre.x() - w / 2, centre.y() + h / 2);
            triangle.lineTo(centre.x() + w / 2, centre.y());
        }
        triangle.closeSubpath();
        painter.fillPath(triangle, ink);
    }
    void paintTarget(QPainter& painter, const QColor& ink, bool enabled) {
        if (m_deck) {
            // Deck identity: the number in its deck colour over a deck
            // colour bar; the selected target fills with it, like the badge.
            const QColor identity = deckColor(m_deck);
            if (!m_selected && enabled) {
                painter.fillRect(QRect(3, height() - 6, width() - 6, 3), identity);
            }
            painter.setFont(numeralFont(20));
            painter.setPen(m_selected ? ink : !enabled ? kDisabledText : m_dim ? kMuted : identity);
            painter.drawText(rect().adjusted(0, 0, 0, -3), Qt::AlignCenter, m_label);
            return;
        }
        painter.setFont(uiFont(16));
        painter.setPen(m_selected ? ink : m_dim ? kMuted : ink);
        painter.drawText(rect(), Qt::AlignCenter, m_label);
    }
    void paintBand(QPainter& painter, const QColor& ink, const QColor& caption, bool enabled) {
        // A lamp: white dot and label when the band gets the effect, a
        // hollow dot and DRY when it stays dry.
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPointF dot(16, height() / 2.0 - 9);
        if (m_selected && enabled) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_lit ? kInk : kText);
        } else {
            painter.setPen(QPen(enabled ? kMuted : kDisabledText, 1.5));
            painter.setBrush(Qt::NoBrush);
        }
        painter.drawEllipse(dot, 4.5, 4.5);
        painter.setFont(uiFont(16));
        painter.setPen(ink);
        painter.drawText(QRectF(28, dot.y() - 12, width() - 32, 24), Qt::AlignLeft | Qt::AlignVCenter, m_label);
        painter.setFont(labelFont());
        painter.setPen(caption);
        painter.drawText(QRectF(28, dot.y() + 10, width() - 32, 20), Qt::AlignLeft | Qt::AlignVCenter, m_caption);
    }
    void paintTwoLine(QPainter& painter, const QColor& ink, const QColor& caption) {
        const QFont main = m_kind == Kind::On ? uiFont(18) : uiFont(16);
        const QFontMetrics metrics(main);
        if (m_caption.isEmpty()) {
            painter.setFont(main);
            painter.setPen(ink);
            painter.drawText(rect(), Qt::AlignCenter, m_label);
            return;
        }
        const QFont small = labelFont();
        const QFontMetrics smallMetrics(small);
        const int total = metrics.height() + smallMetrics.height() - 4;
        const int top = (height() - total) / 2;
        painter.setFont(main);
        painter.setPen(ink);
        painter.drawText(QRect(4, top, width() - 8, metrics.height()), Qt::AlignCenter, m_label);
        painter.setFont(small);
        painter.setPen(caption);
        painter.drawText(QRect(4, top + metrics.height() - 4, width() - 8, smallMetrics.height()),
                Qt::AlignCenter,
                smallMetrics.elidedText(m_caption, Qt::ElideRight, width() - 8));
    }

    Kind m_kind;
    QString m_label;
    QString m_caption;
    int m_icon = -1;
    int m_deck = 0;
    int m_direction = 1;
    bool m_selected = false;
    bool m_lit = false;
    bool m_dim = false;
    bool m_tail = false;
    bool m_outlineBright = true;
};

// Header readout: what plays, where, at what tempo.
class Readout final : public QWidget {
  public:
    explicit Readout(QWidget* parent)
            : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void setLines(const QString& title, const QString& subtitle) {
        if (title == m_title && subtitle == m_subtitle) {
            return;
        }
        m_title = title;
        m_subtitle = subtitle;
        setAccessibleName(title + QStringLiteral(", ") + subtitle);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const QFont title = uiFont(18);
        const QFont subtitle = uiFont(15, QFont::Medium);
        const QFontMetrics titleMetrics(title), subtitleMetrics(subtitle);
        const int total = titleMetrics.height() + subtitleMetrics.height() - 2;
        const int top = (height() - total) / 2;
        painter.setFont(title);
        painter.setPen(kText);
        painter.drawText(QRect(0, top, width(), titleMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(m_title, Qt::ElideRight, width()));
        painter.setFont(subtitle);
        painter.setPen(kMuted);
        painter.drawText(QRect(0, top + titleMetrics.height() - 2, width(), subtitleMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                subtitleMetrics.elidedText(m_subtitle, Qt::ElideRight, width()));
    }

  private:
    QString m_title;
    QString m_subtitle;
};

// Footer readout of the target: the tempo deck's badge, play state, title,
// BPM, four beat lamps and time left; for SMP, a link to the Sampler page.
class DeckReadout final : public QPushButton {
  public:
    struct State {
        int deck = 0;      // badge; 0: none
        bool master = false;
        bool sampler = false;
        int samplersPlaying = 0;
        int samplersLoaded = 0;
        // r4-waveforms: the waveform lanes' DECKS row is right above and
        // shows the beat and the time left; the footer then only names the
        // target (badge, play state, title).
        bool compact = false;
        DeckLabLabFxDeck info;
        bool operator==(const State&) const = default;
    };
    explicit DeckReadout(QWidget* parent)
            : QPushButton(parent) {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void setState(const State& state) {
        if (state == m_state) {
            return;
        }
        m_state = state;
        update();
    }
    const State& state() const {
        return m_state;
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), kGround);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(kLine, 1));
        painter.setBrush(m_state.sampler && isDown() ? kRaise : kPanel);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        const int middle = height() / 2;
        int x = 10;
        if (m_state.sampler) {
            const QRect badge(x, middle - 13, 46, 26);
            painter.setPen(QPen(kText, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(badge).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
            painter.setFont(labelFont());
            painter.setPen(kText);
            painter.drawText(badge, Qt::AlignCenter, trFx("SMP"));
            x = badge.right() + 10;
            const QString link = trFx("SAMPLER ›");
            painter.setFont(uiFont(16));
            const int linkWidth = QFontMetrics(painter.font()).horizontalAdvance(link);
            painter.drawText(QRect(width() - 12 - linkWidth, 0, linkWidth, height()), Qt::AlignVCenter, link);
            painter.setFont(uiFont(15, QFont::Medium));
            painter.setPen(kMuted);
            const QString count = m_state.samplersLoaded
                    ? trFx("%1 PLAYING · %2 LOADED").arg(m_state.samplersPlaying).arg(m_state.samplersLoaded)
                    : trFx("NO SAMPLES LOADED");
            const int room = width() - 12 - linkWidth - 10 - x;
            painter.drawText(QRect(x, 0, room, height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(painter.font()).elidedText(count, Qt::ElideRight, room));
            return;
        }
        if (!m_state.deck) {
            painter.setFont(uiFont(15, QFont::Medium));
            painter.setPen(kMuted);
            painter.drawText(rect().adjusted(12, 0, -12, 0),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    m_state.master ? trFx("MASTER · no deck loaded yet") : trFx("No deck"));
            return;
        }
        const QColor identity = deckColor(m_state.deck);
        const QRect badge(x, middle - 13, 30, 26);
        if (m_state.info.loaded) {
            painter.fillRect(badge, identity);
            painter.setPen(kInk);
        } else {
            painter.setPen(QPen(identity, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(badge).adjusted(0.75, 0.75, -0.75, -0.75));
            painter.setPen(identity);
        }
        painter.setFont(numeralFont(18));
        painter.drawText(badge, Qt::AlignCenter, QString::number(m_state.deck));
        x = badge.right() + 10;
        // Right block first, so the title takes what is left.
        int right = width() - 12;
        if (m_state.info.loaded && !m_state.compact) {
            painter.setFont(numeralFont(24));
            const QFontMetrics clockMetrics(painter.font());
            const int clockWidth = clockMetrics.horizontalAdvance(QStringLiteral("−00:00"));
            painter.setPen(m_state.info.endWarning ? kLive : kText);
            painter.drawText(QRect(right - clockWidth, 0, clockWidth, height()),
                    Qt::AlignRight | Qt::AlignVCenter,
                    m_state.info.remaining);
            right -= clockWidth + 12;
            // Four beat lamps in the deck colour.
            const int lamp = 10, gap = 3;
            right -= 4 * lamp + 3 * gap;
            for (int n = 0; n < 4; ++n) {
                painter.fillRect(QRect(right + n * (lamp + gap), middle - lamp / 2, lamp, lamp),
                        n == m_state.info.phase ? identity : kLine);
            }
            right -= 14;
        }
        if (m_state.info.loaded) {
            // Play state: green ▶ while playing, muted ❚❚ while paused.
            QPainterPath glyph;
            const double top = middle - 7;
            if (m_state.info.playing) {
                glyph.moveTo(x, top);
                glyph.lineTo(x, top + 14);
                glyph.lineTo(x + 12, top + 7);
                glyph.closeSubpath();
                painter.fillPath(glyph, kPlay);
            } else {
                painter.fillRect(QRectF(x, top, 4, 14), kMuted);
                painter.fillRect(QRectF(x + 7, top, 4, 14), kMuted);
            }
            x += 22;
        }
        QString title = m_state.info.loaded ? m_state.info.title : trFx("Empty");
        if (m_state.master) {
            title = trFx("MASTER · ") + title;
        }
        painter.setFont(uiFont(16));
        painter.setPen(m_state.info.loaded ? kText : kMuted);
        const int room = std::max(0, right - x);
        painter.drawText(QRect(x, 0, room, height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(painter.font()).elidedText(title, Qt::ElideRight, room));
    }

  private:
    State m_state;
};

// The X-PAD: touch-down plays, the finger sets the parameter (X) and depth
// (Y), lift returns, double-tap latches. No time limit while it is held.
class Pad final : public QWidget {
  public:
    explicit Pad(DeckLabLabFx* owner)
            : QWidget(owner),
              m_owner(owner) {
        setObjectName(QStringLiteral("decklab-fx-xy"));
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAccessibleName(trFx("Lab FX pad"));
        setAccessibleDescription(
                trFx("Hold to play: across chooses the beat or the sweep, up is "
                   "depth. Lift to return. Double-tap to latch."));
    }
    void setColumnEdges(const std::array<double, DeckLabLabFx::kBeats + 1>& edges) {
        m_edges = edges;
        update();
    }
    const std::array<double, DeckLabLabFx::kBeats + 1>& columnEdges() const {
        return m_edges;
    }
    void resetTouch() {
        m_down = false;
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        m_down = true;
        m_owner->padPressed(normalised(event->position()));
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_down) {
            m_owner->padMoved(normalised(event->position()));
        }
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !m_down) {
            return;
        }
        m_down = false;
        m_owner->padReleased();
    }
    // No hideEvent: leaving the page does not end a hold. The finger's
    // release does, or the next touch anywhere (DeckLabLabFx::noteTouch).
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto& fx = *m_owner;
        const QRect box = rect();
        painter.fillRect(box, kGround);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(kLine, 1));
        painter.setBrush(kPanel);
        painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QRect inner = box.adjusted(12, 8, -12, -8);
        if (!fx.m_available) {
            paintEmpty(painter);
            return;
        }
        const DeckLabLabFx::Effect effect = fx.m_effect;
        const bool beat = fx.usesBeat(effect);
        const QPointF spot = fx.spot();
        const QPointF at(spot.x() * width(), (1 - spot.y()) * height());
        const bool playing = fx.m_on && fx.m_voiceEffect == effect;
        if (beat) {
            // X-PAD columns under the beat cells; the chosen one is raised.
            const int current = fx.beatIndex();
            for (int column = 0; column < DeckLabLabFx::kBeats; ++column) {
                const int left = int(std::round(m_edges[column] * width()));
                const int right = int(std::round(m_edges[column + 1] * width()));
                if (column == current) {
                    painter.fillRect(QRect(left + 1, 1, right - left - 1, height() - 2), kRaise);
                }
                if (column > 0) {
                    painter.fillRect(QRect(left, 1, 1, height() - 2), kLine);
                }
            }
        } else {
            // Bipolar sweep: dry in the middle.
            painter.fillRect(QRect(width() / 2, 1, 1, height() - 2), kLine);
            painter.setFont(labelFont());
            painter.setPen(kMuted);
            const auto [left, right] = axisLabels(effect);
            painter.drawText(inner, Qt::AlignLeft | Qt::AlignVCenter, left);
            painter.drawText(inner, Qt::AlignRight | Qt::AlignVCenter, right);
            painter.drawText(QRect(width() / 2 + 6, inner.top(), 60, 20), Qt::AlignLeft | Qt::AlignVCenter, trFx("DRY"));
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setFont(labelFont());
        painter.setPen(kMuted);
        if (effect != DeckLabLabFx::Effect::Roll) {
            // Depth runs up the pad.
            QPainterPath up;
            up.moveTo(inner.left(), inner.top() + 13);
            up.lineTo(inner.left() + 5, inner.top() + 5);
            up.lineTo(inner.left() + 10, inner.top() + 13);
            up.closeSubpath();
            painter.fillPath(up, kMuted);
            painter.drawText(QRect(inner.left() + 16, inner.top(), 100, 18), Qt::AlignLeft | Qt::AlignVCenter, trFx("DEPTH"));
        }
        const bool touched = fx.m_held;
        if (playing || touched) {
            QColor cross = kText;
            cross.setAlphaF(0.35f);
            painter.setPen(QPen(cross, 1));
            painter.drawLine(QPointF(at.x(), 1), QPointF(at.x(), height() - 1));
            if (effect != DeckLabLabFx::Effect::Roll) {
                painter.drawLine(QPointF(1, at.y()), QPointF(width() - 1, at.y()));
            }
            painter.setPen(QPen(kText, 2));
            painter.setBrush(fx.isLatched() ? QBrush(kText) : QBrush(Qt::NoBrush));
            painter.drawEllipse(at, 14, 14);
            if (fx.isLatched()) {
                painter.setBrush(kInk);
                painter.setPen(Qt::NoPen);
                painter.drawEllipse(at, 4, 4);
            }
            // What the finger is doing, in words.
            painter.setFont(uiFont(16));
            painter.setPen(kText);
            painter.drawText(inner, Qt::AlignTop | Qt::AlignRight, liveText());
        } else {
            // Where ON (or the next touch at the same place) will play.
            painter.setPen(QPen(kMuted, 1.5, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(at, 12, 12);
        }
        if (!touched) {
            const QStringList lines = fx.padHint().split(QLatin1Char('\n'));
            painter.setFont(labelFont());
            const QFontMetrics metrics(painter.font());
            int y = inner.bottom() - lines.size() * metrics.height() + 1;
            for (const QString& line : lines) {
                painter.setPen(kMuted);
                painter.drawText(QRect(inner.left(), y, inner.width(), metrics.height()),
                        Qt::AlignHCenter | Qt::AlignVCenter,
                        metrics.elidedText(line, Qt::ElideRight, inner.width()));
                y += metrics.height();
            }
        }
    }

  private:
    static std::pair<QString, QString> axisLabels(DeckLabLabFx::Effect effect) {
        switch (effect) {
        case DeckLabLabFx::Effect::Duck:
            return {trFx("ON-BEAT"), trFx("OFF-BEAT")};
        case DeckLabLabFx::Effect::Drive:
            return {trFx("CUT"), trFx("DRIVE")};
        default:
            return {trFx("LPF"), trFx("HPF")};
        }
    }
    QString liveText() const {
        const auto& fx = *m_owner;
        const QString depth = trFx("DEPTH %1").arg(qRound(fx.depth() * 100));
        if (fx.m_effect == DeckLabLabFx::Effect::Roll) {
            return trFx("ROLL %1 BEAT").arg(beatText(fx.beatValue()));
        }
        if (fx.usesBeat(fx.m_effect)) {
            return trFx("%1 BEAT · %2").arg(beatText(fx.beatValue()), depth);
        }
        const double x = fx.spot().x() * 2 - 1;
        const auto [left, right] = axisLabels(fx.m_effect);
        const int amount = qRound(std::abs(x) * 100);
        return (amount == 0 ? trFx("DRY") : QStringLiteral("%1 %2").arg(x < 0 ? left : right).arg(amount)) +
                QStringLiteral(" · ") + depth;
    }
    void paintEmpty(QPainter& painter) {
        const auto& fx = *m_owner;
        QString title, detail;
        if (!fx.m_ready) {
            title = trFx("Lab FX effect didn't load.");
            detail = trFx("Restart Deck Lab.");
        } else if (fx.m_target == DeckLabLabFx::kSampler) {
            title = trFx("Load a sample to play Lab FX on the samplers");
            detail = trFx("Samples live on the Sampler page.");
        } else if (fx.m_target == DeckLabLabFx::kMaster) {
            title = trFx("Load a track to play Lab FX");
            detail = trFx("MASTER plays it on every deck and sampler.");
        } else {
            title = trFx("Load a track on deck %1 to play Lab FX").arg(fx.m_target);
            detail = trFx("Or choose another deck above.");
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect area = rect().adjusted(16, 0, -16, 0);
        const bool action = fx.m_padAction && !fx.m_padAction->isHidden();
        const int top = height() / 2 - (action ? 60 : 24);
        painter.setFont(uiFont(18));
        painter.setPen(kText);
        painter.drawText(QRect(area.left(), top, area.width(), 26),
                Qt::AlignCenter,
                QFontMetrics(painter.font()).elidedText(title, Qt::ElideRight, area.width()));
        painter.setFont(uiFont(15, QFont::Medium));
        painter.setPen(kMuted);
        painter.drawText(QRect(area.left(), top + 28, area.width(), 22),
                Qt::AlignCenter,
                QFontMetrics(painter.font()).elidedText(detail, Qt::ElideRight, area.width()));
    }
    QPointF normalised(QPointF position) const {
        return {std::clamp(position.x() / std::max(1, width()), 0.0, 1.0),
                1 - std::clamp(position.y() / std::max(1, height()), 0.0, 1.0)};
    }

    DeckLabLabFx* m_owner;
    std::array<double, DeckLabLabFx::kBeats + 1> m_edges{0, 1 / 6.0, 2 / 6.0, 3 / 6.0, 4 / 6.0, 5 / 6.0, 1};
    bool m_down = false;
};

} // namespace decklab::labfx

using decklab::labfx::DeckReadout;
using decklab::labfx::Key;
using decklab::labfx::Pad;
using decklab::labfx::Readout;
using decklab::labfx::Wordmark;

DeckLabLabFx::DeckLabLabFx(DeckLabLabFxHost host, QWidget* parent)
        : QWidget(parent),
          m_host(std::move(host)) {
    setObjectName(QStringLiteral("decklab-labfx"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Every control paints itself. This sheet only stops the surface sheet's
    // button box (min-height, padding) from constraining the fixed geometry.
    setStyleSheet(QStringLiteral("QPushButton{min-height:0px;max-height:16777215px;padding:0px;"
                                 "margin:0px;border:0px;background:transparent;}"));
    for (int unit = 0; unit < 2; ++unit) {
        m_unitGroups[unit] = QStringLiteral("[DeckLabEffectUnit%1]").arg(unit + 1);
        m_slotGroups[unit] = QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit + 1);
        m_units[unit].written.fill(-1000);
    }
    for (int parameter = 1; parameter < 16; ++parameter) {
        m_parameterNames[parameter] = QStringLiteral("parameter%1").arg(parameter);
    }
    for (int deck = 1; deck <= 4; ++deck) {
        m_deckRoutes[deck - 1] = QStringLiteral("group_[Channel%1]_enable").arg(deck);
    }
    for (int slot = 1; slot <= 16; ++slot) {
        m_samplerRoutes[slot - 1] = QStringLiteral("group_[Sampler%1]_enable").arg(slot);
    }
    // Depth 65 (the effect's own default) half way up; the bipolar sweeps
    // start well off centre so ON is never silent.
    const double depthSpot = (0.65 - 0.2) / 0.8;
    m_spots = {QPointF(0.5, depthSpot),
            QPointF(0.5, depthSpot),
            QPointF(0.8, depthSpot),
            QPointF(0.5, depthSpot),
            QPointF(0.2, depthSpot),
            QPointF(0.8, depthSpot)};
    build();
    refresh();
}

DeckLabLabFx::~DeckLabLabFx() = default;

QString DeckLabLabFx::effectName(Effect effect) {
    switch (effect) {
    case Effect::Echo:
        return tr("ECHO");
    case Effect::Reverb:
        return tr("REVERB");
    case Effect::Filter:
        return tr("FILTER");
    case Effect::Roll:
        return tr("ROLL");
    case Effect::Duck:
        return tr("DUCK");
    case Effect::Drive:
        return tr("DRIVE");
    }
    return {};
}

void DeckLabLabFx::build() {
    m_wordmark = new Wordmark(this);
    m_wordmark->setObjectName(QStringLiteral("decklab-fx-wordmark"));
    m_readout = new Readout(this);
    m_readout->setObjectName(QStringLiteral("decklab-fx-readout"));
    m_onKey = new Key(Key::Kind::On, this);
    m_onKey->setObjectName(QStringLiteral("decklab-fx-on"));
    m_onKey->setLabel(tr("ON"));
    connect(m_onKey, &QPushButton::pressed, this, [this] { toggleOn(); });
    const QStringList targets{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
            QStringLiteral("4"), tr("MASTER"), tr("SMP")};
    for (int n = 0; n < 6; ++n) {
        auto* key = new Key(Key::Kind::Target, this);
        key->setObjectName(QStringLiteral("decklab-fx-target-%1").arg(n + 1));
        key->setLabel(targets[n]);
        key->setDeck(n < 4 ? n + 1 : 0);
        key->setAccessibleName(n < 4 ? tr("Lab FX on deck %1").arg(n + 1)
                        : n == 4   ? tr("Lab FX on the master: every deck and sampler")
                                   : tr("Lab FX on the samplers"));
        connect(key, &QPushButton::pressed, this, [this, n] { selectTarget(n + 1); });
        m_targets[n] = key;
    }
    for (int n = 0; n < kEffects; ++n) {
        auto* tile = new Key(Key::Kind::Tile, this);
        tile->setObjectName(QStringLiteral("decklab-fx-effect-%1").arg(n));
        tile->setLabel(effectName(Effect(n)));
        tile->setIcon(n);
        tile->setAccessibleDescription(tr("Tap to choose. Double-tap to latch it on."));
        connect(tile, &QPushButton::pressed, this, [this, n] {
            // Double-tap a tile: choose it and latch it on. The first tap
            // only chooses, so it never sounds.
            const qint64 now = m_host.now ? m_host.now() : 0;
            const bool second = m_lastTile == n && now - m_lastTilePress <= kDoubleTapMs;
            m_lastTile = n;
            m_lastTilePress = second ? -100000 : now;
            selectEffect(Effect(n));
            if (second && !isLatched()) {
                if (m_on) {
                    m_latched = true;
                    changed();
                } else {
                    engage(true);
                }
            }
        });
        m_tiles[n] = tile;
    }
    m_beatLeft = new Key(Key::Kind::Arrow, this);
    m_beatLeft->setObjectName(QStringLiteral("decklab-fx-beat-left"));
    m_beatLeft->setDirection(-1);
    m_beatLeft->setAccessibleName(tr("Beat shorter"));
    connect(m_beatLeft, &QPushButton::pressed, this, [this] { stepBeat(-1); });
    m_beatRight = new Key(Key::Kind::Arrow, this);
    m_beatRight->setObjectName(QStringLiteral("decklab-fx-beat-right"));
    m_beatRight->setDirection(1);
    m_beatRight->setAccessibleName(tr("Beat longer"));
    connect(m_beatRight, &QPushButton::pressed, this, [this] { stepBeat(1); });
    for (int n = 0; n < kBeats; ++n) {
        auto* cell = new Key(Key::Kind::Beat, this);
        cell->setObjectName(QStringLiteral("decklab-fx-beat-%1").arg(n));
        connect(cell, &QPushButton::pressed, this, [this, n] { selectBeat(n); });
        m_beatCells[n] = cell;
    }
    m_pad = new Pad(this);
    m_padAction = new Key(Key::Kind::Action, m_pad);
    m_padAction->setObjectName(QStringLiteral("decklab-fx-pad-action"));
    m_padAction->hide();
    // Changing page happens on release: never under a finger that is down.
    connect(m_padAction, &QPushButton::clicked, this, [this] {
        if (m_host.showPage && m_ready) {
            m_host.showPage(m_target == kSampler ? 2 : 1);
        }
    });
    const QStringList bands{tr("LOW"), tr("MID"), tr("HIGH")};
    for (int n = 0; n < 3; ++n) {
        auto* key = new Key(Key::Kind::Band, this);
        key->setObjectName(QStringLiteral("decklab-fx-band-%1").arg(n));
        key->setLabel(bands[n]);
        connect(key, &QPushButton::pressed, this, [this, n] { setBand(n, !m_bands[n]); });
        m_bandKeys[n] = key;
    }
    m_releaseKey = new Key(Key::Kind::Action, this);
    m_releaseKey->setObjectName(QStringLiteral("decklab-fx-release"));
    m_releaseKey->setLabel(tr("RELEASE"));
    m_releaseKey->setCaption(tr("TAIL RINGS OUT"));
    m_releaseKey->setAccessibleName(tr("Release: back to dry, the tail rings out"));
    connect(m_releaseKey, &QPushButton::pressed, this, [this] { release(); });
    m_echoOutKey = new Key(Key::Kind::Action, this);
    m_echoOutKey->setObjectName(QStringLiteral("decklab-fx-echo-out"));
    m_echoOutKey->setLabel(tr("ECHO OUT"));
    m_echoOutKey->setCaption(tr("TRACK DROPS 1 BAR"));
    m_echoOutKey->setAccessibleName(tr("Echo out: the track drops for one bar under the tail"));
    connect(m_echoOutKey, &QPushButton::pressed, this, [this] { echoOut(); });
    m_cutKey = new Key(Key::Kind::Action, this);
    m_cutKey->setObjectName(QStringLiteral("decklab-fx-cut"));
    m_cutKey->setLabel(tr("CUT"));
    m_cutKey->setCaption(tr("DRY NOW"));
    m_cutKey->setAccessibleName(tr("Cut: dry now, no tail"));
    connect(m_cutKey, &QPushButton::pressed, this, [this] { cut(); });
    m_deckReadout = new DeckReadout(this);
    m_deckReadout->setObjectName(QStringLiteral("decklab-fx-deck"));
    // The Sampler page link for SMP (it replaces the old Samples tab).
    connect(m_deckReadout, &QPushButton::clicked, this, [this] {
        if (m_target == kSampler && m_host.showPage) {
            m_host.showPage(2);
        }
    });
    syncKeys();
}

void DeckLabLabFx::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutChildren();
}

void DeckLabLabFx::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}

void DeckLabLabFx::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kGround);
    painter.setFont(labelFont());
    painter.setPen(usesBands(m_effect) ? kMuted : kDisabledText);
    if (!m_bandCaption.isEmpty()) {
        painter.drawText(m_bandCaption, Qt::AlignLeft | Qt::AlignVCenter, tr("FX BANDS"));
    }
}

// Fixed geometry, computed once per resize: 800x436 below the rail on the
// 7" screen, also 1024x556; r4-waveforms: 800x270 and 1024x330 under the
// compact waveform lanes.
void DeckLabLabFx::layoutChildren() {
    const int w = width(), h = height();
    const int margin = 6, gap = 6;
    const int headerTop = margin, headerHeight = 44;
    const int footerHeight = 48, footerTop = h - margin - footerHeight;
    const int bodyTop = headerTop + headerHeight + gap;
    const int bodyHeight = std::max(0, footerTop - gap - bodyTop);
    // Header: wordmark, readout, ON, then the targets.
    const QSize mark = Wordmark::preferredSize();
    m_wordmark->setGeometry(margin, headerTop + (headerHeight - mark.height()) / 2, mark.width(), mark.height());
    const std::array<int, 6> targetWidths{44, 44, 44, 44, 76, 52};
    int x = w - margin;
    for (int n = 5; n >= 0; --n) {
        x -= targetWidths[n];
        m_targets[n]->setGeometry(x, headerTop, targetWidths[n], headerHeight);
        x -= 4;
    }
    const int onWidth = 96;
    const int onLeft = x - 8 - onWidth;
    m_onKey->setGeometry(onLeft, headerTop, onWidth, headerHeight);
    const int readoutLeft = margin + mark.width() + 12;
    m_readout->setGeometry(readoutLeft, headerTop, std::max(0, onLeft - 10 - readoutLeft), headerHeight);
    // Body: tiles | beat strip over the pad | band lamps.
    const int tilesWidth = 132, bandsWidth = 120;
    const int padLeft = margin + tilesWidth + gap;
    const int bandsLeft = w - margin - bandsWidth;
    const int padWidth = std::max(0, bandsLeft - gap - padLeft);
    const bool oneColumn = bodyHeight >= kEffects * 44 + (kEffects - 1) * 4;
    const int columns = oneColumn ? 1 : 2;
    const int rows = kEffects / columns;
    const int tileWidth = (tilesWidth - (columns - 1) * 4) / columns;
    for (int n = 0; n < kEffects; ++n) {
        const int row = n / columns, column = n % columns;
        const int top = bodyTop + row * bodyHeight / rows;
        const int bottom = bodyTop + (row + 1) * bodyHeight / rows - (row + 1 < rows ? 4 : 0);
        m_tiles[n]->setGeometry(margin + column * (tileWidth + 4), top, tileWidth, bottom - top);
    }
    const int stripHeight = 44, arrowWidth = 44;
    m_beatLeft->setGeometry(padLeft, bodyTop, arrowWidth, stripHeight);
    m_beatRight->setGeometry(padLeft + padWidth - arrowWidth, bodyTop, arrowWidth, stripHeight);
    const int cellsLeft = padLeft + arrowWidth + 4;
    const int cellsWidth = std::max(0, padWidth - 2 * (arrowWidth + 4));
    std::array<int, kBeats + 1> cellEdges{};
    for (int n = 0; n <= kBeats; ++n) {
        cellEdges[n] = cellsLeft + n * (cellsWidth + 4) / kBeats;
    }
    std::array<double, kBeats + 1> padEdges{};
    padEdges[0] = 0;
    padEdges[kBeats] = 1;
    for (int n = 0; n < kBeats; ++n) {
        m_beatCells[n]->setGeometry(cellEdges[n], bodyTop, cellEdges[n + 1] - cellEdges[n] - 4, stripHeight);
        if (n > 0 && padWidth > 0) {
            // Pad columns line up with the cells: the boundary is the
            // middle of the gap between two cells.
            padEdges[n] = (cellEdges[n] - 2 - padLeft) / double(padWidth);
        }
    }
    const int padTop = bodyTop + stripHeight + 4;
    m_pad->setGeometry(padLeft, padTop, padWidth, std::max(0, bodyTop + bodyHeight - padTop));
    m_pad->setColumnEdges(padEdges);
    const QSize action(156, 44);
    m_padAction->setGeometry((m_pad->width() - action.width()) / 2, m_pad->height() / 2 + 6, action.width(), action.height());
    // r4-waveforms: under the waveform lanes (800x270) the caption steps
    // aside so the three lamps keep 44 px; each lamp still says GETS FX/DRY.
    const bool captioned = (bodyHeight - 22) / 3 - 4 >= 44;
    m_bandCaption = captioned ? QRect(bandsLeft + 2, bodyTop, bandsWidth - 2, 20) : QRect();
    const int bandsTop = bodyTop + (captioned ? 22 : 0);
    const int bandsHeight = std::max(0, bodyTop + bodyHeight - bandsTop);
    // HIGH on top, like an EQ.
    for (int row = 0; row < 3; ++row) {
        const int band = 2 - row;
        const int top = bandsTop + row * bandsHeight / 3;
        const int bottom = bandsTop + (row + 1) * bandsHeight / 3 - (row < 2 ? 4 : 0);
        m_bandKeys[band]->setGeometry(bandsLeft, top, bandsWidth, bottom - top);
    }
    // Footer: the three ways out, then the target readout.
    m_releaseKey->setGeometry(margin, footerTop, 150, footerHeight);
    m_echoOutKey->setGeometry(margin + 154, footerTop, 150, footerHeight);
    m_cutKey->setGeometry(margin + 308, footerTop, 100, footerHeight);
    const int readoutX = margin + 418;
    m_deckReadout->setGeometry(readoutX, footerTop, std::max(0, w - margin - readoutX), footerHeight);
    update();
}

// --- state -----------------------------------------------------------------

bool DeckLabLabFx::usesBeat(Effect effect) const {
    return effect == Effect::Echo || effect == Effect::Reverb || effect == Effect::Roll;
}

bool DeckLabLabFx::usesBands(Effect effect) const {
    return effect != Effect::Roll;
}

int DeckLabLabFx::beatIndex() const {
    return m_beat[m_effect == Effect::Roll ? 1 : 0];
}

double DeckLabLabFx::beatValue() const {
    return (m_effect == Effect::Roll ? kRollBeats : kDelayBeats)[beatIndex()];
}

QStringList DeckLabLabFx::beatLabels() const {
    QStringList labels;
    for (double beats : (m_effect == Effect::Roll ? kRollBeats : kDelayBeats)) {
        labels.append(beatText(beats));
    }
    return labels;
}

QWidget* DeckLabLabFx::pad() const {
    return m_pad;
}

int DeckLabLabFx::columnAt(double x) const {
    const auto& edges = m_pad->columnEdges();
    for (int column = 0; column < kBeats; ++column) {
        if (x < edges[column + 1]) {
            return column;
        }
    }
    return kBeats - 1;
}

double DeckLabLabFx::columnCentre(int column) const {
    const auto& edges = m_pad->columnEdges();
    column = std::clamp(column, 0, kBeats - 1);
    return (edges[column] + edges[column + 1]) / 2;
}

QPointF DeckLabLabFx::spot() const {
    const QPointF stored = m_spots[int(m_effect)];
    return usesBeat(m_effect) ? QPointF(columnCentre(beatIndex()), stored.y()) : stored;
}

void DeckLabLabFx::setSpot(QPointF spot) {
    spot = QPointF(std::clamp(spot.x(), 0.0, 1.0), std::clamp(spot.y(), 0.0, 1.0));
    m_spots[int(m_effect)] = spot;
    if (usesBeat(m_effect)) {
        const int column = columnAt(spot.x());
        if (column != beatIndex()) {
            selectBeat(column);
        }
    }
}

double DeckLabLabFx::depth() const {
    // The bottom edge is a light touch, never silence.
    return 0.2 + 0.8 * spot().y();
}

int DeckLabLabFx::unitFor(int target) const {
    // Decks 1 and 3 (and MASTER) use unit 1, decks 2 and 4 (and SMP) unit
    // 2, so switching between the two main decks lets a tail ring out on
    // one while the other plays.
    if (target == kMaster) {
        return 0;
    }
    if (target == kSampler) {
        return 1;
    }
    return (target - 1) % 2;
}

int DeckLabLabFx::referenceDeck(int target) const {
    if (target >= 1 && target <= 4) {
        return target;
    }
    if (!m_host.value) {
        return 0;
    }
    // MASTER and SMP follow the tempo master, else the first playing deck,
    // else the first loaded one.
    for (int deck = 1; deck <= 4; ++deck) {
        if (m_host.value(deckGroup(deck), kLeaderItem) > 0 && m_host.value(deckGroup(deck), kLoadedItem) > 0) {
            return deck;
        }
    }
    for (int deck = 1; deck <= 4; ++deck) {
        if (m_host.value(deckGroup(deck), kPlayItem) > 0 && m_host.value(deckGroup(deck), kLoadedItem) > 0) {
            return deck;
        }
    }
    for (int deck = 1; deck <= 4; ++deck) {
        if (m_host.value(deckGroup(deck), kLoadedItem) > 0) {
            return deck;
        }
    }
    return 0;
}

double DeckLabLabFx::referenceBpm(int target) const {
    const int deck = referenceDeck(target);
    const double bpm = deck && m_host.value ? m_host.value(deckGroup(deck), kBpmItem) : 0;
    return util_isfinite(bpm) && bpm > 0 && bpm < 1000 ? bpm : 0;
}

bool DeckLabLabFx::targetLoaded(int target) const {
    if (!m_host.value) {
        return false;
    }
    if (target >= 1 && target <= 4) {
        return m_host.value(deckGroup(target), kLoadedItem) > 0;
    }
    if (target == kMaster) {
        for (int deck = 1; deck <= 4; ++deck) {
            if (m_host.value(deckGroup(deck), kLoadedItem) > 0) {
                return true;
            }
        }
    }
    for (int slot = 1; slot <= 16; ++slot) {
        if (m_host.value(samplerGroup(slot), kLoadedItem) > 0) {
            return true;
        }
    }
    return false;
}

bool DeckLabLabFx::available() const {
    return m_available;
}

DeckLabLabFx::Lamp DeckLabLabFx::lamp() const {
    if (m_on) {
        return Lamp::On;
    }
    for (const Unit& unit : m_units) {
        if (unit.releasing) {
            return Lamp::Tail;
        }
    }
    return Lamp::Dry;
}

bool DeckLabLabFx::isWet() const {
    return m_on || m_units[0].enabled || m_units[1].enabled || !m_rollGroups.isEmpty();
}

bool DeckLabLabFx::tabLit() const {
    const Lamp state = lamp();
    return state == Lamp::On || (state == Lamp::Tail && m_tailBlink);
}

QString DeckLabLabFx::readoutTitle() const {
    QString title = effectName(m_effect);
    if (usesBeat(m_effect)) {
        title += tr(" · %1 BEAT").arg(beatText(beatValue()));
    }
    return title;
}

QString DeckLabLabFx::readoutSubtitle() const {
    const QString where = m_target == kMaster ? tr("MASTER")
            : m_target == kSampler            ? tr("SAMPLERS")
                                              : tr("DECK %1").arg(m_target);
    if (m_target <= 4 && !targetLoaded(m_target)) {
        return tr("%1 · EMPTY").arg(where);
    }
    const double bpm = referenceBpm(m_target);
    return bpm > 0 ? tr("%1 · %2 BPM").arg(where, QString::number(bpm, 'f', 1)) : tr("%1 · NO BPM").arg(where);
}

QString DeckLabLabFx::padHint() const {
    if (isLatched()) {
        return tr("LATCHED · TOUCH TO MOVE IT\nON OR RELEASE TO STOP");
    }
    if (m_effect == Effect::Roll && rollGroups().isEmpty()) {
        return m_target <= 4 ? tr("PLAY DECK %1 TO ROLL").arg(m_target) : tr("PLAY A TRACK TO ROLL");
    }
    if (usesBands(m_effect) && !m_bands[0] && !m_bands[1] && !m_bands[2]) {
        return tr("LIGHT LOW, MID OR HIGH\nTO HEAR IT");
    }
    QString lift;
    switch (m_effect) {
    case Effect::Echo:
    case Effect::Reverb:
        lift = tr("LIFT: THE TAIL RINGS OUT");
        break;
    case Effect::Roll:
        lift = tr("LIFT: THE TRACK PLAYS ON IN TIME");
        break;
    default:
        lift = tr("LIFT: BACK TO DRY");
        break;
    }
    return tr("HOLD TO PLAY · DOUBLE-TAP TO LATCH") + QLatin1Char('\n') + lift;
}

// --- controls --------------------------------------------------------------

void DeckLabLabFx::selectEffect(Effect effect) {
    if (effect == m_effect) {
        changed();
        return;
    }
    const Effect previous = m_effect;
    m_effect = effect;
    if (m_on) {
        // The running effect changes live, as an effect select does on a
        // mixer: it stays latched (or held) on the same target.
        if (previous == Effect::Roll) {
            stopRolls();
        }
        m_voiceEffect = effect;
        applyVoice();
        if (effect == Effect::Roll) {
            startRolls(beatValue());
        }
    }
    changed();
}

void DeckLabLabFx::selectTarget(int target) {
    if (target < 1 || target > 6) {
        return;
    }
    if (target == m_target) {
        changed();
        return;
    }
    if (m_on) {
        // The old target rings out instead of cutting (PF-X02); the new
        // one starts dry.
        stopRolls();
        m_on = m_latched = m_held = false;
        m_pad->resetTouch();
        beginRelease(m_voiceUnit, false);
    }
    m_target = target;
    refresh();
}

void DeckLabLabFx::selectBeat(int index) {
    if (!usesBeat(m_effect)) {
        return;
    }
    index = std::clamp(index, 0, kBeats - 1);
    int& beat = m_beat[m_effect == Effect::Roll ? 1 : 0];
    if (index == beat) {
        changed();
        return;
    }
    beat = index;
    if (m_on && m_voiceEffect == m_effect) {
        if (m_effect == Effect::Roll) {
            startRolls(beatValue());
        } else {
            writeParameter(m_voiceUnit, 12, (beatValue() - .0625) / .9375);
        }
    }
    changed();
}

void DeckLabLabFx::stepBeat(int delta) {
    if (usesBeat(m_effect)) {
        selectBeat(beatIndex() + delta);
    }
}

void DeckLabLabFx::setBand(int band, bool on) {
    if (band < 0 || band > 2 || m_bands[band] == on) {
        return;
    }
    m_bands[band] = on;
    if (m_on && usesBands(m_voiceEffect)) {
        applyVoice();
    }
    changed();
}

void DeckLabLabFx::toggleOn() {
    if (m_on && m_held && !m_latched) {
        m_latched = true;
        changed();
        return;
    }
    if (m_on) {
        m_held = false;
        m_pad->resetTouch();
        lift();
        return;
    }
    engage(true);
}

void DeckLabLabFx::release() {
    if (!m_on) {
        return;
    }
    stopRolls();
    m_on = m_latched = m_held = false;
    m_pad->resetTouch();
    beginRelease(m_voiceUnit, false);
    changed();
}

void DeckLabLabFx::echoOut() {
    if (m_on) {
        stopRolls();
        m_on = m_latched = m_held = false;
        m_pad->resetTouch();
        beginRelease(m_voiceUnit, true);
    } else {
        for (int unit = 0; unit < 2; ++unit) {
            if (m_units[unit].releasing && !m_units[unit].mute) {
                beginRelease(unit, true);
            }
        }
    }
    changed();
}

void DeckLabLabFx::cut() {
    stopRolls();
    m_on = m_latched = m_held = false;
    if (m_pad) {
        m_pad->resetTouch();
    }
    for (int unit = 0; unit < 2; ++unit) {
        // Like the old hard release: every loaded unit goes dry and
        // unrouted, whatever this page thinks it did.
        if (m_units[unit].enabled || (m_host.unitReady && m_host.unitReady(unit + 1))) {
            stopUnit(unit);
        }
    }
    changed();
}

void DeckLabLabFx::endHolds() {
    if (!m_held) {
        return;
    }
    m_held = false;
    m_pad->resetTouch();
    if (m_on && !m_latched) {
        lift();
    } else {
        changed();
    }
}

void DeckLabLabFx::noteTouch(QObject* watched, QEvent* event) {
    if (!m_held || !m_pad || !watched) {
        return;
    }
    const QEvent::Type type = event->type();
    const bool mouse = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease;
    if (mouse && static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) {
        return;
    }
    if (watched->isWidgetType()) {
        // Only one contact drives the pointer: a press or release reaching
        // another control means the holding contact has ended.
        auto* widget = static_cast<QWidget*>(watched);
        if (mouse && widget != m_pad && !m_pad->isAncestorOf(widget)) {
            endHolds();
        }
    } else if (watched->isWindowType() && !m_pad->isVisible() && (mouse || type == QEvent::TouchEnd)) {
        // The page changed under a held pad (controller page switch): the
        // finger's release reaches the window, maybe no widget at all.
        endHolds();
    }
}

void DeckLabLabFx::refresh() {
    const int unit = unitFor(m_target);
    m_ready = m_host.unitReady && m_host.unitReady(unit + 1);
    m_available = m_ready && targetLoaded(m_target);
    // A target that lost its source (track ejected) goes dry at once.
    if (m_on && (!(m_host.unitReady && m_host.unitReady(m_voiceUnit + 1)) || !targetLoaded(m_voiceTarget))) {
        cut();
        return;
    }
    changed();
}

void DeckLabLabFx::padPressed(QPointF position) {
    if (!m_available) {
        return;
    }
    const qint64 now = m_host.now ? m_host.now() : 0;
    const QPointF pixels((position.x() - m_lastPadPoint.x()) * m_pad->width(),
            (position.y() - m_lastPadPoint.y()) * m_pad->height());
    const bool doubleTap = now - m_lastPadPress <= kDoubleTapMs && now - m_lastPadRelease <= kDoubleTapMs &&
            std::hypot(pixels.x(), pixels.y()) <= 48;
    m_lastPadPress = doubleTap ? -100000 : now;
    m_lastPadPoint = position;
    m_finger = position;
    m_held = true;
    if (m_on) {
        // A latched effect follows the finger and stays latched.
        setSpot(position);
        if (doubleTap) {
            m_latched = true;
        }
        applyVoice();
        changed();
        return;
    }
    setSpot(position);
    engage(doubleTap);
    if (!m_on) {
        m_held = false;
        // round4: a refused touch (a paused deck has nothing to roll) did
        // nothing, so it is not the first tap of a latching double-tap.
        m_lastPadPress = -100000;
        changed();
    }
}

void DeckLabLabFx::padMoved(QPointF position) {
    if (!m_held) {
        return;
    }
    m_finger = position;
    setSpot(position);
    if (m_on) {
        applyVoice();
    }
    m_pad->update();
}

void DeckLabLabFx::padReleased() {
    m_lastPadRelease = m_host.now ? m_host.now() : 0;
    if (!m_held) {
        return;
    }
    m_held = false;
    if (m_on && !m_latched) {
        lift();
    } else {
        changed();
    }
}

// --- engine ----------------------------------------------------------------

void DeckLabLabFx::engage(bool latch) {
    refresh();
    if (!m_available || (m_effect == Effect::Roll && rollGroups().isEmpty())) {
        return;
    }
    const int unit = unitFor(m_target);
    Unit& state = m_units[unit];
    // A tail ringing on this unit ends now; the new voice takes over.
    if (state.releasing) {
        cancelRelease(unit);
    }
    m_voiceUnit = unit;
    m_voiceTarget = m_target;
    m_voiceEffect = m_effect;
    m_on = true;
    m_latched = latch;
    // Every touch-down rewrites the whole parameter set once.
    state.written.fill(-1000);
    // Arm before routing and enabling: a freeze right after the first touch
    // must not leave an unprotected effect running (the DSP's 1.5 s
    // deadman needs a non-zero heartbeat).
    state.heartbeat = state.heartbeat % 65535 + 1;
    writeParameter(unit, 15, state.heartbeat / 65535.0, true);
    route(unit, m_target);
    applyVoice();
    m_host.set(m_slotGroups[unit], kEnabledItem, 1);
    m_host.setParameter(m_unitGroups[unit], kMixItem, 1);
    state.enabled = true;
    m_lastTick = m_host.now ? m_host.now() : 0;
    if (m_effect == Effect::Roll) {
        startRolls(beatValue());
    }
    changed();
}

void DeckLabLabFx::lift() {
    if (!m_on) {
        return;
    }
    m_on = m_latched = m_held = false;
    // The musical return depends on the effect.
    switch (m_voiceEffect) {
    case Effect::Echo:
    case Effect::Reverb:
        beginRelease(m_voiceUnit, false); // the tail rings out
        break;
    case Effect::Roll:
        stopRolls(); // slip: the track plays on where it would have been
        stopUnit(m_voiceUnit);
        break;
    default:
        stopUnit(m_voiceUnit); // straight back to dry
        break;
    }
    changed();
}

void DeckLabLabFx::applyVoice() {
    if (!m_on) {
        return;
    }
    const int unit = m_voiceUnit;
    const QPointF at = spot();
    const double amount = depth();
    int leverMode = 0, isoMode = 0;
    std::array<double, 3> lever{}, iso{};
    switch (m_voiceEffect) {
    case Effect::Echo:
    case Effect::Reverb:
        // The band sends into the beat-synced delay; the dry stays whole.
        leverMode = m_voiceEffect == Effect::Echo ? 1 : 2;
        for (int band = 0; band < 3; ++band) {
            lever[band] = m_bands[band] ? 1 : 0;
        }
        break;
    case Effect::Filter:
    case Effect::Duck:
    case Effect::Drive:
        isoMode = m_voiceEffect == Effect::Filter ? 4 : m_voiceEffect == Effect::Duck ? 5 : 3;
        for (int band = 0; band < 3; ++band) {
            iso[band] = m_bands[band] ? at.x() * 2 - 1 : 0;
        }
        break;
    case Effect::Roll:
        // The slot runs dry under a roll, so RELEASE and ECHO OUT have the
        // roll in their tail.
        break;
    }
    for (int band = 0; band < 3; ++band) {
        writeParameter(unit, band + 1, lever[band]);
        writeParameter(unit, band + 6, (iso[band] + 1) / 2);
    }
    writeParameter(unit, 4, leverMode / 3.0);
    writeParameter(unit, 9, isoMode / 5.0);
    writeParameter(unit, 5, amount);
    writeParameter(unit, 10, amount);
    const double bpm = referenceBpm(m_voiceTarget);
    writeParameter(unit, 11, (std::clamp(bpm > 0 ? bpm : 120.0, 40.0, 240.0) - 40) / 200);
    if (m_voiceEffect != Effect::Roll) {
        writeParameter(unit, 12, (kDelayBeats[m_beat[0]] - .0625) / .9375);
    }
    // The beat phase from the first touch, so DUCK pumps on the beat at once.
    const int deck = referenceDeck(m_voiceTarget);
    const double phase = deck && m_host.value ? m_host.value(deckGroup(deck), kPhaseItem) : -1;
    writeParameter(unit, 14, util_isfinite(phase) && phase >= 0 && phase <= 1 ? (phase + 1) / 2 : 0);
    writeParameter(unit, 13, 0);
}

void DeckLabLabFx::route(int unit, int target) {
    for (int deck = 1; deck <= 4; ++deck) {
        m_host.set(m_unitGroups[unit], m_deckRoutes[deck - 1], target == deck || target == kMaster ? 1 : 0);
    }
    for (int slot = 1; slot <= 16; ++slot) {
        m_host.set(m_unitGroups[unit], m_samplerRoutes[slot - 1], target == kSampler || target == kMaster ? 1 : 0);
    }
    m_units[unit].target = target;
}

void DeckLabLabFx::writeParameter(int unit, int parameter, double normalized, bool force) {
    Unit& state = m_units[unit];
    if (!force && state.written[parameter] == normalized) {
        return;
    }
    state.written[parameter] = normalized;
    if (m_host.setParameter) {
        m_host.setParameter(m_slotGroups[unit], m_parameterNames[parameter], normalized);
    }
}

void DeckLabLabFx::beginRelease(int unit, bool mute) {
    Unit& state = m_units[unit];
    if (!state.enabled) {
        return;
    }
    for (int band = 0; band < 3; ++band) {
        writeParameter(unit, band + 1, 0);
        writeParameter(unit, band + 6, .5);
    }
    writeParameter(unit, 4, 0);
    writeParameter(unit, 9, 0);
    // Release type 1: dry at once, the tail rings out. 2: the track drops
    // under the tail, then (tick) 3: it fades back in over a beat.
    writeParameter(unit, 13, mute ? 2.0 / 3 : 1.0 / 3, true);
    state.releasing = true;
    state.mute = mute;
    state.returning = false;
    const double bpm = referenceBpm(state.target);
    const qint64 now = m_host.now ? m_host.now() : 0;
    state.releaseAt = now + qint64(std::clamp(240000.0 / (bpm > 0 ? bpm : 120), 2000.0, 6000.0));
}

void DeckLabLabFx::cancelRelease(int unit) {
    Unit& state = m_units[unit];
    writeParameter(unit, 13, 0, true);
    state.releasing = state.mute = state.returning = false;
}

void DeckLabLabFx::stopUnit(int unit) {
    Unit& state = m_units[unit];
    m_host.set(m_slotGroups[unit], kEnabledItem, 0);
    m_host.setParameter(m_unitGroups[unit], kMixItem, 0);
    for (int deck = 1; deck <= 4; ++deck) {
        m_host.set(m_unitGroups[unit], m_deckRoutes[deck - 1], 0);
    }
    for (int slot = 1; slot <= 16; ++slot) {
        m_host.set(m_unitGroups[unit], m_samplerRoutes[slot - 1], 0);
    }
    state.enabled = state.releasing = state.mute = state.returning = false;
}

QStringList DeckLabLabFx::rollGroups() const {
    QStringList groups;
    if (!m_host.value) {
        return groups;
    }
    if (m_target >= 1 && m_target <= 4) {
        if (m_host.value(deckGroup(m_target), kPlayItem) > 0) {
            groups.append(deckGroup(m_target));
        }
        return groups;
    }
    if (m_target == kMaster) {
        for (int deck = 1; deck <= 4; ++deck) {
            if (m_host.value(deckGroup(deck), kPlayItem) > 0) {
                groups.append(deckGroup(deck));
            }
        }
    }
    // Samplers roll on their own grid, so only analysed ones.
    for (int slot = 1; slot <= 16; ++slot) {
        const QString group = samplerGroup(slot);
        if (m_host.value(group, kPlayItem) > 0 && m_host.value(group, kLocalBpmItem) > 0) {
            groups.append(group);
        }
    }
    return groups;
}

void DeckLabLabFx::startRolls(double size) {
    static const std::array<QString, kBeats> items = [] {
        std::array<QString, kBeats> names;
        for (int n = 0; n < kBeats; ++n) {
            names[n] = QStringLiteral("beatlooproll_%1_activate").arg(kRollBeats[n]);
        }
        return names;
    }();
    QString item;
    for (int n = 0; n < kBeats; ++n) {
        if (std::abs(kRollBeats[n] - size) < 1e-9) {
            item = items[n];
        }
    }
    if (item.isEmpty() || !m_host.set) {
        return;
    }
    // Same groups, new size: press the new size first, then let go of the
    // old one, so Mixxx's roll stack keeps rolling without a gap.
    const QStringList groups = m_rollGroups.isEmpty() ? rollGroups() : m_rollGroups;
    for (const QString& group : groups) {
        m_host.set(group, item, 1);
    }
    if (!m_rollItem.isEmpty() && m_rollItem != item) {
        for (const QString& group : std::as_const(m_rollGroups)) {
            m_host.set(group, m_rollItem, 0);
        }
    }
    m_rollGroups = groups;
    m_rollItem = groups.isEmpty() ? QString() : item;
}

void DeckLabLabFx::stopRolls() {
    if (m_host.set) {
        for (const QString& group : std::as_const(m_rollGroups)) {
            m_host.set(group, m_rollItem, 0);
        }
    }
    m_rollGroups.clear();
    m_rollItem.clear();
}

void DeckLabLabFx::tick() {
    const qint64 now = m_host.now ? m_host.now() : 0;
    // A GUI stall longer than the DSP's deadman (1.5 s) has already
    // returned the effect to dry; show that rather than resume.
    if (m_lastTick >= 0 && now - m_lastTick > 1500 && isWet()) {
        m_lastTick = now;
        cut();
        return;
    }
    m_lastTick = now;
    bool blink = m_tailBlink;
    for (int unit = 0; unit < 2; ++unit) {
        Unit& state = m_units[unit];
        if (!state.enabled) {
            continue;
        }
        // Heartbeat, then tempo and beat phase: a latched delay follows
        // pitch and Sync without another touch.
        state.heartbeat = state.heartbeat % 65535 + 1;
        writeParameter(unit, 15, state.heartbeat / 65535.0, true);
        const int deck = referenceDeck(state.target);
        const bool known = deck && m_host.value;
        const double rawBpm = known ? m_host.value(deckGroup(deck), kBpmItem) : 0;
        const double bpm = util_isfinite(rawBpm) && rawBpm > 0 ? std::clamp(rawBpm, 40.0, 240.0) : 120;
        writeParameter(unit, 11, (bpm - 40) / 200);
        const double phase = known ? m_host.value(deckGroup(deck), kPhaseItem) : -1;
        // util_isfinite: the build uses -ffast-math.
        const bool phaseKnown = util_isfinite(phase) && phase >= 0 && phase <= 1;
        writeParameter(unit, 14, phaseKnown ? (phase + 1) / 2 : 0);
        if (state.releasing) {
            blink = phaseKnown ? phase < 0.5 : (now / 250) % 2 == 0;
            if (now >= state.releaseAt) {
                if (state.mute && !state.returning) {
                    writeParameter(unit, 13, 1, true);
                    state.returning = true;
                    state.releaseAt = now + qint64(60000 / bpm) + 100;
                } else {
                    stopUnit(unit); // the tail has rung out
                    changed();
                }
            }
        }
    }
    if (blink != m_tailBlink) {
        m_tailBlink = blink;
        m_onKey->setOutlineBright(blink);
    }
    if (isVisible()) {
        // A sample or track loaded from a controller while the page is open.
        if (m_ready && targetLoaded(m_target) != m_available) {
            refresh();
        }
        refreshReadouts();
    }
}

// --- presentation ----------------------------------------------------------

void DeckLabLabFx::changed() {
    syncKeys();
    if (m_pad) {
        m_pad->update();
    }
    emit stateChanged();
}

void DeckLabLabFx::syncKeys() {
    if (!m_onKey) {
        return;
    }
    const Lamp state = lamp();
    m_onKey->setActive(m_available || m_on || state == Lamp::Tail);
    m_onKey->setLit(state == Lamp::On);
    m_onKey->setLabel(state == Lamp::Tail ? tr("TAIL") : tr("ON"));
    m_onKey->setTail(state == Lamp::Tail);
    m_onKey->setCaption(state == Lamp::On ? (m_latched ? tr("LATCHED") : tr("HELD")) : QString());
    m_onKey->setOutlineBright(m_tailBlink || state != Lamp::Tail);
    setAccessibleNameIf(m_onKey, state == Lamp::On ? (m_latched ? tr("Lab FX on, latched: tap to stop")
                                                              : tr("Lab FX on while held"))
                    : state == Lamp::Tail ? tr("Lab FX tail ringing out: tap to play again")
                                          : tr("Lab FX dry: tap to latch it on"));
    for (int n = 0; n < kEffects; ++n) {
        m_tiles[n]->setSelected(int(m_effect) == n);
        m_tiles[n]->setLit(m_on && int(m_voiceEffect) == n);
        setAccessibleNameIf(m_tiles[n], effectName(Effect(n)) + (int(m_effect) == n ? tr(", selected") : QString()));
    }
    const bool beat = usesBeat(m_effect);
    const QStringList labels = beatLabels();
    for (int n = 0; n < kBeats; ++n) {
        m_beatCells[n]->setLabel(labels[n]);
        m_beatCells[n]->setActive(beat);
        m_beatCells[n]->setSelected(beat && n == beatIndex());
        setAccessibleNameIf(m_beatCells[n], tr("%1 beat").arg(labels[n]));
    }
    m_beatLeft->setActive(beat && beatIndex() > 0);
    m_beatRight->setActive(beat && beatIndex() < kBeats - 1);
    for (int n = 0; n < 6; ++n) {
        m_targets[n]->setSelected(m_target == n + 1);
        m_targets[n]->setDim(!targetLoaded(n + 1));
    }
    const bool bands = usesBands(m_effect);
    for (int n = 0; n < 3; ++n) {
        m_bandKeys[n]->setActive(bands);
        m_bandKeys[n]->setSelected(m_bands[n]);
        m_bandKeys[n]->setLit(bands && m_bands[n] && m_on && usesBands(m_voiceEffect));
        m_bandKeys[n]->setCaption(!bands ? tr("ALL BANDS") : m_bands[n] ? tr("GETS FX") : tr("DRY"));
        setAccessibleNameIf(m_bandKeys[n], tr("%1 band %2").arg(m_bandKeys[n]->label(),
                m_bands[n] ? tr("gets the effect") : tr("stays dry")));
    }
    bool tail = false, muteTail = true;
    for (const Unit& unit : m_units) {
        if (unit.releasing) {
            tail = true;
            muteTail = muteTail && unit.mute;
        }
    }
    m_releaseKey->setActive(m_on);
    m_echoOutKey->setActive(m_on || (tail && !muteTail));
    m_cutKey->setActive(isWet());
    // Empty state: the way to a track (Browse) or to samples (Sampler).
    const bool action = !m_available && m_ready;
    m_padAction->setLabel(m_target == kSampler ? tr("SAMPLER ›") : tr("BROWSE"));
    setAccessibleNameIf(m_padAction, m_target == kSampler ? tr("Open the Sampler page") : tr("Open Browse to load a track"));
    if (m_padAction->isVisibleTo(m_pad) != action) {
        m_padAction->setVisible(action);
    }
    refreshReadouts();
    update(m_bandCaption);
}

void DeckLabLabFx::setLanesShown(bool shown) {
    if (shown == m_lanesShown) {
        return;
    }
    m_lanesShown = shown;
    refreshReadouts();
}

void DeckLabLabFx::refreshReadouts() {
    if (!m_readout) {
        return;
    }
    const double bpm = referenceBpm(m_target);
    const std::array<int, 5> header{int(m_effect),
            beatIndex(),
            m_target,
            m_target <= 4 && !targetLoaded(m_target) ? -1 : 1,
            int(std::lround(bpm * 10))};
    if (header != m_header) {
        m_header = header;
        m_readout->setLines(readoutTitle(), readoutSubtitle());
    }
    DeckReadout::State state;
    state.compact = m_lanesShown; // r4-waveforms
    if (m_target == kSampler) {
        state.sampler = true;
        if (m_host.value && isVisible()) {
            for (int slot = 1; slot <= 16; ++slot) {
                if (m_host.value(samplerGroup(slot), kLoadedItem) > 0) {
                    ++state.samplersLoaded;
                    if (m_host.value(samplerGroup(slot), kPlayItem) > 0) {
                        ++state.samplersPlaying;
                    }
                }
            }
        }
    } else {
        state.master = m_target == kMaster;
        state.deck = referenceDeck(m_target);
        // The readout ticks only while the page is shown.
        if (state.deck && m_host.deck && isVisible()) {
            state.info = m_host.deck(state.deck);
            if (state.compact) {
                // Not shown here: never a repaint for a clock second.
                state.info.remaining.clear();
                state.info.phase = -1;
                state.info.endWarning = false;
            }
        }
    }
    const DeckReadout::State& shown = m_deckReadout->state();
    if (shown.sampler != state.sampler || shown.deck != state.deck || m_deckReadout->accessibleName().isEmpty()) {
        setAccessibleNameIf(m_deckReadout, state.sampler ? tr("Samplers: open the Sampler page")
                        : state.deck                   ? tr("Tempo from deck %1").arg(state.deck)
                                                       : tr("No deck loaded"));
    }
    m_deckReadout->setState(state);
}
