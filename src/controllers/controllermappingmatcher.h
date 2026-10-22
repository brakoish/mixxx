// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>
#include <optional>

#include "controllers/controllermappinginfo.h"

/// Suggests a bundled or user mapping for a connected controller.
///
/// HID (and MIDI mappings that declare USB ids) match exactly by product
/// info. Everything else is matched by name heuristics against the mapping's
/// file name, <info><name> and <controller id>. A small JSON override table
/// covers controllers whose port names do not resemble their mapping.
/// Scores: 0 = no match, 70+ = worth suggesting, 90+ = confident, 100 =
/// exact model, 110 = explicit override.
class ControllerMappingMatcher {
  public:
    static constexpr int kSuggestScore = 70;
    static constexpr int kConfidentScore = 90;
    /// Same model by name (not merely a generic controller id).
    static constexpr int kExactScore = 95;

    struct Device {
        QString name; // Mixxx controller name, e.g. "DDJ-FLX4 MIDI 1"
        std::optional<uint16_t> vendorId;
        std::optional<uint16_t> productId;
        std::optional<int> interfaceNumber;
        bool hid = false;
    };
    struct Match {
        MappingInfo mapping;
        int score = 0;
    };

    /// Name heuristic only. `variantPenalty` lowers weak sources (ids).
    static int scoreName(const QString& deviceName,
            const QString& mappingText,
            int variantPenalty = 0);
    static int score(const Device& device,
            const MappingInfo& mapping,
            const QJsonArray& overrides = builtinOverrides());
    /// Best first. Mappings scoring below kSuggestScore are omitted.
    static QList<Match> rank(const Device& device,
            const QList<MappingInfo>& mappings,
            const QJsonArray& overrides = builtinOverrides());

    /// The compiled-in table, parsed once.
    static const QJsonArray& builtinOverrides();
    /// User entries from `path` (same format) take precedence over builtins.
    static QJsonArray loadOverrides(const QString& path);
    static QJsonArray parseOverrides(const QByteArray& json);

    /// "DDJ-FLX4 MIDI 1" -> "DDJ-FLX4" for display.
    static QString modelName(const QString& deviceName);
    /// 4 when the mapping or its scripts address [Channel3]/[Channel4].
    static int suggestedDeckCount(const MappingInfo& mapping, const QString& systemMappingsPath);
};
