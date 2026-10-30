// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabbrowse.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QListWidget>
#include <QSet>
#include <QSignalBlocker>
#include <QStorageInfo>
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

#include "decklab/decklabkeys.h" // r6-keys
#include "decklab/decklaboverlay.h"
#include "decklab/decklabusbname.h"
#include "library/coverart.h"
#include "library/coverartcache.h"
#include "library/starrating.h"
#include "library/trackmodel.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "util/color/rgbcolor.h"
#include "widget/decklabdeckwidgets.h"

namespace decklab {

namespace colors {
const QColor ground(0x0d, 0x0f, 0x12);
const QColor panel(0x16, 0x19, 0x1e);
const QColor raise(0x20, 0x24, 0x2b);
const QColor edge(0x2c, 0x31, 0x3a);
const QColor ink(0xf2, 0xf4, 0xf7);
const QColor muted(0x9a, 0xa3, 0xaf);
const QColor dim(0x5d, 0x64, 0x70);
const QColor accent(0xf2, 0xf4, 0xf7);
const QColor keyMatch(0x3e, 0xcf, 0x8e);
const QColor tempoMatch(0xf2, 0xf4, 0xf7);
const QColor onAir(0xff, 0x3d, 0x2e);
} // namespace colors

namespace {
QFont brandFont(const QFont& base, const QStringList& families, int pixels, QFont::Weight weight) {
    QFont font = base;
    QStringList all = families;
    all << base.family();
    font.setFamilies(all);
    font.setPixelSize(pixels);
    font.setWeight(weight);
    font.setItalic(false);
    return font;
}
} // namespace

QFont titleFont(const QFont& base) {
    return brandFont(base, {QStringLiteral("Barlow")}, 18, QFont::DemiBold);
}
QFont bodyFont(const QFont& base) {
    return brandFont(base, {QStringLiteral("Barlow")}, 16, QFont::Medium);
}
QFont strongFont(const QFont& base) {
    return brandFont(base, {QStringLiteral("Barlow")}, 16, QFont::DemiBold);
}
QFont secondaryFont(const QFont& base) {
    return brandFont(base, {QStringLiteral("Barlow")}, 15, QFont::Medium);
}
QFont labelFont(const QFont& base) {
    QFont font = brandFont(base, {QStringLiteral("Barlow")}, 14, QFont::DemiBold);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    return font;
}
QFont numberFont(const QFont& base, int pixels) {
    QFont font = brandFont(base,
            {QStringLiteral("Barlow Semi Condensed"), QStringLiteral("Barlow")},
            std::max(14, pixels),
            QFont::DemiBold);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    return font;
}

namespace {
constexpr double kPi = 3.14159265358979323846;

QString translate(const char* text) {
    return QCoreApplication::translate("DeckLabBrowse", text);
}

MatchReference& mutableReference() {
    static MatchReference reference;
    return reference;
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
    QColor fill = colors::raise;
    if (tone.isValid()) {
        fill = tone;
        fill.setAlpha(40);
    }
    painter.setPen(QPen(tone.isValid() ? tone : colors::edge, tone.isValid() ? 1.5 : 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(frame, 3, 3);
    const QFont valueFont = numberFont(base, box.height() >= 48 ? 18 : 16);
    const QFont captionFont = labelFont(base);
    const QFontMetrics valueMetrics(valueFont);
    const QFontMetrics captionMetrics(captionFont);
    const int top = box.top() + std::max(1, (box.height() - valueMetrics.height() - captionMetrics.height()) / 2);
    painter.setFont(valueFont);
    painter.setPen(tone.isValid() ? tone.lighter(115) : colors::ink);
    painter.drawText(QRect(box.left() + 3, top, box.width() - 6, valueMetrics.height()),
            Qt::AlignCenter,
            valueMetrics.elidedText(value, Qt::ElideRight, box.width() - 6));
    painter.setFont(captionFont);
    painter.setPen(tone.isValid() ? tone : colors::ink);
    painter.drawText(QRect(box.left() + 2, top + valueMetrics.height() - 2, box.width() - 4, captionMetrics.height()),
            Qt::AlignCenter,
            captionMetrics.elidedText(caption, Qt::ElideRight, box.width() - 4));
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

TempoWindow tempoWindow(double referenceBpm, TempoDirection direction, double tolerance) {
    if (!std::isfinite(referenceBpm) || referenceBpm <= 0) {
        return {};
    }
    TempoWindow window;
    window.low = direction == TempoDirection::Faster ? referenceBpm : referenceBpm * (1.0 - tolerance);
    window.high = direction == TempoDirection::Slower ? referenceBpm : referenceBpm * (1.0 + tolerance);
    return window;
}

bool tempoInWindow(double trackBpm, const TempoWindow& window) {
    return window.isValid() && std::isfinite(trackBpm) && trackBpm > 0 &&
            trackBpm >= window.low - 1e-6 && trackBpm <= window.high + 1e-6;
}

QString tempoWindowText(const TempoWindow& window) {
    if (!window.isValid()) {
        return {};
    }
    return QStringLiteral("%1%2%3")
            .arg(std::lround(window.low))
            .arg(QChar(0x2013))
            .arg(std::lround(window.high));
}

QString bpmDeltaText(double trackBpm, double referenceBpm) {
    if (!std::isfinite(trackBpm) || !std::isfinite(referenceBpm) ||
            trackBpm <= 0 || referenceBpm <= 0) {
        return {};
    }
    const double delta = trackBpm - referenceBpm;
    if (std::abs(delta) < 0.05) {
        return QString(QChar(0x00b1)) + QLatin1Char('0');
    }
    const double size = std::abs(delta);
    return QString(QChar(delta > 0 ? 0x25b2 : 0x25bc)) +
            QString::number(size, 'f', size < 9.95 ? 1 : 0);
}

QString keyRelationText(ChromaticKey track, ChromaticKey reference) {
    switch (harmonicMatch(track, reference)) {
    case HarmonicMatch::Same:
        return translate("Same key");
    case HarmonicMatch::Relative:
        // r6-keys: in the chosen key format ("Relative 8A→8B", "Relative Am→C").
        return translate("Relative %1→%2").arg(formatKey(reference), formatKey(track));
    case HarmonicMatch::Adjacent: {
        const int distance = (lancelot(track).number - lancelot(reference).number + 12) % 12;
        return distance == 1 ? translate("Key +1 (up)") : translate("Key −1 (down)");
    }
    case HarmonicMatch::None:
        break;
    }
    return {};
}

QVector<int> easyModeRanges() {
    return {3, 6, 10, 16};
}

namespace {
const QString kEasyGroup = QStringLiteral("[DeckLab]");
} // namespace

EasyMode loadEasyMode(const UserSettingsPointer& config) {
    EasyMode mode;
    if (!config) {
        return mode;
    }
    mode.on = config->getValue(ConfigKey(kEasyGroup, QStringLiteral("EasyMode")), 1) != 0;
    mode.matchOn = static_cast<MatchOn>(std::clamp(
            config->getValue(ConfigKey(kEasyGroup, QStringLiteral("EasyModeMatchOn")), 0), 0, 2));
    mode.keys = static_cast<KeyRule>(std::clamp(
            config->getValue(ConfigKey(kEasyGroup, QStringLiteral("EasyModeKeys")), 0), 0, 2));
    const int range = config->getValue(ConfigKey(kEasyGroup, QStringLiteral("EasyModeBpmRange")), 6);
    mode.rangePercent = easyModeRanges().contains(range) ? range : 6;
    return mode;
}

void saveEasyMode(const UserSettingsPointer& config, const EasyMode& mode) {
    if (!config) {
        return;
    }
    config->setValue(ConfigKey(kEasyGroup, QStringLiteral("EasyMode")), mode.on ? 1 : 0);
    config->setValue(ConfigKey(kEasyGroup, QStringLiteral("EasyModeMatchOn")), static_cast<int>(mode.matchOn));
    config->setValue(ConfigKey(kEasyGroup, QStringLiteral("EasyModeKeys")), static_cast<int>(mode.keys));
    config->setValue(ConfigKey(kEasyGroup, QStringLiteral("EasyModeBpmRange")), mode.rangePercent);
}

bool keyRuleAllows(KeyRule rule, HarmonicMatch match) {
    switch (match) {
    case HarmonicMatch::Same:
        return true;
    case HarmonicMatch::Relative:
        return rule != KeyRule::Same;
    case HarmonicMatch::Adjacent:
        return rule == KeyRule::Wide;
    case HarmonicMatch::None:
        break;
    }
    return false;
}

QStringList compatibleLancelotKeys(ChromaticKey reference, KeyRule rule) {
    const LancelotPosition position = lancelot(reference);
    if (!position.isValid()) {
        return {};
    }
    const QChar letter = position.major ? QLatin1Char('B') : QLatin1Char('A');
    const QChar other = position.major ? QLatin1Char('A') : QLatin1Char('B');
    const auto wheel = [](int number) {
        return (number + 11) % 12 + 1;
    };
    QStringList keys{QString::number(position.number) + letter};
    if (keyRuleAllows(rule, HarmonicMatch::Relative)) {
        keys << QString::number(position.number) + other;
    }
    if (keyRuleAllows(rule, HarmonicMatch::Adjacent)) {
        keys << QString::number(wheel(position.number - 1)) + letter
             << QString::number(wheel(position.number + 1)) + letter;
    }
    return keys;
}

QString matchFilterQuery(const MatchReference& reference, const QString& extra) {
    if (!reference.active()) {
        return {};
    }
    QStringList required;
    if (!extra.trimmed().isEmpty()) {
        required << extra.trimmed();
    }
    const TempoWindow window = reference.window();
    if (window.isValid()) {
        // QString::arg(double) always writes a '.' decimal point.
        required << QStringLiteral("bpm:%1-%2").arg(window.low, 0, 'f', 2).arg(window.high, 0, 'f', 2);
    }
    // r8-browse: Easy Mode's key rule; no key terms when it matches BPM only.
    const QStringList keys = reference.usesKey() ? compatibleLancelotKeys(reference.key, reference.keys) : QStringList();
    if (keys.isEmpty()) {
        return required.join(QLatin1Char(' '));
    }
    QStringList alternatives;
    for (const QString& key : keys) {
        QStringList terms = required;
        terms << QStringLiteral("key:") + key;
        alternatives << terms.join(QLatin1Char(' '));
    }
    return alternatives.join(QStringLiteral(" | "));
}

bool hasRekordboxExport(const QString& path) {
    for (const auto* relative : {"PIONEER/rekordbox/export.pdb", ".PIONEER/rekordbox/export.pdb"}) {
        if (QFileInfo(QDir(path).filePath(QString::fromLatin1(relative))).isFile()) {
            return true;
        }
    }
    return false;
}

void setUsbStickRootsForTesting(const QStringList& roots) {
    // Shared with the rekordbox device scan (decklab/decklabusbname.h).
    setUsbMediaRootsForTesting(roots);
}

QList<UsbStick> findUsbSticks() {
    QList<UsbStick> sticks;
#if defined(Q_OS_LINUX)
    const bool testing = usbMediaRootsAreForTesting();
    QStringList roots = testing ? usbMediaRoots() : QStringList();
    if (!testing) {
        const QString user = QString::fromLocal8Bit(qgetenv("USER"));
        roots << QStringLiteral("/media/decklab");
        if (!user.isEmpty() && user != QStringLiteral("decklab")) {
            roots << QStringLiteral("/media/") + user << QStringLiteral("/run/media/") + user;
        }
    }
    QSet<QString> seen;
    QSet<QByteArray> devices;
    QList<UsbNameSource> names; // r5-polish: friendly names, as the rail and Storage
    for (const QString& root : std::as_const(roots)) {
        const bool applianceRoot = QDir(root).dirName() == QStringLiteral("decklab");
        const QFileInfoList entries = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& entry : entries) {
            const QString path = entry.absoluteFilePath();
            if (seen.contains(path)) {
                continue;
            }
            UsbNameSource name;
            if (!testing) {
                // Only real mounts: the appliance helper removes stale
                // folders, but a desktop may leave empty ones behind. A stick
                // also automounted by a desktop is listed once (the appliance
                // mount is scanned first).
                const QStorageInfo mount(path);
                if (mount.rootPath() != path || devices.contains(mount.device())) {
                    continue;
                }
                devices.insert(mount.device());
                name.bytes = mount.bytesTotal();
            }
            seen.insert(path);
            UsbStick stick;
            stick.path = path;
            stick.rekordbox = hasRekordboxExport(path);
            // The appliance mounts by filesystem UUID and udev knows the
            // label; udisks names desktop mount folders after the label (or
            // the UUID when there is none).
            name.id = applianceRoot ? entry.fileName() : path;
            name.label = applianceRoot ? (testing ? QString() : usbLabelForUuid(entry.fileName()))
                                       : entry.fileName();
            name.rekordbox = stick.rekordbox;
            names.append(name);
            sticks.append(stick);
        }
    }
    const QStringList labels = usbNames(names);
    for (int index = 0; index < sticks.size(); ++index) {
        sticks[index].label = labels.at(index);
    }
    std::sort(sticks.begin(), sticks.end(), [](const UsbStick& a, const UsbStick& b) {
        const int order = QString::localeAwareCompare(a.label, b.label);
        return order != 0 ? order < 0 : a.path < b.path;
    });
#endif
    return sticks;
}

QStringList stickAudioFiles(const QString& folder) {
    static const QStringList patterns{QStringLiteral("*.mp3"), QStringLiteral("*.wav"),
            QStringLiteral("*.flac"), QStringLiteral("*.aif"), QStringLiteral("*.aiff"),
            QStringLiteral("*.m4a"), QStringLiteral("*.mp4"), QStringLiteral("*.ogg"),
            QStringLiteral("*.opus"), QStringLiteral("*.wv"), QStringLiteral("*.alac")};
    return QDir(folder).entryList(patterns, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
}

QStringList stickSubfolders(const QString& folder) {
    static const QStringList system{QStringLiteral("System Volume Information"),
            QStringLiteral("$RECYCLE.BIN"), QStringLiteral("RECYCLER"), QStringLiteral("LOST.DIR"),
            QStringLiteral("FOUND.000"), QStringLiteral("PIONEER"), QStringLiteral("lost+found")};
    QStringList folders;
    const QStringList entries = QDir(folder).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QString& name : entries) {
        if (name.startsWith(QLatin1Char('.')) || system.contains(name, Qt::CaseInsensitive)) {
            continue;
        }
        folders << name;
    }
    return folders;
}

ChromaticKey keyFromText(const QString& text) {
    // r6-keys: one reader for every key text (decklab/decklabkeys.h).
    return parseKey(text);
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
            automatic == other.automatic && direction == other.direction &&
            std::abs(bpm - other.bpm) < 0.005;
}
            matchOn == other.matchOn && keys == other.keys && // r8-browse
            std::abs(tolerance - other.tolerance) < 1e-9 &&

const MatchReference& matchReference() {
    return mutableReference();
HarmonicMatch MatchReference::keyMatch(ChromaticKey track) const {
    if (!usesKey()) {
        return HarmonicMatch::None;
    }
    const HarmonicMatch match = harmonicMatch(track, key);
    return keyRuleAllows(keys, match) ? match : HarmonicMatch::None;
}

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
    case Category::RecentlyAdded:
        return translate("Recently Added");
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

QDate recentlyAddedSince(const QDate& today) {
    return today.addDays(-kRecentlyAddedDays);
}

QString recentlyAddedQuery(const QDate& today) {
    return QStringLiteral("added:\">=%1\"")
            .arg(QLocale().toString(recentlyAddedSince(today), QLocale::ShortFormat));
}

int countAddedSince(const QAbstractItemModel& model, int column, const QDate& since) {
    if (column < 0 || column >= model.columnCount()) {
        return -1;
    }
    int count = 0;
    const int rows = model.rowCount();
    for (int row = 0; row < rows; ++row) {
        const QVariant value = model.index(row, column).data();
        const QDate date = value.canConvert<QDate>() ? value.toDate() : QDate();
        if (date.isValid() && date >= since) {
            ++count;
        }
    }
    return count;
}

QString recentDayLabel(const QDate& day, const QDate& today) {
    if (!day.isValid()) {
        return QString();
    }
    const qint64 ago = day.daysTo(today);
    if (ago == 0) {
        return translate("Today");
    }
    if (ago == 1) {
        return translate("Yesterday");
    }
    return QLocale().toString(day, day.year() == today.year() ? QStringLiteral("d MMM") : QStringLiteral("d MMM yyyy"));
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
        // r6-keys: round the wheel of the chosen key format, keyless last.
        std::sort(keys.begin(), keys.end(), [](int a, int b) {
            const int x = keyWheelOrder(static_cast<ChromaticKey>(a));
            const int y = keyWheelOrder(static_cast<ChromaticKey>(b));
            if ((x > 0) != (y > 0)) {
                return x > 0;
            }
            return x < y;
        });
        for (int value : keys) {
            const auto key = static_cast<ChromaticKey>(value);
            const QString lancelotName = lancelotText(key);
            if (lancelotName.isEmpty()) {
                values.append({translate("No key"), QStringLiteral("key:\"\""), counts.value(value), QColor()});
                continue;
            }
            // The label in the chosen format; the query stays a wheel name,
            // which Mixxx's key filter reads whatever the format.
            values.append({formatKey(key), QStringLiteral("key:") + lancelotName, counts.value(value), keyWheelColor(key)});
        }
        break;
    }
    default:
        break;
    }
    return values;
}

void relabelKeyValues(QListWidget& list) {
    const auto keyOf = [](const QListWidgetItem* item) {
        const QString query = item->data(DeckLabListDelegate::QueryRole).toString();
        return query.startsWith(QStringLiteral("key:")) ? parseKey(query.mid(4))
                                                         : mixxx::track::io::key::INVALID;
    };
    const QSignalBlocker blocker(&list);
    QListWidgetItem* current = list.currentItem();
    QList<QListWidgetItem*> rows;
    while (list.count()) {
        rows.append(list.takeItem(0));
    }
    for (QListWidgetItem* row : std::as_const(rows)) {
        const ChromaticKey key = keyOf(row);
        if (key != mixxx::track::io::key::INVALID) {
            row->setText(formatKey(key));
        }
    }
    // The order collectCategoryValues() gives: round the wheel, keyless last.
    std::stable_sort(rows.begin(), rows.end(), [&keyOf](const QListWidgetItem* a, const QListWidgetItem* b) {
        const int x = keyWheelOrder(keyOf(a));
        const int y = keyWheelOrder(keyOf(b));
        if ((x > 0) != (y > 0)) {
            return x > 0;
        }
        return x < y;
    });
    for (QListWidgetItem* row : std::as_const(rows)) {
        list.addItem(row);
    }
    if (current) {
        list.setCurrentItem(current);
    }
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
        // Stick body with its label notch, and the plug.
        painter.drawRoundedRect(QRectF(at(0.28, 0.36), at(0.72, 0.92)), 2, 2);
        painter.drawRect(QRectF(at(0.38, 0.08), at(0.62, 0.36)));
        painter.drawLine(at(0.44, 0.18), at(0.44, 0.24));
        painter.drawLine(at(0.56, 0.18), at(0.56, 0.24));
        painter.drawLine(at(0.4, 0.62), at(0.6, 0.62));
        break;
    case Glyph::Mic:
        painter.drawRoundedRect(QRectF(at(0.36, 0.06), at(0.64, 0.56)), s * 0.14, s * 0.14);
        painter.drawArc(QRectF(at(0.22, 0.2), at(0.78, 0.72)), 180 * 16, 180 * 16);
        painter.drawLine(at(0.5, 0.72), at(0.5, 0.9));
        painter.drawLine(at(0.32, 0.92), at(0.68, 0.92));
        break;
    case Glyph::Sources:
        for (qreal x : {0.1, 0.56}) {
            for (qreal y : {0.1, 0.56}) {
                painter.drawRoundedRect(QRectF(at(x, y), at(x + 0.34, y + 0.34)), 2, 2);
            }
        }
        break;
    case Glyph::AutoDj: {
        painter.drawArc(QRectF(at(0.14, 0.14), at(0.86, 0.86)), 30 * 16, 300 * 16);
        painter.drawPolyline(QPolygonF{at(0.62, 0.1), at(0.82, 0.3), at(0.56, 0.36)});
        break;
    }
    case Glyph::Analyze:
        painter.drawLine(at(0.16, 0.44), at(0.16, 0.84));
        painter.drawLine(at(0.39, 0.16), at(0.39, 0.84));
        painter.drawLine(at(0.62, 0.32), at(0.62, 0.84));
        painter.drawLine(at(0.85, 0.56), at(0.85, 0.84));
        break;
    case Glyph::ArrowDown:
        painter.drawLine(at(0.5, 0.12), at(0.5, 0.86));
        painter.drawPolyline(QPolygonF{at(0.22, 0.58), at(0.5, 0.86), at(0.78, 0.58)});
        break;
    case Glyph::ArrowUp:
        painter.drawLine(at(0.5, 0.14), at(0.5, 0.88));
        painter.drawPolyline(QPolygonF{at(0.22, 0.42), at(0.5, 0.14), at(0.78, 0.42)});
        break;
    case Glyph::ArrowBoth:
        painter.drawLine(at(0.5, 0.12), at(0.5, 0.88));
        painter.drawPolyline(QPolygonF{at(0.24, 0.36), at(0.5, 0.1), at(0.76, 0.36)});
        painter.drawPolyline(QPolygonF{at(0.24, 0.64), at(0.5, 0.9), at(0.76, 0.64)});
        break;
    case Glyph::Close:
        painter.drawLine(at(0.2, 0.2), at(0.8, 0.8));
        painter.drawLine(at(0.8, 0.2), at(0.2, 0.8));
        break;
    case Glyph::Warning:
        painter.drawPolygon(QPolygonF{at(0.5, 0.08), at(0.94, 0.88), at(0.06, 0.88)});
        painter.drawLine(at(0.5, 0.36), at(0.5, 0.6));
        painter.drawPoint(at(0.5, 0.74));
        break;
    case Glyph::Retry:
        painter.drawArc(QRectF(at(0.14, 0.14), at(0.86, 0.86)), 60 * 16, 290 * 16);
        painter.drawPolyline(QPolygonF{at(0.5, 0.04), at(0.7, 0.16), at(0.52, 0.32)});
        break;
    case Glyph::Download:
        // An arrow into a tray: what Get Music saved.
        painter.drawLine(at(0.5, 0.08), at(0.5, 0.62));
        painter.drawPolyline(QPolygonF{at(0.28, 0.42), at(0.5, 0.64), at(0.72, 0.42)});
        painter.drawPolyline(QPolygonF{at(0.12, 0.66), at(0.12, 0.9), at(0.88, 0.9), at(0.88, 0.66)});
        break;
    case Glyph::Sample:
        // A pad holding a short sound.
        painter.drawRoundedRect(QRectF(at(0.08, 0.08), at(0.92, 0.92)), s * 0.12, s * 0.12);
        painter.drawPolyline(QPolygonF{at(0.24, 0.5), at(0.36, 0.5), at(0.44, 0.28), at(0.54, 0.72),
                at(0.62, 0.38), at(0.68, 0.5), at(0.78, 0.5)});
        break;
    case Glyph::Recent:
        // r7-recent: the track list with a plus: what joined it lately.
        for (qreal y : {0.22, 0.47}) {
            painter.drawLine(at(0.32, y), at(0.88, y));
            painter.drawPoint(at(0.14, y));
        }
        painter.drawPoint(at(0.14, 0.72));
        painter.drawLine(at(0.32, 0.72), at(0.5, 0.72));
        painter.drawLine(at(0.62, 0.74), at(0.9, 0.74));
        painter.drawLine(at(0.76, 0.6), at(0.76, 0.88));
        break;
    case Glyph::Mixes:
        // Two keys that overlap: what mixes with the playing deck.
        painter.drawEllipse(QRectF(at(0.06, 0.22), at(0.62, 0.78)));
        painter.drawEllipse(QRectF(at(0.38, 0.22), at(0.94, 0.78)));
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

void DeckLabActionButton::setDetail(const QString& detail, const QString& shortDetail) {
    if (m_detail == detail && m_shortDetail == shortDetail) {
        return;
    }
    m_detail = detail;
    m_shortDetail = shortDetail;
    update();
}

QString DeckLabActionButton::shownDetail() const {
    if (m_shortDetail.isEmpty()) {
        return m_detail;
    }
    const int width = rect().adjusted(8, 2, -8, -2).width();
    return QFontMetrics(secondaryFont(font())).horizontalAdvance(m_detail) <= width ? m_detail : m_shortDetail;
}

bool DeckLabActionButton::textFits() const {
    if (m_deck > 0 || (text().isEmpty() && m_glyph != Glyph::None)) {
        return true; // LOAD targets have their own layout; glyph-only buttons
    }
    const QRect content = rect().adjusted(8, 2, -8, -2);
    const QFontMetrics primaryMetrics(strongFont(font()));
    const int textWidth = std::max(0, content.width() - (m_glyph == Glyph::None ? 0 : 20));
    const QString label = text().toUpper();
    if (primaryMetrics.elidedText(label, Qt::ElideRight, textWidth) != label) {
        return false;
    }
    const QString detail = shownDetail();
    return QFontMetrics(secondaryFont(font())).elidedText(detail, Qt::ElideRight, content.width()) == detail;
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

void DeckLabActionButton::setDeck(int deck) {
    if (m_deck == deck) {
        return;
    }
    m_deck = deck;
    update();
}

void DeckLabActionButton::paintDeckTarget(QPainter& painter, const QColor& primary, const QColor& secondary) {
    // LOAD target: deck-colour bar and badge always (dimmed when disabled),
    // the action word, and what loading would do on the second line.
    const bool enabled = isEnabled();
    QColor deckColor = deckLabDeckColor(m_deck);
    if (!enabled) {
        deckColor.setAlpha(120);
    }
    const QRectF inner = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(Qt::NoPen);
    painter.setBrush(deckColor);
    painter.drawRect(QRectF(inner.left(), inner.top(), 6, inner.height()));
    const QFont primaryFont = strongFont(font());
    const QFont detailFont = secondaryFont(font());
    const QFontMetrics primaryMetrics(primaryFont);
    const QFontMetrics detailMetrics(detailFont);
    const int left = static_cast<int>(inner.left()) + 12;
    const int right = static_cast<int>(inner.right()) - 6;
    const bool twoLines = !m_detail.isEmpty() &&
            height() >= primaryMetrics.height() + detailMetrics.height();
    // The badge sits on the first line; the second line starts below it.
    const int lineTop = twoLines
            ? (height() - primaryMetrics.height() - detailMetrics.height() + 1) / 2
            : (height() - primaryMetrics.height()) / 2;
    const QRectF badge(left, lineTop + (primaryMetrics.height() - 20) / 2.0, 22, 20);
    painter.drawRoundedRect(badge, 3, 3);
    painter.setFont(numberFont(font(), 15));
    painter.setPen(colors::ground);
    painter.drawText(badge, Qt::AlignCenter, QString::number(m_deck));
    QString label = text();
    const QString suffix = QLatin1Char(' ') + QString::number(m_deck);
    if (label.endsWith(suffix)) {
        label.chop(suffix.size());
    }
    const int labelLeft = static_cast<int>(badge.right()) + 7;
    painter.setFont(primaryFont);
    painter.setPen(primary);
    painter.drawText(QRect(labelLeft, lineTop, std::max(0, right - labelLeft), primaryMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            primaryMetrics.elidedText(label.toUpper(), Qt::ElideRight, std::max(0, right - labelLeft)));
    if (twoLines) {
        painter.setFont(detailFont);
        painter.setPen(secondary);
        painter.drawText(QRect(left, lineTop + primaryMetrics.height() - 1, std::max(0, right - left), detailMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                detailMetrics.elidedText(m_detail, Qt::ElideRight, std::max(0, right - left)));
    }
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
    QColor primary = enabled ? colors::ink : colors::dim;
    QColor secondary = enabled ? colors::muted : colors::dim;
    const QRectF inner = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    if (m_deck > 0) {
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
        case Tone::Ready:
            // Ready to load: raised fill and full-contrast text, never red.
            painter.setPen(QPen(colors::edge, 1));
            painter.setBrush(colors::raise);
            painter.drawRoundedRect(inner, 2, 2);
            primary = colors::ink;
            secondary = colors::ink;
            break;
        default:
            break;
        }
        paintDeckTarget(painter, primary, secondary);
        return;
    }
    switch (m_tone) {
    case Tone::Selected:
        painter.setPen(QPen(colors::accent, 2));
        painter.setBrush(colors::raise);
        painter.drawRoundedRect(inner, 2, 2);
        secondary = enabled ? colors::ink : colors::dim;
        break;
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
        fill.setAlpha(30);
        painter.setPen(QPen(colors::keyMatch, 2));
        painter.setBrush(fill);
        painter.drawRoundedRect(inner, 2, 2);
        secondary = colors::keyMatch;
        break;
    }
    case Tone::Ready:
        // Ready to load: white edge (selection), never red.
        painter.setPen(Qt::NoPen);
        painter.setBrush(enabled ? colors::accent : colors::edge);
        painter.drawRect(QRectF(inner.left(), inner.top(), 3, inner.height()));
        break;
    case Tone::Neutral:
        break;
    }
    const QFont primaryFont = strongFont(font());
    const QFont detailFont = secondaryFont(font());
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
    const bool twoLines = !m_detail.isEmpty() && content.height() >= primaryMetrics.height() + detailMetrics.height() - 4;
    const int primaryTop = twoLines
            ? content.top() + (content.height() - primaryMetrics.height() - detailMetrics.height() + 4) / 2
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
                                 primaryTop + primaryMetrics.height() - 4,
                                 content.width(),
                                 detailMetrics.height()),
                Qt::AlignHCenter | Qt::AlignVCenter,
                detailMetrics.elidedText(shownDetail(), Qt::ElideRight, content.width()));
    }
}

DeckLabDeckChip::DeckLabDeckChip(QWidget* parent)
        : QPushButton(parent) {
    setFocusPolicy(Qt::NoFocus);
}

bool DeckLabDeckChip::textFits() const {
    if (m_deck <= 0) {
        return QFontMetrics(secondaryFont(font())).elidedText(m_detail, Qt::ElideRight, width() - 18) == m_detail;
    }
    const int left = 13;
    const QString title = (m_automatic ? tr("Deck %1 · auto").arg(m_deck) : tr("Deck %1").arg(m_deck)).toUpper();
    return QFontMetrics(labelFont(font())).elidedText(title, Qt::ElideRight, width() - left - 6) == title &&
            QFontMetrics(numberFont(font(), 16)).elidedText(m_detail, Qt::ElideRight, width() - left - 6) == m_detail;
}

void DeckLabDeckChip::setReference(int deck, bool automatic, const QString& detail) {
    if (m_deck == deck && m_automatic == automatic && m_detail == detail) {
        return;
    }
    m_deck = deck;
    m_automatic = automatic;
    m_detail = detail;
    setAccessibleName(deck > 0
                    ? (automatic ? tr("Match to deck %1 (automatic): %2").arg(deck).arg(detail)
                                 : tr("Match to deck %1: %2").arg(deck).arg(detail))
                    : detail);
    update();
}

void DeckLabDeckChip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 3.5, -0.5, -3.5);
    painter.setPen(QPen(colors::edge, 1));
    painter.setBrush(isDown() ? colors::raise : colors::panel);
    painter.drawRoundedRect(frame, 3, 3);
    const QFont label = labelFont(font());
    const QFont value = numberFont(font(), 16);
    const QFontMetrics labelMetrics(label);
    const QFontMetrics valueMetrics(value);
    const int top = (height() - labelMetrics.height() - valueMetrics.height() + 4) / 2;
    if (m_deck <= 0) {
        // No reference deck: neutral, never the "match" green.
        painter.setFont(secondaryFont(font()));
        painter.setPen(colors::muted);
        painter.drawText(rect().adjusted(10, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(secondaryFont(font())).elidedText(m_detail, Qt::ElideRight, width() - 18));
        return;
    }
    const QColor deckColor = deckLabDeckColor(m_deck);
    painter.setPen(Qt::NoPen);
    painter.setBrush(deckColor);
    painter.drawRoundedRect(QRectF(frame.left(), frame.top(), 6, frame.height()), 2, 2);
    const int left = 13;
    const QString title = m_automatic ? tr("Deck %1 · auto").arg(m_deck) : tr("Deck %1").arg(m_deck);
    painter.setFont(label);
    painter.setPen(deckColor);
    painter.drawText(QRect(left, top, width() - left - 6, labelMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
            labelMetrics.elidedText(title.toUpper(), Qt::ElideRight, width() - left - 6));
    painter.setFont(value);
    painter.setPen(colors::ink);
    painter.drawText(QRect(left, top + labelMetrics.height() - 4, width() - left - 6, valueMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            valueMetrics.elidedText(m_detail, Qt::ElideRight, width() - left - 6));
}

DeckLabTwoLineLabel::DeckLabTwoLineLabel(QWidget* parent)
        : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(44);
}

void DeckLabTwoLineLabel::setLines(const QString& primary, const QString& secondary, bool active) {
    if (m_primary == primary && m_secondary == secondary && m_active == active) {
        return;
    }
    m_primary = primary;
    m_secondary = secondary;
    m_active = active;
    setAccessibleName(secondary.isEmpty() ? primary : primary + QStringLiteral(", ") + secondary);
    update();
}

bool DeckLabTwoLineLabel::textFits() const {
    const QFontMetrics valueMetrics(m_active ? numberFont(font(), 16) : secondaryFont(font()));
    const int width = std::max(0, this->width() - 8);
    if (valueMetrics.elidedText(m_primary, Qt::ElideRight, width) != m_primary) {
        return false;
    }
    return m_secondary.isEmpty() ||
            QFontMetrics(labelFont(font())).elidedText(m_secondary, Qt::ElideRight, width) == m_secondary;
}

void DeckLabTwoLineLabel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QFont value = m_active ? numberFont(font(), 16) : secondaryFont(font());
    const QFont label = labelFont(font());
    const QFontMetrics valueMetrics(value);
    const QFontMetrics labelMetrics(label);
    const int width = std::max(0, this->width() - 8);
    if (m_secondary.isEmpty()) {
        painter.setFont(value);
        painter.setPen(m_active ? colors::ink : colors::muted);
        painter.drawText(QRect(4, 0, width, height()), Qt::AlignLeft | Qt::AlignVCenter,
                valueMetrics.elidedText(m_primary, Qt::ElideRight, width));
        return;
    }
    const int top = (height() - valueMetrics.height() - labelMetrics.height() + 4) / 2;
    painter.setFont(value);
    painter.setPen(m_active ? colors::ink : colors::muted);
    painter.drawText(QRect(4, top, width, valueMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
            valueMetrics.elidedText(m_primary, Qt::ElideRight, width));
    painter.setFont(label);
    painter.setPen(colors::muted);
    painter.drawText(QRect(4, top + valueMetrics.height() - 4, width, labelMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            labelMetrics.elidedText(m_secondary, Qt::ElideRight, width));
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
    // Path segments use the regular font; reserve the extra width of the
    // bold deepest level, which is always shown.
    const QFontMetrics pathMetrics(secondaryFont(font()));
    const QFontMetrics lastMetrics(titleFont(font()));
    const QString last = m_segments.isEmpty() ? QString() : m_segments.last();
    const int boldExtra = std::max(0, lastMetrics.horizontalAdvance(last) - pathMetrics.horizontalAdvance(last));
    return elideBreadcrumb(m_segments,
            std::max(0, width() - 20 - boldExtra),
            [&pathMetrics](const QString& text) {
                return pathMetrics.horizontalAdvance(text);
            },
            QStringLiteral(" › "));
}

void DeckLabBreadcrumb::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QStringList shown = displayedSegments();
    const QFont lastFont = titleFont(font());
    const QFont pathFont = secondaryFont(font());
    const QString separator = QStringLiteral(" › ");
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
            painter.setPen(colors::muted);
            painter.drawText(QRect(x, 0, available - x, height()), Qt::AlignLeft | Qt::AlignVCenter, separator);
            x += separatorMetrics.horizontalAdvance(separator);
        }
        if (x >= available) {
            break;
        }
    }
}

DeckLabTrackRow deckLabTrackRow(const TrackModel& tracks, const QModelIndex& index) {
    DeckLabTrackRow row;
    if (!index.isValid()) {
        return row;
    }
    const auto cell = [&tracks, &index](TrackModel::SortColumnId id, int role) {
        const int column = tracks.columnIndexFromSortColumnId(id);
        return column >= 0 ? index.sibling(index.row(), column).data(role) : QVariant();
    };
    using Column = TrackModel::SortColumnId;
    row.location = tracks.getTrackLocation(index);
    row.title = cell(Column::Title, Qt::DisplayRole).toString();
    row.artist = cell(Column::Artist, Qt::DisplayRole).toString();
    row.album = cell(Column::Album, Qt::DisplayRole).toString();
    row.genre = cell(Column::Genre, Qt::DisplayRole).toString();
    row.comment = cell(Column::Comment, Qt::DisplayRole).toString();
    row.keyText = cell(Column::Key, Qt::DisplayRole).toString();
    // Mixxx's tables give the stored values for editing: BPM, seconds, the
    // star count (a StarRating) and the color's RGB value.
    bool ok = false;
    const double bpm = cell(Column::Bpm, Qt::EditRole).toDouble(&ok);
    row.bpm = ok && std::isfinite(bpm) && bpm > 0 ? bpm : 0;
    const double duration = cell(Column::Duration, Qt::EditRole).toDouble(&ok);
    row.duration = ok && std::isfinite(duration) && duration > 0 ? duration : 0;
    const QVariant rating = cell(Column::Rating, Qt::EditRole);
    row.rating = rating.canConvert<StarRating>() ? rating.value<StarRating>().starCount() : rating.toInt();
    row.color = mixxx::RgbColor::toQColor(mixxx::RgbColor::fromQVariant(cell(Column::Color, Qt::EditRole)));
    return row;
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

void DeckLabTrackCard::showTrack(const TrackPointer& track, const QString& storedKeyText) {
    if (!track) {
        showItem(Glyph::Tracks, tr("Pick a track"), QString(), QString());
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
    // r6-keys: a key Mixxx has not read stays as its source stores it: the
    // track's own key text, else its row's ("8A" from a rekordbox export, a
    // USB file's tag). Painted through displayKey(): the chosen key format
    // applies, a change of format is a repaint, and text that is not a key
    // shows as stored, never as "—".
    m_keyText.clear();
    if (m_key == mixxx::track::io::key::INVALID) {
        m_keyText = track->getKeys().getGlobalKeyText().trimmed();
        if (m_keyText.isEmpty()) {
            m_keyText = storedKeyText.trimmed();
        }
        if (!m_keyText.isEmpty()) {
            m_key = keyFromText(m_keyText);
        }
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

void DeckLabTrackCard::showRow(const DeckLabTrackRow& row) {
    m_itemMode = false;
    m_track.reset();
    m_location = row.location;
    m_title = row.title.trimmed();
    if (m_title.isEmpty()) {
        m_title = QFileInfo(m_location).completeBaseName();
    }
    m_artist = row.artist.trimmed();
    m_album = row.album.trimmed();
    m_genre = row.genre.trimmed();
    m_comment = row.comment.simplified();
    // r6-keys: as stored, painted through displayKey().
    m_keyText = row.keyText.trimmed();
    m_key = m_keyText.isEmpty() ? mixxx::track::io::key::INVALID : keyFromText(m_keyText);
    m_bpm = row.bpm;
    m_duration = row.duration;
    m_rating = row.rating;
    m_color = row.color;
    m_cover = QPixmap();
    m_coverScaled = QPixmap();
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

QString DeckLabTrackCard::keyText() const {
    return m_itemMode ? QString() : displayKey(m_key, m_keyText);
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
    // The reference deck is named on the Match bar; keep this short and say
    // it in BPM: "Same key · ▼ 2.0 BPM slower".
    QStringList parts;
    const QString relation = keyRelationText(m_key, reference.key);
    if (!relation.isEmpty()) {
        parts << relation;
    }
    if (std::isfinite(m_bpm) && m_bpm > 0 && reference.bpm > 0) {
        const double delta = m_bpm - reference.bpm;
        if (std::abs(delta) < 0.05) {
            parts << tr("same BPM");
        } else {
            const QString size = QLocale().toString(std::abs(delta), 'f', 1);
            parts << (delta > 0 ? tr("▲ %1 BPM faster").arg(size) : tr("▼ %1 BPM slower").arg(size));
        }
    }
    if (parts.isEmpty()) {
        return {};
    }
    return parts.join(QStringLiteral(" · "));
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
    painter.fillRect(rect(), colors::ground);
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
    const QFont trackTitleFont = titleFont(font());
    const QFont artistFont = secondaryFont(font());
    const QFont smallFont = secondaryFont(font());
    int y = drawWrapped(painter,
            QRect(textLeft, pad - 2, textWidth, art - 18),
            m_title,
            trackTitleFont,
            colors::ink,
            compact ? 2 : 3);
    drawWrapped(painter,
            QRect(textLeft, y + 1, textWidth, artBox.bottom() - y),
            m_artist.isEmpty() ? tr("Unknown artist") : m_artist,
            artistFont,
            colors::muted,
            2);

    const MatchReference& reference = matchReference();
    const HarmonicMatch harmonic = reference.active()
            ? harmonicMatch(m_key, reference.key)
            : HarmonicMatch::None;
    const bool tempo = reference.active() && tempoInWindow(m_bpm, reference.window());
    const QString key = keyText(); // r6-keys
    const int chipTop = artBox.bottom() + 9;
    const int chipHeight = compact ? 44 : 50;
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
            key.isEmpty() ? QStringLiteral("—") : key,
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
    // Rating and track color, then the relation to the reference deck.
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
        y += 20;
    }
    const QString summary = matchSummary();
    const QFont summaryFont = strongFont(font());
    const QFontMetrics summaryMetrics(summaryFont);
    if (!summary.isEmpty() && y + summaryMetrics.height() <= bottom) {
        painter.setFont(summaryFont);
        painter.setPen(harmonic != HarmonicMatch::None || tempo ? colors::keyMatch : colors::muted);
        painter.drawText(QRect(pad, y, width() - 2 * pad, summaryMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                summaryMetrics.elidedText(summary, Qt::ElideRight, width() - 2 * pad));
        y += summaryMetrics.height() + 4;
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
        drawWrapped(painter,
                QRect(pad, y, width() - 2 * pad, bottom - y),
                m_comment,
                smallFont,
                colors::muted,
                2);
    }
}

void DeckLabTrackCard::paintItem(QPainter& painter) {
    const int pad = 12;
    const int tile = std::clamp(height() / 3, 48, 84);
    const QRect tileBox(pad, pad, tile, tile);
    QColor base = m_accent.isValid() ? m_accent : colors::raise;
    QLinearGradient gradient(tileBox.topLeft(), tileBox.bottomRight());
    gradient.setColorAt(0, m_accent.isValid() ? base.darker(160) : colors::raise);
    gradient.setColorAt(1, colors::ground);
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
            titleFont(font()),
            colors::ink,
            3);
    drawWrapped(painter,
            QRect(textLeft, y + 2, textWidth, std::max(tile + pad - y, 34)),
            m_subtitle,
            secondaryFont(font()),
            colors::muted,
            2);
    if (!m_hint.isEmpty()) {
        const QFont hintFont = secondaryFont(font());
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
                    colors::muted,
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
    m_timer.setSingleShot(true);
    QObject::connect(&m_timer, &QTimer::timeout, this, &QWidget::hide);
    hide();
    // It can cross the waveforms (Browse strip, or Decks for a controller
    // load): a native window above the GL waveform windows. Touches pass
    // through to the waveform beneath.
    decklab::overlay::keepAboveWaveforms(this);
}

void DeckLabToast::showMessage(const QString& text, int milliseconds, bool live, const QRect& anchor) {
    setFont(strongFont(font()));
    setStyleSheet(QStringLiteral(
            "QLabel#decklab-toast{background:#20242B;color:#F2F4F7;border:2px solid %1;"
            "border-radius:4px;padding:10px 18px;font-size:16px;font-weight:600;}")
                          .arg(live ? QStringLiteral("#FF3D2E") : QStringLiteral("#F2F4F7")));
    setText(text);
    setAccessibleName(text);
    adjustSize();
    QWidget* surface = parentWidget();
    if (surface) {
        const int width = std::min(this->width(), surface->width() - 32);
        resize(width, height());
        if (anchor.isValid()) {
            // r4-browse (B-33): next to what refused, 8 px above it.
            const int x = std::clamp(anchor.center().x() - width / 2, 16, std::max(16, surface->width() - 16 - width));
            const int y = std::clamp(anchor.top() - 8 - height(), 8, std::max(8, surface->height() - 8 - height()));
            move(x, y);
        } else {
            move((surface->width() - width) / 2, 96);
        }
    }
    decklab::overlay::raiseAboveWaveforms(this);
    show();
    m_timer.start(milliseconds);
}

DeckLabListDelegate::DeckLabListDelegate(QObject* parent)
        : QStyledItemDelegate(parent) {
}

QSize DeckLabListDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    // Source rows say what is inside on a second line.
    const bool twoLines = !m_glyphProvider && !index.data(SubtitleRole).toString().isEmpty();
    return QSize(option.rect.width(), twoLines ? 60 : 44);
}

void DeckLabListDelegate::paint(QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    // Line separators on the ground (no zebra stripe); selection is a raised
    // fill with a white edge.
    painter->fillRect(r, selected ? colors::raise : colors::ground);
    painter->setPen(colors::panel.lighter(115));
    painter->drawLine(r.bottomLeft(), r.bottomRight());
    if (selected) {
        painter->fillRect(QRect(r.left(), r.top(), 3, r.height()), colors::accent);
    }
    int x = r.left() + 16;
    const QColor textColor = enabled ? colors::ink : colors::dim;
    const bool sources = static_cast<bool>(m_glyphProvider);
    const QString subtitle = sources ? QString() : index.data(SubtitleRole).toString();
    const auto glyph = sources ? m_glyphProvider(index) : static_cast<Glyph>(index.data(GlyphRole).toInt());
    if (!subtitle.isEmpty()) {
        // Two-line source row: a 24 px glyph in text colour, the name and
        // what is inside.
        if (glyph != Glyph::None) {
            paintGlyph(*painter, QRectF(x, r.center().y() - 12, 24, 24), glyph, enabled ? colors::ink : colors::dim);
            x += 40;
        }
        const bool chevron = index.data(ChevronRole).toBool();
        const int right = r.right() - (chevron ? 30 : 14);
        if (chevron) {
            paintGlyph(*painter, QRectF(r.right() - 26, r.center().y() - 7, 14, 14), Glyph::Chevron, colors::muted);
        }
        const QFont nameFont = strongFont(option.font);
        const QFont lineFont = secondaryFont(option.font);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics lineMetrics(lineFont);
        const int top = r.top() + (r.height() - nameMetrics.height() - lineMetrics.height()) / 2;
        const int width = std::max(0, right - x);
        painter->setFont(nameFont);
        painter->setPen(textColor);
        painter->drawText(QRect(x, top, width, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                nameMetrics.elidedText(index.data().toString(), Qt::ElideRight, width));
        painter->setFont(lineFont);
        painter->setPen(enabled ? colors::muted : colors::dim);
        painter->drawText(QRect(x, top + nameMetrics.height(), width, lineMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                lineMetrics.elidedText(subtitle, Qt::ElideRight, width));
        painter->restore();
        return;
    }
    if (glyph != Glyph::None) {
        paintGlyph(*painter, QRectF(x, r.center().y() - 10, 20, 20), glyph, selected ? colors::ink : colors::muted);
        x += 34;
    }
    const QColor accent = sources ? QColor() : index.data(AccentRole).value<QColor>();
    if (accent.isValid()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(accent);
        painter->drawRoundedRect(QRectF(x, r.center().y() - 9, 6, 18), 2, 2);
        x += 16;
    }
    const bool chevron = sources || index.data(ChevronRole).toBool();
    int right = r.right() - (chevron ? 30 : 14);
    if (chevron) {
        paintGlyph(*painter,
                QRectF(r.right() - 26, r.center().y() - 7, 14, 14),
                Glyph::Chevron,
                colors::muted);
    }
    const QString count = sources ? QString() : index.data(CountRole).toString();
    const QFont countFont = numberFont(option.font, 15);
    if (!count.isEmpty()) {
        const QFontMetrics metrics(countFont);
        const int countWidth = metrics.horizontalAdvance(count) + 4;
        painter->setFont(countFont);
        painter->setPen(colors::muted);
        painter->drawText(QRect(right - countWidth, r.top(), countWidth, r.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                count);
        right -= countWidth + 12;
    }
    const QFont rowFont = selected ? strongFont(option.font) : bodyFont(option.font);
    const QFontMetrics labelMetrics(rowFont);
    painter->setFont(rowFont);
    painter->setPen(textColor);
    painter->drawText(QRect(x, r.top(), std::max(0, right - x), r.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            labelMetrics.elidedText(index.data().toString(), Qt::ElideRight, std::max(0, right - x)));
    painter->restore();
}
