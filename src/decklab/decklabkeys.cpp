// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabkeys.h"

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <array>
#include <atomic>

#include "control/controlobject.h"
#include "library/library_prefs.h"
#include "track/keyutils.h"

namespace decklab {

using mixxx::track::io::key::ChromaticKey;

namespace {

constexpr int kKeys = 25; // INVALID and the 24 keys
constexpr int kLongestKeyText = 32;

std::atomic<int> s_format{static_cast<int>(KeyFormat::Lancelot)};

bool validKey(ChromaticKey key) {
    return key > mixxx::track::io::key::INVALID && key <= mixxx::track::io::key::B_MINOR;
}

int formatIndex(KeyFormat format) {
    const int index = static_cast<int>(format);
    return index >= 0 && index < 3 ? index : 0;
}

// One name per key and the sharps and flats DJ software uses: a single name
// for F♯ major and E♭ minor (Mixxx's "F♯/G♭" and "D♯m/E♭m" do not fit a row).
const std::array<QString, kKeys>& musicalNames() {
    static const std::array<QString, kKeys> names{QString(),
            QStringLiteral(u"C"),
            QStringLiteral(u"D♭"),
            QStringLiteral(u"D"),
            QStringLiteral(u"E♭"),
            QStringLiteral(u"E"),
            QStringLiteral(u"F"),
            QStringLiteral(u"F♯"),
            QStringLiteral(u"G"),
            QStringLiteral(u"A♭"),
            QStringLiteral(u"A"),
            QStringLiteral(u"B♭"),
            QStringLiteral(u"B"),
            QStringLiteral(u"Cm"),
            QStringLiteral(u"C♯m"),
            QStringLiteral(u"Dm"),
            QStringLiteral(u"E♭m"),
            QStringLiteral(u"Em"),
            QStringLiteral(u"Fm"),
            QStringLiteral(u"F♯m"),
            QStringLiteral(u"Gm"),
            QStringLiteral(u"G♯m"),
            QStringLiteral(u"Am"),
            QStringLiteral(u"B♭m"),
            QStringLiteral(u"Bm")};
    return names;
}

const std::array<std::array<QString, kKeys>, 3>& nameTables() {
    static const std::array<std::array<QString, kKeys>, 3> tables = [] {
        std::array<std::array<QString, kKeys>, 3> built;
        for (int value = 1; value < kKeys; ++value) {
            const auto key = static_cast<ChromaticKey>(value);
            built[0][value] = KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot);
            built[1][value] = KeyUtils::keyToString(key, KeyUtils::KeyNotation::OpenKey);
        }
        built[2] = musicalNames();
        return built;
    }();
    return tables;
}

// "8A", "08a", "12 B" (Lancelot) or "1m", "12d" (Open Key). Text that starts
// with a digit and is neither is not read: Mixxx would take "8D" for an
// advanced Lancelot mode, a DJ for Open Key; we do not guess.
ChromaticKey parseWheel(const QString& text) {
    const int size = static_cast<int>(text.size());
    int pos = 0;
    while (pos < size && text.at(pos) == QLatin1Char('0')) {
        ++pos;
    }
    int number = 0;
    int digits = 0;
    while (pos < size && text.at(pos) >= QLatin1Char('0') && text.at(pos) <= QLatin1Char('9')) {
        number = number * 10 + (text.at(pos).unicode() - '0');
        ++pos;
        if (++digits > 2) {
            return mixxx::track::io::key::INVALID;
        }
    }
    if (digits == 0 || number < 1 || number > 12) {
        return mixxx::track::io::key::INVALID;
    }
    if (pos < size && text.at(pos) == QLatin1Char(' ')) {
        ++pos;
    }
    if (pos != size - 1) {
        return mixxx::track::io::key::INVALID;
    }
    const QChar letter = text.at(pos);
    if (letter == QLatin1Char('A') || letter == QLatin1Char('a') ||
            letter == QLatin1Char('B') || letter == QLatin1Char('b')) {
        // Lancelot is Open Key turned five steps: 8A is 1m (A minor).
        const int openKey = number + 5 > 12 ? number - 7 : number + 5;
        const bool major = letter == QLatin1Char('B') || letter == QLatin1Char('b');
        return KeyUtils::openKeyNumberToKey(openKey, major);
    }
    if (letter == QLatin1Char('m') || letter == QLatin1Char('d')) {
        return KeyUtils::openKeyNumberToKey(number, letter == QLatin1Char('d'));
    }
    return mixxx::track::io::key::INVALID;
}

ChromaticKey parseSingle(const QString& text) {
    if (text.isEmpty() || text.size() > kLongestKeyText) {
        return mixxx::track::io::key::INVALID;
    }
    const QChar first = text.at(0);
    if (first >= QLatin1Char('0') && first <= QLatin1Char('9')) {
        return parseWheel(text);
    }
    // Musical names: Mixxx's own reader (sharps, flats, ♯/♭, m/min/minor,
    // maj/major, lower case for minor).
    const QChar letter = first.toUpper();
    if (letter >= QLatin1Char('A') && letter <= QLatin1Char('G')) {
        return KeyUtils::guessKeyFromText(text);
    }
    return mixxx::track::io::key::INVALID;
}

} // namespace

KeyFormat keyFormat() {
    return static_cast<KeyFormat>(s_format.load(std::memory_order_relaxed));
}

void setKeyFormat(KeyFormat format) {
    s_format.store(formatIndex(format), std::memory_order_relaxed);
}

void applyKeyFormat(KeyFormat format) {
    setKeyFormat(format);
    KeyUtils::setNotation(keyNotationMap(format));
    // Mixxx's tables repaint and re-sort keys on this control.
    if (ControlObject* notation = ControlObject::getControl(
                mixxx::library::prefs::kKeyNotationConfigKey, ControlFlag::NoWarnIfMissing)) {
        notation->set(keyFormatNotation(format));
    }
}

KeyFormat keyFormatFromConfig(const QString& value) {
    if (value == QStringLiteral("OpenKey") || value == QStringLiteral("OpenKeyAndTraditional")) {
        return KeyFormat::OpenKey;
    }
    if (value == QStringLiteral("Traditional")) {
        return KeyFormat::Musical;
    }
    return KeyFormat::Lancelot;
}

QString keyFormatConfigValue(KeyFormat format) {
    switch (format) {
    case KeyFormat::OpenKey:
        return QStringLiteral("OpenKey");
    case KeyFormat::Musical:
        return QStringLiteral("Traditional");
    case KeyFormat::Lancelot:
        break;
    }
    return QStringLiteral("Lancelot");
}

int keyFormatNotation(KeyFormat format) {
    switch (format) {
    case KeyFormat::OpenKey:
        return static_cast<int>(KeyUtils::KeyNotation::OpenKey);
    case KeyFormat::Musical:
        return static_cast<int>(KeyUtils::KeyNotation::Traditional);
    case KeyFormat::Lancelot:
        break;
    }
    return static_cast<int>(KeyUtils::KeyNotation::Lancelot);
}

bool keyFormatFromNotation(int notation, KeyFormat* format) {
    switch (static_cast<KeyUtils::KeyNotation>(notation)) {
    case KeyUtils::KeyNotation::Lancelot:
    case KeyUtils::KeyNotation::LancelotAndTraditional:
        *format = KeyFormat::Lancelot;
        return true;
    case KeyUtils::KeyNotation::OpenKey:
    case KeyUtils::KeyNotation::OpenKeyAndTraditional:
        *format = KeyFormat::OpenKey;
        return true;
    case KeyUtils::KeyNotation::Traditional:
        *format = KeyFormat::Musical;
        return true;
    default:
        return false;
    }
}

QString keyFormatName(KeyFormat format) {
    switch (format) {
    case KeyFormat::OpenKey:
        return QCoreApplication::translate("DeckLabKeys", "Open Key (1m)");
    case KeyFormat::Musical:
        return QCoreApplication::translate("DeckLabKeys", "Musical (Am)");
    case KeyFormat::Lancelot:
        break;
    }
    return QCoreApplication::translate("DeckLabKeys", "Lancelot (8A)");
}

QString keyFormatShortName(KeyFormat format) {
    switch (format) {
    case KeyFormat::OpenKey:
        return QCoreApplication::translate("DeckLabKeys", "Open Key");
    case KeyFormat::Musical:
        return QCoreApplication::translate("DeckLabKeys", "Musical");
    case KeyFormat::Lancelot:
        break;
    }
    return QCoreApplication::translate("DeckLabKeys", "Lancelot");
}

ChromaticKey parseKeyText(const QString& raw) {
    const QString text = raw.trimmed();
    if (text.isEmpty() || text.size() > kLongestKeyText) {
        return mixxx::track::io::key::INVALID;
    }
    const ChromaticKey whole = parseSingle(text);
    if (validKey(whole)) {
        return whole;
    }
    // Combined notations ("8A (Am)", "1m / Am", Mixxx's "F♯/G♭"): every part
    // must be a key and all of them the same key.
    static const QRegularExpression separators(QStringLiteral("[\\s/(),|]+"));
    const QStringList parts = text.split(separators, Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        return mixxx::track::io::key::INVALID;
    }
    ChromaticKey agreed = mixxx::track::io::key::INVALID;
    for (const QString& part : parts) {
        const ChromaticKey key = parseSingle(part);
        if (!validKey(key) || (validKey(agreed) && key != agreed)) {
            return mixxx::track::io::key::INVALID;
        }
        agreed = key;
    }
    return agreed;
}

ChromaticKey parseKey(const QString& text) {
    // Few distinct key texts exist in any library; each is read once.
    static QHash<QString, ChromaticKey> cache;
    const auto found = cache.constFind(text);
    if (found != cache.constEnd()) {
        return found.value();
    }
    if (cache.size() > 2048) {
        cache.clear();
    }
    const ChromaticKey key = parseKeyText(text);
    cache.insert(text, key);
    return key;
}

const QString& formatKey(ChromaticKey key) {
    return formatKey(key, keyFormat());
}

const QString& formatKey(ChromaticKey key, KeyFormat format) {
    static const QString none;
    if (!validKey(key)) {
        return none;
    }
    return nameTables()[formatIndex(format)][static_cast<int>(key)];
}

QString displayKey(const QString& text) {
    if (text.isEmpty()) {
        return text;
    }
    const ChromaticKey key = parseKey(text);
    return validKey(key) ? formatKey(key) : text;
}

QString displayKey(ChromaticKey key, const QString& text) {
    return validKey(key) ? formatKey(key) : displayKey(text);
}

int keyWheelOrder(ChromaticKey key) {
    return keyWheelOrder(key, keyFormat());
}

int keyWheelOrder(ChromaticKey key, KeyFormat format) {
    if (!validKey(key)) {
        return 0;
    }
    return KeyUtils::keyToCircleOfFifthsOrder(key,
            format == KeyFormat::Lancelot ? KeyUtils::KeyNotation::Lancelot
                                          : KeyUtils::KeyNotation::OpenKey);
}

QMap<ChromaticKey, QString> keyNotationMap(KeyFormat format) {
    QMap<ChromaticKey, QString> map;
    for (int value = 1; value < kKeys; ++value) {
        const auto key = static_cast<ChromaticKey>(value);
        map.insert(key, formatKey(key, format));
    }
    return map;
}

} // namespace decklab
