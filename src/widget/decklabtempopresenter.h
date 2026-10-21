// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>

// Presentation only. Never writes tempo, beat grids or engine rate.
struct DeckLabTempoPresenter {
    double local = 0, ratio = 0, shown = 0;
    long long at = -1;
    double update(bool loaded, double localBpm, double speed, double raw, long long now) {
        if (!loaded || !std::isfinite(localBpm) || localBpm <= 0 ||
                !std::isfinite(speed) || speed <= 0) {
            at = -1;
            return loaded && std::isfinite(raw) && raw > 0 ? raw : 0;
        }
        const bool reset = at < 0 || now < at || now - at > 2000 || std::abs(localBpm - local) > .1;
        local = reset ? localBpm : local + (localBpm - local) * (1 - std::exp(-(now - at) / 4000.0));
        const double bpm = local * speed;
        if (reset || std::abs(speed - ratio) > 1e-9 || std::abs(bpm - shown) >= .015)
            shown = std::round(bpm * 100) / 100;
        ratio = speed;
        at = now;
        return shown;
    }
};
