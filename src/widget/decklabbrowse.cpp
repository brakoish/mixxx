// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabbrowse.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextLayout>
#include <algorithm>
#include <cmath>

#include "library/coverart.h"
#include "library/coverartcache.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "util/color/rgbcolor.h"

namespace decklab {

namespace colors {
const QColor ink(0xf2, 0xf2, 0xf2);
const QColor muted(0xa8, 0xa8, 0xa8);
const QColor dim(0x70, 0x70, 0x70);
const QColor panel(0x17, 0x17, 0x17);
const QColor edge(0x2e, 0x2e, 0x2e);
const QColor accent(0xd7, 0x44, 0x48);
const QColor keyMatch(0x3e, 0xcf, 0x8e);
const QColor tempoMatch(0x5a, 0xb0, 0xff);
const QColor onAir(0xff, 0x5a, 0x5f);
} // namespace colors

namespace {
constexpr double kPi = 3.14159265358979323846;

QString translate(const char* text) {
    return QCoreApplication::translate("DeckLabBrowse", text);
}

MatchReference& mutableReference() {
    static MatchReference reference;
    return reference;
}

QFont pixelFont(const QFont& base, int pixels, bool bold = false) {
    QFont font = base;
    font.setPixelSize(pixels);
    font.setBold(bold);
    return font;
}

// Wraps text into at most maxLines lines inside box, eliding the last line.
// Returns the y coordinate below the last painted line.
int drawWrapped(QPainter& painter,
        const QRect& box,
        const QString& text,
        const QFont& font,
        const QColor& color,
        int maxLines) {
    if (text.isEmpty() || box.width() <= 0 || box.height() <= 0 || maxLines <= 0) {
        return box.top();
    }
    const QFontMetrics metrics(font);
    const int lineHeight = metrics.height();
    maxLines = std::min(maxLines, std::max(1, box.height() / std::max(1, lineHeight)));
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QList<QPair<int, int>> spans;
    bool truncated = false;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(box.width());
        if (spans.size() == maxLines) {
            truncated = true;
            break;
        }
        spans.append({line.textStart(), line.textLength()});
    }
    layout.endLayout();
    painter.setFont(font);
    painter.setPen(color);
    int y = box.top();
    for (int n = 0; n < spans.size(); ++n) {
        QString line = n + 1 == spans.size() && truncated
                ? text.mid(spans[n].first).simplified()
                : text.mid(spans[n].first, spans[n].second).trimmed();
        line = metrics.elidedText(line, Qt::ElideRight, box.width());
        painter.drawText(QRect(box.left(), y, box.width(), lineHeight),
                Qt::AlignLeft | Qt::AlignVCenter,
                line);
        y += lineHeight;
    }
    return y;
}

void drawStar(QPainter& painter, const QPointF& center, qreal radius, bool filled, const QColor& color) {
    QPolygonF star;
    for (int n = 0; n < 10; ++n) {
        const qreal r = n % 2 ? radius * 0.45 : radius;
        const qreal angle = kPi / 2 + n * kPi / 5;
        star << QPointF(center.x() + r * std::cos(angle), center.y() - r * std::sin(angle));
    }
    painter.setPen(QPen(color, 1));
    painter.setBrush(filled ? QBrush(color) : Qt::NoBrush);
    painter.drawPolygon(star);
}

QString durationText(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return QStringLiteral("—");
    }
    const int total = static_cast<int>(std::round(seconds));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString bpmText(double bpm) {
    if (!std::isfinite(bpm) || bpm <= 0) {
        return QStringLiteral("—");
    }
    return QLocale().toString(bpm, 'f', 1);
}

// A rounded value chip in the details pane: big value, small caption.
void paintChip(QPainter& painter,
        const QRect& box,
        const QString& value,
        const QString& caption,
        const QColor& tone,
        const QFont& base) {
    const QRectF frame = QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor fill(0x22, 0x22, 0x22);
    if (tone.isValid()) {
        fill = tone;
        fill.setAlpha(46);
    }
    painter.setPen(QPen(tone.isValid() ? tone : colors::edge, tone.isValid() ? 1.5 : 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(frame, 3, 3);
    const QFont valueFont = pixelFont(base, box.height() >= 38 ? 16 : 14, true);
    const QFont captionFont = pixelFont(base, 9, true);
    const int captionHeight = QFontMetrics(captionFont).height();
    const QRect valueBox = box.adjusted(3, 2, -3, -captionHeight);
    painter.setFont(valueFont);
    painter.setPen(tone.isValid() ? tone.lighter(125) : colors::ink);
    painter.drawText(valueBox,
            Qt::AlignCenter,
            QFontMetrics(valueFont).elidedText(value, Qt::ElideRight, valueBox.width()));
    painter.setFont(captionFont);
    painter.setPen(tone.isValid() ? tone : colors::dim);
    painter.drawText(box.adjusted(2, 0, -2, -3), Qt::AlignHCenter | Qt::AlignBottom, caption);
}
} // namespace

LancelotPosition lancelot(ChromaticKey key) {
    const int value = static_cast<int>(key);
    if (value < 1 || value > 24) {
        return {};
    }
    const bool major = value <= 12;
    const int tonic = major ? value - 1 : value - 13;
    // Relative major tonic for minor keys (A minor -> C major).
    const int majorTonic = major ? tonic : (tonic + 3) % 12;
    // C major is 8B; each fifth (7 semitones) moves one step clockwise.
    return {(7 + 7 * majorTonic) % 12 + 1, major};
}

QString lancelotText(ChromaticKey key) {
    const LancelotPosition position = lancelot(key);
    if (!position.isValid()) {
        return {};
    }
    return QString::number(position.number) + (position.major ? QLatin1Char('B') : QLatin1Char('A'));
}

HarmonicMatch harmonicMatch(ChromaticKey track, ChromaticKey reference) {
    const LancelotPosition a = lancelot(track);
    const LancelotPosition b = lancelot(reference);
    if (!a.isValid() || !b.isValid()) {
        return HarmonicMatch::None;
    }
    if (a.number == b.number) {
        return a.major == b.major ? HarmonicMatch::Same : HarmonicMatch::Relative;
    }
    const int distance = (a.number - b.number + 12) % 12;
    if (a.major == b.major && (distance == 1 || distance == 11)) {
        return HarmonicMatch::Adjacent;
    }
    return HarmonicMatch::None;
}

bool tempoMatches(double trackBpm, double referenceBpm, double tolerance) {
    if (!std::isfinite(trackBpm) || !std::isfinite(referenceBpm) ||
            trackBpm <= 0 || referenceBpm <= 0) {
        return false;
    }
    return std::abs(trackBpm / referenceBpm - 1.0) <= tolerance + 1e-9;
}

double tempoDifferencePercent(double trackBpm, double referenceBpm) {
    if (!std::isfinite(trackBpm) || !std::isfinite(referenceBpm) ||
            trackBpm <= 0 || referenceBpm <= 0) {
        return std::nan("");
    }
    return (trackBpm / referenceBpm - 1.0) * 100.0;
}

ChromaticKey keyFromText(const QString& text) {
    static QHash<QString, ChromaticKey> cache;
    const auto found = cache.constFind(text);
    if (found != cache.constEnd()) {
        return found.value();
    }
    if (cache.size() > 512) {
        cache.clear();
    }
    ChromaticKey key = KeyUtils::guessKeyFromText(text);
    if (key == mixxx::track::io::key::INVALID) {
        // Combined notations, for example "8A (Am)" or "1m (Am)".
        const QString first = text.section(QLatin1Char(' '), 0, 0).trimmed();
        if (!first.isEmpty() && first != text) {
            key = KeyUtils::guessKeyFromText(first);
        }
    }
    cache.insert(text, key);
    return key;
}

QColor keyWheelColor(ChromaticKey key) {
    const LancelotPosition position = lancelot(key);
    if (!position.isValid()) {
        return {};
    }
    // Wheel positions run round the hue circle, starting at yellow-green (1).
    const float hue = static_cast<float>(std::fmod(0.21 + (position.number - 1) / 12.0, 1.0));
    return QColor::fromHsvF(hue, position.major ? 0.55f : 0.70f, position.major ? 0.95f : 0.85f);
}

bool MatchReference::sameAs(const MatchReference& other) const {
    return enabled == other.enabled && deck == other.deck && key == other.key &&
            std::abs(bpm - other.bpm) < 0.005;
}

const MatchReference& matchReference() {
    return mutableReference();
}

void setMatchReference(const MatchReference& reference) {
    mutableReference() = reference;
}

QString ellipsis() {
    return QString(QChar(0x2026));
}

QStringList elideBreadcrumb(const QStringList& segments,
        int width,
        const std::function<int(const QString&)>& measure,
        const QString& separator) {
    if (segments.size() <= 1 || measure(segments.join(separator)) <= width) {
        return segments;
    }
    for (int keep = segments.size() - 2; keep >= 1; --keep) {
        QStringList candidate{segments.first(), ellipsis()};
        candidate += segments.mid(segments.size() - keep);
        if (measure(candidate.join(separator)) <= width) {
            return candidate;
        }
    }
    const QStringList tail{ellipsis(), segments.last()};
    if (measure(tail.join(separator)) <= width) {
        return tail;
    }
    return {segments.last()};
}

QString categoryName(Category category) {
    switch (category) {
    case Category::AllTracks:
        return translate("All Tracks");
    case Category::Playlists:
        return translate("Playlists");
    case Category::Crates:
        return translate("Crates");
    case Category::History:
        return translate("History");
    case Category::Artist:
        return translate("Artist");
    case Category::Album:
        return translate("Album");
    case Category::Bpm:
        return translate("BPM");
    case Category::Key:
        return translate("Key");
    }
    return {};
}

QString exactTextQuery(const QString& field, const QString& value) {
    if (value.isEmpty()) {
        return field + QStringLiteral(":\"\"");
    }
    if (value.contains(QLatin1Char('"'))) {
        // The native parser cannot escape quotes. Match the longest quote-free part.
        QString part;
        for (const QString& piece : value.split(QLatin1Char('"'))) {
            if (piece.trimmed().size() > part.size()) {
                part = piece.trimmed();
            }
        }
        return field + QStringLiteral(":\"") + part + QLatin1Char('"');
    }
    return field + QStringLiteral(":=\"") + value + QLatin1Char('"');
}

QString bpmBucketQuery(int bpm) {
    if (bpm <= 0) {
        return QStringLiteral("bpm:\"\"");
    }
    return QStringLiteral("bpm:%1-%2").arg(QString::number(bpm - 0.5), QString::number(bpm + 0.49));
}

QVector<CategoryValue> collectCategoryValues(
        const QAbstractItemModel& model, int column, Category category) {
    QVector<CategoryValue> values;
    if (column < 0 || column >= model.columnCount()) {
        return values;
    }
    const int rows = model.rowCount();
    switch (category) {
    case Category::Artist:
    case Category::Album: {
        QHash<QString, int> counts;
        QHash<QString, QString> labels;
        for (int row = 0; row < rows; ++row) {
            const QString text = model.index(row, column).data().toString().trimmed();
            const QString folded = text.toCaseFolded();
            ++counts[folded];
            if (!labels.contains(folded)) {
                labels.insert(folded, text);
            }
        }
        QStringList keys = counts.keys();
        std::sort(keys.begin(), keys.end(), [&labels](const QString& a, const QString& b) {
            if (a.isEmpty() != b.isEmpty()) {
                return b.isEmpty();
            }
            return QString::localeAwareCompare(labels.value(a), labels.value(b)) < 0;
        });
        const QString field = category == Category::Artist
                ? QStringLiteral("artist")
                : QStringLiteral("album");
        for (const QString& key : keys) {
            const QString label = labels.value(key);
            values.append({label.isEmpty()
                            ? (category == Category::Artist ? translate("Unknown artist")
                                                            : translate("Unknown album"))
                            : label,
                    exactTextQuery(field, label),
                    counts.value(key),
                    QColor()});
        }
        break;
    }
    case Category::Bpm: {
        QHash<int, int> counts;
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = model.index(row, column);
            bool ok = false;
            double bpm = index.data(Qt::EditRole).toDouble(&ok);
            if (!ok) {
                bpm = index.data().toString().toDouble(&ok);
            }
            const int bucket = ok && std::isfinite(bpm) && bpm > 0
                    ? static_cast<int>(std::lround(bpm))
                    : 0;
            ++counts[bucket];
        }
        QList<int> buckets = counts.keys();
        std::sort(buckets.begin(), buckets.end(), [](int a, int b) {
            if ((a == 0) != (b == 0)) {
                return b == 0;
            }
            return a < b;
        });
        for (int bucket : buckets) {
            values.append({bucket ? QString::number(bucket) : translate("No BPM"),
                    bpmBucketQuery(bucket),
                    counts.value(bucket),
                    QColor()});
        }
        break;
    }
    case Category::Key: {
        QHash<int, int> counts;
        for (int row = 0; row < rows; ++row) {
            const QString text = model.index(row, column).data().toString().trimmed();
            ++counts[static_cast<int>(keyFromText(text))];
        }
        QList<int> keys = counts.keys();
        std::sort(keys.begin(), keys.end(), [](int a, int b) {
            const LancelotPosition x = lancelot(static_cast<ChromaticKey>(a));
            const LancelotPosition y = lancelot(static_cast<ChromaticKey>(b));
            if (x.isValid() != y.isValid()) {
                return x.isValid();
            }
            if (x.number != y.number) {
                return x.number < y.number;
            }
            return !x.major && y.major;
        });
        for (int value : keys) {
            const auto key = static_cast<ChromaticKey>(value);
            const QString lancelotName = lancelotText(key);
            if (lancelotName.isEmpty()) {
                values.append({translate("No key"), QStringLiteral("key:\"\""), counts.value(value), QColor()});
                continue;
            }
            QString label = KeyUtils::keyToString(key);
            if (label.isEmpty()) {
                label = lancelotName;
            }
            values.append({label, QStringLiteral("key:") + lancelotName, counts.value(value), keyWheelColor(key)});
        }
        break;
    }
    default:
        break;
    }
    return values;
}

void paintGlyph(QPainter& painter, const QRectF& box, Glyph glyph, const QColor& color) {
    if (glyph == Glyph::None) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal s = std::min(box.width(), box.height());
    const QRectF r(box.center().x() - s / 2, box.center().y() - s / 2, s, s);
    QPen pen(color, std::max<qreal>(1.4, s / 12.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const auto at = [&r](qreal x, qreal y) {
        return QPointF(r.left() + x * r.width(), r.top() + y * r.height());
    };
    switch (glyph) {
    case Glyph::Tracks:
        for (qreal y : {0.25, 0.5, 0.75}) {
            painter.drawLine(at(0.32, y), at(0.88, y));
            painter.drawPoint(at(0.14, y));
        }
        break;
    case Glyph::Playlist: {
        for (qreal y : {0.22, 0.44}) {
            painter.drawLine(at(0.1, y), at(0.9, y));
        }
        painter.drawLine(at(0.1, 0.66), at(0.45, 0.66));
        QPolygonF play{at(0.6, 0.58), at(0.6, 0.9), at(0.9, 0.74)};
        painter.setBrush(color);
        painter.drawPolygon(play);
        break;
    }
    case Glyph::Crate:
        painter.drawRoundedRect(QRectF(at(0.12, 0.3), at(0.88, 0.86)), 2, 2);
        painter.drawLine(at(0.12, 0.48), at(0.88, 0.48));
        painter.drawLine(at(0.4, 0.62), at(0.6, 0.62));
        break;
    case Glyph::History:
        painter.drawEllipse(QRectF(at(0.12, 0.12), at(0.88, 0.88)));
        painter.drawLine(at(0.5, 0.5), at(0.5, 0.26));
        painter.drawLine(at(0.5, 0.5), at(0.68, 0.6));
        break;
    case Glyph::Artist:
        painter.drawEllipse(QRectF(at(0.33, 0.1), at(0.67, 0.44)));
        painter.drawArc(QRectF(at(0.14, 0.54), at(0.86, 1.24)), 0, 180 * 16);
        break;
    case Glyph::Album:
        painter.drawEllipse(QRectF(at(0.1, 0.1), at(0.9, 0.9)));
        painter.drawEllipse(QRectF(at(0.4, 0.4), at(0.6, 0.6)));
        break;
    case Glyph::Bpm: {
        QPolygonF pulse{at(0.05, 0.55), at(0.3, 0.55), at(0.4, 0.2), at(0.55, 0.85),
                at(0.66, 0.4), at(0.74, 0.55), at(0.95, 0.55)};
        painter.drawPolyline(pulse);
        break;
    }
    case Glyph::Key: {
        painter.drawEllipse(QRectF(at(0.1, 0.1), at(0.9, 0.9)));
        for (int n = 0; n < 12; n += 3) {
            const qreal angle = n * kPi / 6;
            painter.drawLine(at(0.5 + 0.22 * std::cos(angle), 0.5 + 0.22 * std::sin(angle)),
                    at(0.5 + 0.36 * std::cos(angle), 0.5 + 0.36 * std::sin(angle)));
        }
        break;
    }
    case Glyph::Usb:
        painter.drawRoundedRect(QRectF(at(0.3, 0.38), at(0.7, 0.92)), 2, 2);
        painter.drawRect(QRectF(at(0.38, 0.1), at(0.62, 0.38)));
        break;
    case Glyph::Folder: {
        QPainterPath path;
        path.moveTo(at(0.08, 0.84));
        path.lineTo(at(0.08, 0.2));
        path.lineTo(at(0.38, 0.2));
        path.lineTo(at(0.48, 0.32));
        path.lineTo(at(0.92, 0.32));
        path.lineTo(at(0.92, 0.84));
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }
    case Glyph::Collection:
        painter.drawEllipse(QRectF(at(0.08, 0.16), at(0.72, 0.8)));
        painter.drawEllipse(QRectF(at(0.33, 0.41), at(0.47, 0.55)));
        painter.drawLine(at(0.8, 0.12), at(0.8, 0.86));
        painter.drawLine(at(0.92, 0.12), at(0.92, 0.86));
        break;
    case Glyph::Play: {
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygonF{at(0.22, 0.12), at(0.22, 0.88), at(0.88, 0.5)});
        break;
    }
    case Glyph::Stop:
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(QRectF(at(0.18, 0.18), at(0.82, 0.82)), 1.5, 1.5);
        break;
    case Glyph::Back:
        painter.drawPolyline(QPolygonF{at(0.62, 0.14), at(0.28, 0.5), at(0.62, 0.86)});
        break;
    case Glyph::Chevron:
        painter.drawPolyline(QPolygonF{at(0.38, 0.14), at(0.72, 0.5), at(0.38, 0.86)});
        break;
    case Glyph::Search:
        painter.drawEllipse(QRectF(at(0.12, 0.12), at(0.64, 0.64)));
        painter.drawLine(at(0.58, 0.58), at(0.88, 0.88));
        break;
    case Glyph::More:
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        for (qreal x : {0.18, 0.5, 0.82}) {
            painter.drawEllipse(at(x, 0.5), s * 0.09, s * 0.09);
        }
        break;
    case Glyph::None:
        break;
    }
    painter.restore();
}

} // namespace decklab

using namespace decklab;

DeckLabActionButton::DeckLabActionButton(const QString& text, QWidget* parent)
        : QPushButton(text, parent) {
    setFocusPolicy(Qt::NoFocus);
}

void DeckLabActionButton::setDetail(const QString& detail) {
    if (m_detail == detail) {
        return;
    }
    m_detail = detail;
    update();
}

void DeckLabActionButton::setTone(Tone tone) {
    if (m_tone == tone) {
        return;
    }
    m_tone = tone;
    update();
}

void DeckLabActionButton::setGlyph(Glyph glyph) {
    if (m_glyph == glyph) {
        return;
    }
    m_glyph = glyph;
    update();
}

void DeckLabActionButton::paintEvent(QPaintEvent*) {
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();
    QPainter painter(this);
    style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool enabled = isEnabled();
    QColor primary = enabled ? colors::ink : QColor(0x8a, 0x8a, 0x8a);
    QColor secondary = enabled ? colors::muted : colors::dim;
    const QRectF inner = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    switch (m_tone) {
    case Tone::OnAir: {
        QColor fill = colors::onAir;
        fill.setAlpha(38);
        painter.setPen(QPen(colors::onAir, 2));
        painter.setBrush(fill);
        painter.drawRoundedRect(inner, 2, 2);
        primary = QColor(0xe6, 0xe6, 0xe6);
        secondary = colors::onAir;
        break;
    }
    case Tone::Active: {
        QColor fill = colors::keyMatch;
        fill.setAlpha(36);
        painter.setPen(QPen(colors::keyMatch, 2));
        painter.setBrush(fill);
        painter.drawRoundedRect(inner, 2, 2);
        secondary = colors::keyMatch;
        break;
    }
    case Tone::Ready:
        painter.setPen(Qt::NoPen);
        painter.setBrush(enabled ? colors::accent : colors::edge);
        painter.drawRect(QRectF(inner.left(), inner.top(), 3, inner.height()));
        break;
    case Tone::Neutral:
        break;
    }
    const QFont primaryFont = pixelFont(font(), height() >= 48 ? 16 : 14, true);
    const QFont detailFont = pixelFont(font(), 10, true);
    const QFontMetrics primaryMetrics(primaryFont);
    const QFontMetrics detailMetrics(detailFont);
    const QRect content = rect().adjusted(8, 2, -8, -2);
    if (text().isEmpty() && m_glyph != Glyph::None) {
        // Icon-only header buttons (Back, Search, Tools).
        const qreal size = std::min(20, content.height() - 16);
        paintGlyph(painter,
                QRectF(content.center().x() - size / 2 + 0.5, content.center().y() - size / 2 + 0.5, size, size),
                m_glyph,
                primary);
        return;
    }
    int glyphSpace = m_glyph == Glyph::None ? 0 : 20;
    const int textWidth = std::max(0, content.width() - glyphSpace);
    const QString label = primaryMetrics.elidedText(text().toUpper(), Qt::ElideRight, textWidth);
    const int labelWidth = primaryMetrics.horizontalAdvance(label);
    const int blockLeft = content.left() + (content.width() - labelWidth - glyphSpace) / 2;
    const bool twoLines = !m_detail.isEmpty() && content.height() >= primaryMetrics.height() + detailMetrics.height() - 2;
    const int primaryTop = twoLines
            ? content.top() + (content.height() - primaryMetrics.height() - detailMetrics.height() + 2) / 2
            : content.top() + (content.height() - primaryMetrics.height()) / 2;
    if (glyphSpace) {
        paintGlyph(painter,
                QRectF(blockLeft, primaryTop + (primaryMetrics.height() - 13) / 2.0, 13, 13),
                m_glyph,
                m_tone == Tone::Active ? colors::keyMatch : primary);
    }
    painter.setFont(primaryFont);
    painter.setPen(primary);
    painter.drawText(QRect(blockLeft + glyphSpace, primaryTop, labelWidth + 2, primaryMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            label);
    if (twoLines) {
        painter.setFont(detailFont);
        painter.setPen(secondary);
        painter.drawText(QRect(content.left(),
                                 primaryTop + primaryMetrics.height() - 2,
                                 content.width(),
                                 detailMetrics.height()),
                Qt::AlignHCenter | Qt::AlignVCenter,
                detailMetrics.elidedText(m_detail.toUpper(), Qt::ElideRight, content.width()));
    }
}

DeckLabBreadcrumb::DeckLabBreadcrumb(QWidget* parent)
        : QWidget(parent) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setFixedHeight(44);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

QSize DeckLabBreadcrumb::sizeHint() const {
    return QSize(200, 44);
}

void DeckLabBreadcrumb::setSegments(const QStringList& segments) {
    if (m_segments == segments) {
        return;
    }
    m_segments = segments;
    const QString full = segments.join(QStringLiteral(" › "));
    setToolTip(full);
    setAccessibleName(full);
    update();
}

QStringList DeckLabBreadcrumb::displayedSegments() const {
    const QFontMetrics metrics(pixelFont(font(), 15, true));
    return elideBreadcrumb(m_segments,
            std::max(0, width() - 20),
            [&metrics](const QString& text) {
                return metrics.horizontalAdvance(text);
            },
            QStringLiteral("  ›  "));
}

void DeckLabBreadcrumb::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QStringList shown = displayedSegments();
    const QFont lastFont = pixelFont(font(), 15, true);
    const QFont pathFont = pixelFont(font(), 14);
    const QString separator = QStringLiteral("  ›  ");
    int x = 10;
    const int available = width() - 10;
    for (int n = 0; n < shown.size(); ++n) {
        const bool last = n + 1 == shown.size();
        const QFont& segmentFont = last ? lastFont : pathFont;
        const QFontMetrics metrics(segmentFont);
        const QString text = metrics.elidedText(shown[n], Qt::ElideRight, std::max(0, available - x));
        painter.setFont(segmentFont);
        painter.setPen(last ? colors::ink : colors::muted);
        painter.drawText(QRect(x, 0, available - x, height()), Qt::AlignLeft | Qt::AlignVCenter, text);
        x += metrics.horizontalAdvance(text);
        if (!last) {
            const QFontMetrics separatorMetrics(pathFont);
            painter.setFont(pathFont);
            painter.setPen(colors::dim);
            painter.drawText(QRect(x, 0, available - x, height()), Qt::AlignLeft | Qt::AlignVCenter, separator);
            x += separatorMetrics.horizontalAdvance(separator);
        }
        if (x >= available) {
            break;
        }
    }
}

DeckLabTrackCard::DeckLabTrackCard(QWidget* parent)
        : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(96);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

QSize DeckLabTrackCard::sizeHint() const {
    return QSize(228, 200);
}

void DeckLabTrackCard::enableCovers() {
    if (m_covers) {
        return;
    }
    CoverArtCache* cache = CoverArtCache::instance();
    if (!cache) {
        return;
    }
    m_covers = true;
    QObject::connect(cache,
            &CoverArtCache::coverFound,
            this,
            [this](const QObject* requester, const CoverInfo& info, const QPixmap& pixmap) {
                if (requester != this || !m_track || info.trackLocation != m_location) {
                    return;
                }
                m_cover = pixmap;
                m_coverScaled = QPixmap();
                update();
            });
}

void DeckLabTrackCard::showTrack(const TrackPointer& track) {
    if (!track) {
        showItem(Glyph::Tracks,
                tr("No track selected"),
                tr("Turn the browse knob or tap a track"),
                tr("Preview and Load use the highlighted track"));
        return;
    }
    const bool sameTrack = !m_itemMode && m_track == track;
    m_itemMode = false;
    m_track = track;
    m_location = track->getLocation();
    m_title = track->getTitle().trimmed();
    if (m_title.isEmpty()) {
        m_title = QFileInfo(m_location).completeBaseName();
    }
    m_artist = track->getArtist().trimmed();
    m_album = track->getAlbum().trimmed();
    m_genre = track->getGenre().trimmed();
    m_comment = track->getComment().simplified();
    m_key = track->getKey();
    m_keyText = m_key != mixxx::track::io::key::INVALID
            ? KeyUtils::keyToString(m_key)
            : track->getKeyText();
    if (m_key == mixxx::track::io::key::INVALID && !m_keyText.isEmpty()) {
        m_key = keyFromText(m_keyText);
    }
    m_bpm = track->getBpm();
    m_duration = track->getDuration();
    m_rating = track->getRating();
    m_color = mixxx::RgbColor::toQColor(track->getColor());
    if (!sameTrack) {
        m_cover = QPixmap();
        m_coverScaled = QPixmap();
        requestCover();
    }
    setAccessibleName(m_artist.isEmpty() ? m_title : QString(m_title + QStringLiteral(" — ") + m_artist));
    update();
}

void DeckLabTrackCard::showItem(Glyph glyph,
        const QString& title,
        const QString& subtitle,
        const QString& hint,
        const QColor& accent) {
    if (m_itemMode && m_glyph == glyph && m_title == title && m_subtitle == subtitle &&
            m_hint == hint && m_accent == accent) {
        return;
    }
    m_itemMode = true;
    m_track.reset();
    m_location.clear();
    m_cover = QPixmap();
    m_coverScaled = QPixmap();
    m_glyph = glyph;
    m_title = title;
    m_subtitle = subtitle;
    m_hint = hint;
    m_accent = accent;
    setAccessibleName(title);
    update();
}

void DeckLabTrackCard::refreshMatch() {
    if (!m_itemMode) {
        update();
    }
}

QString DeckLabTrackCard::matchSummary() const {
    const MatchReference& reference = matchReference();
    if (m_itemMode || !reference.active()) {
        return {};
    }
    QStringList parts;
    const HarmonicMatch harmonic = harmonicMatch(m_key, reference.key);
    if (harmonic == HarmonicMatch::Same) {
        parts << tr("same key");
    } else if (harmonic == HarmonicMatch::Relative) {
        parts << tr("relative key");
    } else if (harmonic == HarmonicMatch::Adjacent) {
        parts << tr("key ±1");
    }
    const double difference = tempoDifferencePercent(m_bpm, reference.bpm);
    if (std::isfinite(difference)) {
        parts << QStringLiteral("%1%2%").arg(difference >= 0 ? QStringLiteral("+") : QString(),
                         QLocale().toString(difference, 'f', 1));
    }
    if (parts.isEmpty()) {
        return {};
    }
    return tr("vs Deck %1 · %2").arg(reference.deck).arg(parts.join(QStringLiteral(" · ")));
}

void DeckLabTrackCard::requestCover() {
    if (!m_covers || !m_track) {
        return;
    }
    const CoverInfo info = m_track->getCoverInfoWithLocation();
    if (!info.hasImage()) {
        return;
    }
    constexpr int kCoverWidth = 128;
    const QPixmap cached = CoverArtCache::getCachedCover(info, kCoverWidth);
    if (!cached.isNull()) {
        m_cover = cached;
        return;
    }
    CoverArtCache::requestUncachedCover(this, m_track, kCoverWidth);
}

void DeckLabTrackCard::resizeEvent(QResizeEvent* event) {
    m_coverScaled = QPixmap();
    QWidget::resizeEvent(event);
}

void DeckLabTrackCard::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(0x10, 0x10, 0x10));
    painter.setPen(QPen(colors::edge, 1));
    painter.setBrush(colors::panel);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    if (m_itemMode) {
        paintItem(painter);
    } else {
        paintTrack(painter);
    }
}

void DeckLabTrackCard::paintArtwork(QPainter& painter, const QRect& box) {
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(QRectF(box), 3, 3);
    painter.setClipPath(clip);
    if (!m_cover.isNull()) {
        if (m_coverScaled.isNull() || m_coverScaled.size() != box.size()) {
            const QPixmap scaled = m_cover.scaled(box.size(),
                    Qt::KeepAspectRatioByExpanding,
                    Qt::SmoothTransformation);
            m_coverScaled = scaled.copy((scaled.width() - box.width()) / 2,
                    (scaled.height() - box.height()) / 2,
                    box.width(),
                    box.height());
        }
        painter.drawPixmap(box.topLeft(), m_coverScaled);
    } else {
        QColor base = m_color.isValid() ? m_color : keyWheelColor(m_key);
        if (!base.isValid()) {
            base = QColor(0x3a, 0x3a, 0x3a);
        }
        QLinearGradient gradient(box.topLeft(), box.bottomRight());
        gradient.setColorAt(0, base.darker(150));
        gradient.setColorAt(1, QColor(0x14, 0x14, 0x14));
        painter.fillRect(box, gradient);
        const QPointF center = QRectF(box).center();
        const qreal radius = box.width() * 0.34;
        painter.setBrush(QColor(0, 0, 0, 90));
        painter.setPen(QPen(QColor(255, 255, 255, 46), 1));
        painter.drawEllipse(center, radius, radius);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius * 0.7, radius * 0.7);
        painter.setPen(Qt::NoPen);
        painter.setBrush(base.lighter(130));
        painter.drawEllipse(center, radius * 0.24, radius * 0.24);
        painter.setBrush(QColor(0x10, 0x10, 0x10));
        painter.drawEllipse(center, 2, 2);
    }
    painter.restore();
    painter.setPen(QPen(QColor(255, 255, 255, 20), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
}

void DeckLabTrackCard::paintTrack(QPainter& painter) {
    const bool compact = height() < 176;
    const int pad = 10;
    const int art = compact ? 64 : 84;
    const QRect artBox(pad, pad, art, art);
    paintArtwork(painter, artBox);

    const int textLeft = artBox.right() + 10;
    const int textWidth = width() - textLeft - pad;
    const QFont titleFont = pixelFont(font(), 15, true);
    const QFont bodyFont = pixelFont(font(), 13);
    const QFont smallFont = pixelFont(font(), 11);
    int y = drawWrapped(painter,
            QRect(textLeft, pad - 2, textWidth, art - 14),
            m_title,
            titleFont,
            colors::ink,
            compact ? 2 : 3);
    drawWrapped(painter,
            QRect(textLeft, y + 1, textWidth, artBox.bottom() - y),
            m_artist.isEmpty() ? tr("Unknown artist") : m_artist,
            bodyFont,
            m_artist.isEmpty() ? colors::dim : colors::muted,
            2);

    const MatchReference& reference = matchReference();
    const HarmonicMatch harmonic = reference.active()
            ? harmonicMatch(m_key, reference.key)
            : HarmonicMatch::None;
    const bool tempo = reference.active() && tempoMatches(m_bpm, reference.bpm);
    const int chipTop = artBox.bottom() + 9;
    const int chipHeight = compact ? 34 : 40;
    const int gap = 6;
    const int chipWidth = (width() - 2 * pad - 2 * gap) / 3;
    paintChip(painter,
            QRect(pad, chipTop, chipWidth, chipHeight),
            bpmText(m_bpm),
            tr("BPM"),
            tempo ? colors::tempoMatch : QColor(),
            font());
    paintChip(painter,
            QRect(pad + chipWidth + gap, chipTop, chipWidth, chipHeight),
            m_keyText.isEmpty() ? QStringLiteral("—") : m_keyText,
            tr("KEY"),
            harmonic != HarmonicMatch::None ? colors::keyMatch : QColor(),
            font());
    paintChip(painter,
            QRect(pad + 2 * (chipWidth + gap), chipTop, chipWidth, chipHeight),
            durationText(m_duration),
            tr("TIME"),
            QColor(),
            font());

    y = chipTop + chipHeight + 8;
    const int bottom = height() - pad;
    const QFontMetrics smallMetrics(smallFont);
    // Match summary, rating and color share one line.
    if (y + 14 <= bottom) {
        int x = pad;
        for (int star = 0; star < 5; ++star) {
            drawStar(painter, QPointF(x + 6, y + 7), 5.5, star < m_rating,
                    star < m_rating ? QColor(0xf5, 0xc5, 0x42) : QColor(0x55, 0x55, 0x55));
            x += 14;
        }
        if (m_color.isValid()) {
            painter.setPen(QPen(QColor(255, 255, 255, 60), 1));
            painter.setBrush(m_color);
            painter.drawEllipse(QPointF(x + 7, y + 7), 5, 5);
            x += 18;
        }
        const QString summary = matchSummary();
        if (!summary.isEmpty()) {
            painter.setFont(smallFont);
            painter.setPen(harmonic != HarmonicMatch::None || tempo ? colors::keyMatch : colors::muted);
            painter.drawText(QRect(x + 4, y, width() - pad - x - 4, 14),
                    Qt::AlignRight | Qt::AlignVCenter,
                    smallMetrics.elidedText(summary, Qt::ElideRight, width() - pad - x - 4));
        }
        y += 20;
    }
    QStringList about;
    if (!m_album.isEmpty()) {
        about << m_album;
    }
    if (!m_genre.isEmpty()) {
        about << m_genre;
    }
    if (!about.isEmpty() && y + smallMetrics.height() <= bottom) {
        painter.setFont(smallFont);
        painter.setPen(colors::muted);
        painter.drawText(QRect(pad, y, width() - 2 * pad, smallMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                smallMetrics.elidedText(about.join(QStringLiteral(" · ")), Qt::ElideRight, width() - 2 * pad));
        y += smallMetrics.height() + 2;
    }
    if (!m_comment.isEmpty() && y + smallMetrics.height() <= bottom) {
        QFont commentFont = smallFont;
        commentFont.setItalic(true);
        drawWrapped(painter,
                QRect(pad, y, width() - 2 * pad, bottom - y),
                m_comment,
                commentFont,
                colors::dim,
                2);
    }
}

void DeckLabTrackCard::paintItem(QPainter& painter) {
    const int pad = 12;
    const int tile = height() < 150 ? 48 : 64;
    const QRect tileBox(pad, pad, tile, tile);
    QColor base = m_accent.isValid() ? m_accent : QColor(0x2a, 0x2a, 0x2a);
    QLinearGradient gradient(tileBox.topLeft(), tileBox.bottomRight());
    gradient.setColorAt(0, m_accent.isValid() ? base.darker(160) : QColor(0x2e, 0x2e, 0x2e));
    gradient.setColorAt(1, QColor(0x18, 0x18, 0x18));
    painter.setPen(QPen(QColor(255, 255, 255, 22), 1));
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(tileBox).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    paintGlyph(painter,
            QRectF(tileBox).adjusted(tile * 0.27, tile * 0.27, -tile * 0.27, -tile * 0.27),
            m_glyph,
            m_accent.isValid() ? m_accent.lighter(120) : colors::muted);
    const int textLeft = tileBox.right() + 12;
    const int textWidth = width() - textLeft - pad;
    int y = drawWrapped(painter,
            QRect(textLeft, pad - 1, textWidth, tile + 20),
            m_title,
            pixelFont(font(), 16, true),
            colors::ink,
            3);
    drawWrapped(painter,
            QRect(textLeft, y + 2, textWidth, std::max(tile + pad - y, 34)),
            m_subtitle,
            pixelFont(font(), 13),
            colors::muted,
            2);
    if (!m_hint.isEmpty()) {
        const QFont hintFont = pixelFont(font(), 12);
        const QFontMetrics metrics(hintFont);
        const int lines = height() - tileBox.bottom() - 2 * pad >= 2 * metrics.height() ? 2 : 1;
        const int top = height() - pad - lines * metrics.height();
        if (top > tileBox.bottom() + 6) {
            painter.setPen(QPen(colors::edge, 1));
            painter.drawLine(pad, top - 8, width() - pad, top - 8);
            drawWrapped(painter,
                    QRect(pad, top, width() - 2 * pad, lines * metrics.height()),
                    m_hint,
                    hintFont,
                    colors::dim,
                    lines);
        }
    }
}

DeckLabToast::DeckLabToast(QWidget* parent)
        : QLabel(parent) {
    setObjectName(QStringLiteral("decklab-toast"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAlignment(Qt::AlignCenter);
    setWordWrap(false);
    setStyleSheet(QStringLiteral(
            "QLabel#decklab-toast{background:rgba(24,24,24,245);color:#ffffff;"
            "border:2px solid #ff5a5f;border-radius:4px;padding:10px 18px;"
            "font-size:16px;font-weight:600;}"));
    m_timer.setSingleShot(true);
    QObject::connect(&m_timer, &QTimer::timeout, this, &QWidget::hide);
    hide();
}

void DeckLabToast::showMessage(const QString& text, int milliseconds) {
    setText(text);
    setAccessibleName(text);
    adjustSize();
    QWidget* surface = parentWidget();
    if (surface) {
        const int width = std::min(this->width(), surface->width() - 32);
        resize(width, height());
        move((surface->width() - width) / 2, 52);
    }
    raise();
    show();
    m_timer.start(milliseconds);
}

DeckLabListDelegate::DeckLabListDelegate(QObject* parent)
        : QStyledItemDelegate(parent) {
}

QSize DeckLabListDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
    return QSize(option.rect.width(), 44);
}

void DeckLabListDelegate::paint(QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    painter->fillRect(r, selected ? QColor(0x2a, 0x2a, 0x2a) : (index.row() % 2 ? QColor(0x16, 0x16, 0x16) : QColor(0x11, 0x11, 0x11)));
    painter->setPen(QColor(0x24, 0x24, 0x24));
    painter->drawLine(r.bottomLeft(), r.bottomRight());
    if (selected) {
        painter->fillRect(QRect(r.left(), r.top(), 3, r.height()), colors::accent);
    }
    int x = r.left() + 16;
    const QColor textColor = enabled ? colors::ink : colors::dim;
    const auto glyph = static_cast<Glyph>(index.data(GlyphRole).toInt());
    if (glyph != Glyph::None) {
        paintGlyph(*painter, QRectF(x, r.center().y() - 10, 20, 20), glyph, selected ? colors::ink : colors::muted);
        x += 34;
    }
    const QColor accent = index.data(AccentRole).value<QColor>();
    if (accent.isValid()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(accent);
        painter->drawRoundedRect(QRectF(x, r.center().y() - 9, 6, 18), 2, 2);
        x += 16;
    }
    const bool chevron = index.data(ChevronRole).toBool();
    int right = r.right() - (chevron ? 30 : 14);
    if (chevron) {
        paintGlyph(*painter,
                QRectF(r.right() - 26, r.center().y() - 7, 14, 14),
                Glyph::Chevron,
                QColor(0x5c, 0x5c, 0x5c));
    }
    const QString count = index.data(CountRole).toString();
    QFont countFont = option.font;
    countFont.setPixelSize(13);
    if (!count.isEmpty()) {
        const QFontMetrics metrics(countFont);
        const int countWidth = metrics.horizontalAdvance(count) + 4;
        painter->setFont(countFont);
        painter->setPen(enabled ? colors::muted : colors::dim);
        painter->drawText(QRect(right - countWidth, r.top(), countWidth, r.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                count);
        right -= countWidth + 12;
    }
    QFont labelFont = option.font;
    labelFont.setPixelSize(15);
    labelFont.setBold(selected);
    const QFontMetrics labelMetrics(labelFont);
    painter->setFont(labelFont);
    painter->setPen(textColor);
    painter->drawText(QRect(x, r.top(), std::max(0, right - x), r.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            labelMetrics.elidedText(index.data().toString(), Qt::ElideRight, std::max(0, right - x)));
    painter->restore();
}
