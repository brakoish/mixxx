// SPDX-License-Identifier: GPL-2.0-or-later
#include "controllers/controllermappingmatcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <utility>

namespace {

// Compiled-in overrides for controllers whose port names do not resemble
// their mapping. Entries match by case-insensitive port-name regex or by USB
// "vid:pid" (as reported by decklabd). Mapping is a file name in either the
// user or system controller directory.
const char kBuiltinOverrides[] = R"JSON({
  "version": 1,
  "overrides": [
    {"name": "^(pioneer )?ddj-flx4\\b", "mapping": "Pioneer-DDJ-FLX4.midi.xml"},
    {"name": "^(pioneer )?ddj-400\\b", "mapping": "Pioneer-DDJ-400.midi.xml"},
    {"name": "^(pioneer )?ddj-sb2\\b", "mapping": "Pioneer-DDJ-SB2.midi.xml"},
    {"name": "^(pioneer )?ddj-sb3\\b", "mapping": "Pioneer-DDJ-SB3.midi.xml"},
    {"name": "^(numark )?mix ?track pro 3\\b", "mapping": "Numark-Mixtrack-3.midi.xml"},
    {"usb": "15e4:0035", "mapping": "Numark-Mixtrack-3.midi.xml"},
    {"name": "^(numark )?mix ?track platinum fx\\b", "mapping": "Numark Mixtrack Platinum FX.midi.xml"},
    {"name": "^(numark )?party ?mix\\b", "mapping": "Numark-Party-Mix.midi.xml"},
    {"name": "^(hercules )?dj ?control inpulse 500(?!\\s*mk)\\b", "mapping": "Hercules_DJControl_Inpulse_500.midi.xml"},
    {"name": "^(denon dj |denon )?mc7000\\b", "mapping": "Denon-MC7000.midi.xml"},
    {"name": "^kaoss dj\\b", "mapping": "Korg-KAOSS-DJ.midi.xml"},
    {"name": "^xone:k2\\b", "mapping": "Allen and Heath Xone K2.midi.xml"}
  ]
})JSON";

const QSet<QString>& brandTokens() {
    static const QSet<QString> tokens{QStringLiteral("pioneer"),
            QStringLiteral("pioneerdj"),
            QStringLiteral("alphatheta"),
            QStringLiteral("numark"),
            QStringLiteral("hercules"),
            QStringLiteral("denon"),
            QStringLiteral("denondj"),
            QStringLiteral("native"),
            QStringLiteral("instruments"),
            QStringLiteral("ni"),
            QStringLiteral("reloop"),
            QStringLiteral("roland"),
            QStringLiteral("behringer"),
            QStringLiteral("allen"),
            QStringLiteral("heath"),
            QStringLiteral("rane"),
            QStringLiteral("korg"),
            QStringLiteral("akai"),
            QStringLiteral("akaipro"),
            QStringLiteral("novation"),
            QStringLiteral("vestax"),
            QStringLiteral("stanton"),
            QStringLiteral("gemini"),
            QStringLiteral("mixars"),
            QStringLiteral("american"),
            QStringLiteral("audio"),
            QStringLiteral("dj"),
            QStringLiteral("djtechtools"),
            QStringLiteral("djtech"),
            QStringLiteral("tech"),
            QStringLiteral("m"),
            QStringLiteral("maudio"),
            QStringLiteral("ion"),
            QStringLiteral("electrix"),
            QStringLiteral("evolution"),
            QStringLiteral("mixvibes"),
            QStringLiteral("mixman"),
            QStringLiteral("yaeltex"),
            QStringLiteral("arturia"),
            QStringLiteral("icon"),
            QStringLiteral("soundless"),
            QStringLiteral("intech"),
            QStringLiteral("guillemot"),
            QStringLiteral("technics")};
    return tokens;
}

const QSet<QString>& noiseTokens() {
    static const QSet<QString> tokens{QStringLiteral("midi"),
            QStringLiteral("usb"),
            QStringLiteral("controller"),
            QStringLiteral("port"),
            QStringLiteral("in"),
            QStringLiteral("out"),
            QStringLiteral("input"),
            QStringLiteral("output"),
            QStringLiteral("device"),
            QStringLiteral("hid"),
            QStringLiteral("the"),
            QStringLiteral("decklab"),
            QStringLiteral("pioneered"),
            QStringLiteral("custom"),
            QStringLiteral("advanced"),
            QStringLiteral("mapping"),
            QStringLiteral("edition"),
            QStringLiteral("mod"),
            QStringLiteral("modified"),
            QStringLiteral("default"),
            QStringLiteral("for"),
            QStringLiteral("with"),
            QStringLiteral("and"),
            QStringLiteral("mixxx")};
    return tokens;
}

// Shared words that do not identify a model on their own.
const QSet<QString>& genericTokens() {
    static const QSet<QString> tokens{QStringLiteral("pro"),
            QStringLiteral("mix"),
            QStringLiteral("fx"),
            QStringLiteral("control"),
            QStringLiteral("djcontrol"),
            QStringLiteral("mini"),
            QStringLiteral("plus"),
            QStringLiteral("touch"),
            QStringLiteral("live"),
            QStringLiteral("digital"),
            QStringLiteral("studio"),
            QStringLiteral("one"),
            QStringLiteral("two"),
            QStringLiteral("ii"),
            QStringLiteral("deck"),
            QStringLiteral("decks"),
            QStringLiteral("jockey"),
            QStringLiteral("console"),
            QStringLiteral("kontrol")};
    return tokens;
}

struct NameModel {
    QStringList required;
    QStringList optional;
    QStringList brands;
};

QString stripPortDecoration(QString name) {
    name = name.trimmed();
    static const QRegularExpression alsaAddress(QStringLiteral("\\s+\\d+:\\d+\\s*$"));
    name.remove(alsaAddress);
    const int colon = name.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        const QString client = name.left(colon).trimmed();
        const QString port = name.mid(colon + 1).trimmed();
        if (!client.isEmpty() && port.startsWith(client, Qt::CaseInsensitive)) {
            name = port;
        }
    }
    static const QRegularExpression midiSuffix(QStringLiteral("\\s+midi\\s*\\d*\\s*$"),
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression portSuffix(QStringLiteral("\\s+port[- ]?\\d+\\s*$"),
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression directionSuffix(
            QStringLiteral("\\s+(in|out)(put)?\\s*\\d*\\s*$"),
            QRegularExpression::CaseInsensitiveOption);
    name.remove(midiSuffix);
    name.remove(portSuffix);
    name.remove(directionSuffix);
    return name.trimmed();
}

QString normalizeText(QString text) {
    text = text.toLower();
    static const QRegularExpression extension(QStringLiteral("\\.(midi|hid|bulk)\\.xml$"));
    static const QRegularExpression xml(QStringLiteral("\\.xml$"));
    text.remove(extension);
    text.remove(xml);
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    static const QRegularExpression mk3(QStringLiteral("\\bmk\\s*-?\\s*iii\\b"));
    static const QRegularExpression mk2(QStringLiteral("\\bmk\\s*-?\\s*ii\\b"));
    static const QRegularExpression mkNumber(QStringLiteral("\\bmk\\s*-?\\s*(\\d)\\b"));
    text.replace(mk3, QStringLiteral("mk3"));
    text.replace(mk2, QStringLiteral("mk2"));
    text.replace(mkNumber, QStringLiteral("mk\\1"));
    return text;
}

QStringList tokensOf(const QString& text) {
    static const QRegularExpression separators(QStringLiteral("[^a-z0-9]+"));
    return text.split(separators, Qt::SkipEmptyParts);
}

NameModel modelOf(const QString& text, bool device) {
    QString normalized = normalizeText(device ? stripPortDecoration(text) : text);
    QStringList optional;
    static const QRegularExpression parenthesis(QStringLiteral("\\(([^)]*)\\)"));
    auto matches = parenthesis.globalMatch(normalized);
    while (matches.hasNext()) {
        optional += tokensOf(matches.next().captured(1));
    }
    normalized.remove(parenthesis);
    NameModel model;
    for (const QString& token : tokensOf(normalized)) {
        if (brandTokens().contains(token)) {
            model.brands.append(token);
        } else if (!noiseTokens().contains(token)) {
            model.required.append(token);
        }
    }
    for (const QString& token : std::as_const(optional)) {
        if (!brandTokens().contains(token) && !noiseTokens().contains(token)) {
            model.optional.append(token);
        }
    }
    return model;
}

bool hasDigit(const QString& token) {
    return std::any_of(token.cbegin(), token.cend(), [](QChar c) { return c.isDigit(); });
}

bool isMk(const QString& token) {
    static const QRegularExpression mk(QStringLiteral("^mk\\d$"));
    return mk.match(token).hasMatch();
}

// Exact token sets score 95-100; every unexplained token costs confidence and
// any unexplained model number (digit token other than mkN) rejects outright.
int scoreModels(const NameModel& device, const NameModel& candidate, int penalty) {
    if (device.required.isEmpty() || candidate.required.isEmpty()) {
        return 0;
    }
    const QString deviceCompact = device.required.join(QString());
    const QString candidateCompact = candidate.required.join(QString());
    if (deviceCompact == candidateCompact) {
        return 100 - penalty;
    }
    const auto contains = [](const QStringList& list, const QStringList& extra, const QString& compact,
                                  const QString& token) {
        return list.contains(token) || extra.contains(token) ||
                (token.size() >= 3 && compact.contains(token));
    };
    QStringList missing;
    QStringList extra;
    QStringList shared;
    for (const QString& token : candidate.required) {
        if (!contains(device.required, device.optional, deviceCompact, token)) {
            missing.append(token);
        }
    }
    for (const QString& token : device.required) {
        if (contains(candidate.required, candidate.optional, candidateCompact, token)) {
            shared.append(token);
        } else {
            extra.append(token);
        }
    }
    const bool distinctive = std::any_of(shared.cbegin(), shared.cend(), [](const QString& token) {
        return (hasDigit(token) && !isMk(token)) ||
                (token.size() >= 3 && !genericTokens().contains(token));
    });
    if (!distinctive) {
        return 0;
    }
    QStringList deviceMk;
    QStringList candidateMk;
    for (const QString& token : device.required) {
        if (isMk(token)) {
            deviceMk.append(token);
        }
    }
    for (const QString& token : candidate.required) {
        if (isMk(token)) {
            candidateMk.append(token);
        }
    }
    if (!deviceMk.isEmpty() && !candidateMk.isEmpty() &&
            QSet<QString>(deviceMk.cbegin(), deviceMk.cend()) !=
                    QSet<QString>(candidateMk.cbegin(), candidateMk.cend())) {
        return 0;
    }
    int mkPenalty = 0;
    int missingCount = 0;
    int extraCount = 0;
    for (const QString& token : std::as_const(missing)) {
        if (isMk(token)) {
            mkPenalty += 25;
        } else if (hasDigit(token)) {
            return 0;
        } else {
            ++missingCount;
        }
    }
    for (const QString& token : std::as_const(extra)) {
        if (isMk(token)) {
            mkPenalty += 25;
        } else if (hasDigit(token)) {
            return 0;
        } else {
            ++extraCount;
        }
    }
    int score = (missingCount == 0 && extraCount == 0)
            ? 95
            : 90 - 25 * missingCount - 10 * extraCount;
    score -= mkPenalty + penalty;
    if (!device.brands.isEmpty() && !candidate.brands.isEmpty()) {
        bool brandShared = false;
        for (const QString& brand : device.brands) {
            brandShared |= candidate.brands.contains(brand);
        }
        if (!brandShared) {
            score -= 30;
        }
    }
    return score >= 50 ? score : 0;
}

bool productMatches(const ControllerMappingMatcher::Device& device, const ProductInfo& product) {
    if (!device.vendorId || !device.productId) {
        return false;
    }
    const QString protocol = product.protocol.toLower();
    if (device.hid ? protocol != QStringLiteral("hid") : protocol != QStringLiteral("midi")) {
        return false;
    }
    bool ok = false;
    if (product.vendor_id.toInt(&ok, 16) != *device.vendorId || !ok) {
        return false;
    }
    if (product.product_id.toInt(&ok, 16) != *device.productId || !ok) {
        return false;
    }
    if (device.hid && device.interfaceNumber && !product.interface_number.isEmpty()) {
        const int productInterface = product.interface_number.toInt(&ok, 16);
        if (ok && productInterface != *device.interfaceNumber) {
            return false;
        }
    }
    return true;
}

QString usbKey(const ControllerMappingMatcher::Device& device) {
    if (!device.vendorId || !device.productId) {
        return QString();
    }
    return QStringLiteral("%1:%2")
            .arg(static_cast<uint>(*device.vendorId), 4, 16, QLatin1Char('0'))
            .arg(static_cast<uint>(*device.productId), 4, 16, QLatin1Char('0'));
}

bool overrideMatches(const ControllerMappingMatcher::Device& device,
        const MappingInfo& mapping,
        const QJsonArray& overrides) {
    const QString fileName = QFileInfo(mapping.getPath()).fileName();
    const QString usb = usbKey(device);
    for (const auto& value : overrides) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("mapping")).toString().compare(fileName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString entryUsb = entry.value(QStringLiteral("usb")).toString().toLower();
        if (!entryUsb.isEmpty() && entryUsb == usb) {
            return true;
        }
        const QString pattern = entry.value(QStringLiteral("name")).toString();
        if (!pattern.isEmpty()) {
            const QRegularExpression expression(pattern, QRegularExpression::CaseInsensitiveOption);
            if (expression.isValid() && expression.match(device.name.trimmed()).hasMatch()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

int ControllerMappingMatcher::scoreName(const QString& deviceName,
        const QString& mappingText,
        int variantPenalty) {
    return scoreModels(modelOf(deviceName, true), modelOf(mappingText, false), variantPenalty);
}

int ControllerMappingMatcher::score(const Device& device,
        const MappingInfo& mapping,
        const QJsonArray& overrides) {
    if (!mapping.isValid()) {
        return 0;
    }
    if (overrideMatches(device, mapping, overrides)) {
        return 110;
    }
    for (const ProductInfo& product : mapping.getProducts()) {
        if (productMatches(device, product)) {
            return 100;
        }
    }
    const NameModel deviceModel = modelOf(device.name, true);
    int best = scoreModels(deviceModel, modelOf(QFileInfo(mapping.getPath()).fileName(), false), 0);
    best = std::max(best, scoreModels(deviceModel, modelOf(mapping.getName(), false), 0));
    if (!mapping.getControllerId().isEmpty()) {
        // Ids are often generic ("MixTrack", "Traktor"); rank them lower.
        best = std::max(best, scoreModels(deviceModel, modelOf(mapping.getControllerId(), false), 10));
    }
    return best;
}

QList<ControllerMappingMatcher::Match> ControllerMappingMatcher::rank(const Device& device,
        const QList<MappingInfo>& mappings,
        const QJsonArray& overrides) {
    QList<Match> result;
    for (const MappingInfo& mapping : mappings) {
        const int value = score(device, mapping, overrides);
        if (value >= kSuggestScore) {
            result.append(Match{mapping, value});
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const Match& left, const Match& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.mapping.getName().compare(right.mapping.getName(), Qt::CaseInsensitive) < 0;
    });
    return result;
}

const QJsonArray& ControllerMappingMatcher::builtinOverrides() {
    static const QJsonArray overrides = parseOverrides(QByteArray(kBuiltinOverrides));
    return overrides;
}

QJsonArray ControllerMappingMatcher::parseOverrides(const QByteArray& json) {
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (document.isArray()) {
        return document.array();
    }
    return document.object().value(QStringLiteral("overrides")).toArray();
}

QJsonArray ControllerMappingMatcher::loadOverrides(const QString& path) {
    QJsonArray merged;
    QFile file(path);
    if (!path.isEmpty() && file.open(QIODevice::ReadOnly)) {
        merged = parseOverrides(file.read(256 * 1024));
    }
    for (const auto& entry : builtinOverrides()) {
        merged.append(entry);
    }
    return merged;
}

QString ControllerMappingMatcher::modelName(const QString& deviceName) {
    const QString stripped = stripPortDecoration(deviceName);
    return stripped.isEmpty() ? deviceName.trimmed() : stripped;
}

int ControllerMappingMatcher::suggestedDeckCount(const MappingInfo& mapping,
        const QString& systemMappingsPath) {
    const auto mentionsUpperDecks = [](const QByteArray& text) {
        return text.contains("[Channel3]") || text.contains("[Channel4]");
    };
    QFile xml(mapping.getPath());
    if (!xml.open(QIODevice::ReadOnly)) {
        return 2;
    }
    const QByteArray content = xml.read(4 * 1024 * 1024);
    if (mentionsUpperDecks(content)) {
        return 4;
    }
    static const QRegularExpression scriptFile(
            QStringLiteral("<file\\b[^>]*\\bfilename\\s*=\\s*\"([^\"]+)\""));
    auto files = scriptFile.globalMatch(QString::fromUtf8(content));
    while (files.hasNext()) {
        const QString name = files.next().captured(1);
        if (name.contains(QStringLiteral(".."))) {
            continue;
        }
        for (const QString& directory : {mapping.getDirPath(), systemMappingsPath}) {
            QFile script(QDir(directory).filePath(name));
            if (script.open(QIODevice::ReadOnly)) {
                if (mentionsUpperDecks(script.read(4 * 1024 * 1024))) {
                    return 4;
                }
                break;
            }
        }
    }
    return 2;
}
