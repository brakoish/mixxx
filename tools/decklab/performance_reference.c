/* Deck Lab three-band performance LV2: REFERENCE COPY, test-only. This is the
 * unoptimised processor exactly as shipped before the perf-engine work
 * (commit d22d6e4a15). It is built by CMake as performance_reference.so, never
 * installed, and tools/decklab/test_dsp.py compares the shipped processor
 * against it on generated signals. Do not edit or optimise this file. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef void* LV2_Handle;
typedef struct { const char* URI; void* data; } LV2_Feature;
typedef struct LV2_Descriptor LV2_Descriptor;
struct LV2_Descriptor {
    const char* URI;
    LV2_Handle (*instantiate)(const LV2_Descriptor*, double, const char*, const LV2_Feature* const*);
    void (*connect_port)(LV2_Handle, uint32_t, void*);
    void (*activate)(LV2_Handle);
    void (*run)(LV2_Handle, uint32_t);
    void (*deactivate)(LV2_Handle);
    void (*cleanup)(LV2_Handle);
    const void* (*extension_data)(const char*);
};

enum {
    IN_L, IN_R, OUT_L, OUT_R,
    LEVER_LOW, LEVER_MID, LEVER_HIGH, LEVER_MODE, LEVER_AMOUNT,
    ISO_LOW, ISO_MID, ISO_HIGH, ISO_MODE, ISO_AMOUNT,
    BPM, BEAT_FRACTION, RELEASE_TYPE, BEAT_PHASE, HEARTBEAT, PORT_COUNT
};

typedef struct {
    const float* input[2];
    float* output[2];
    const float* port[PORT_COUNT];
    float* lever_delay[2][3];
    float* isolate_delay[2][3];
    float* release_delay[2];
    float low_state[2], high_state[2];
    float filter_state[2][3];
    float lever_smooth[3], isolate_smooth[3], amount_smooth[2];
    float low_alpha, high_alpha, slew_alpha, delay_alpha;
    float delay_smooth;
    int delay_initialized;
    int lever_mode_current, lever_mode_previous, iso_mode_current, iso_mode_previous;
    float lever_mode_fade, iso_mode_fade, mode_fade_step;
    float release_envelope;
    float release_dry_fade;
    float beat_phase, last_phase_report;
    int phase_initialized;
    uint32_t capacity, write_pos, history_frames;
    int prior_release;
    uint32_t heartbeat, stale_frames;
    int heartbeat_expired;
    float safety_mix;
    double rate;
} Performance;

static float bound(const float* value, float fallback, float low, float high) {
    if (!value || !isfinite(*value)) return fallback;
    return fminf(high, fmaxf(low, *value));
}

static float soft_drive(float input, float amount) {
    const float gain = 1.0f + amount * 7.0f;
    /* Bounded rational soft clip with unity slope at zero. */
    const float shaped = (input * gain) / (1.0f + fabsf(input * gain)) *
        (1.0f + 1.0f / gain);
    return input + amount * (shaped - input);
}

static float tap(const float* buffer, uint32_t newer, uint32_t older, float fraction) {
    return buffer[newer] * (1.0f - fraction) + buffer[older] * fraction;
}

static float isolate_result(int mode, float input, float isolate,
                            float echo, float short_echo, float amount,
                            float filtered, float beat_gate) {
    const float cut_input = isolate < 0 && mode <= 3 ? input * (1 + isolate) : input;
    const float positive = fmaxf(0, isolate);
    if (mode == 1) return cut_input + amount * echo;
    if (mode == 2) return cut_input + amount * (.7f * echo + .3f * short_echo);
    if (mode == 3 && positive > 0) return cut_input * (1 - positive) +
        soft_drive(cut_input, positive * amount) * positive;
    if (mode == 4) {
        const float strength = fabsf(isolate) * amount;
        return input * (1 - strength) + filtered * strength;
    }
    if (mode == 5) return input * (1 - fabsf(isolate) * amount * (1 - beat_gate));
    return cut_input;
}

static float lever_result(int mode, float input, float lever, float echo,
                          float short_echo, float amount, int solo_enabled) {
    if (mode == 1) return input + amount * echo;
    if (mode == 2) return input + amount * (.7f * echo + .3f * short_echo);
    if (mode == 3 && solo_enabled) return input * (1 - amount * (1 - lever));
    return input;
}

// Prefault delay pages during non-realtime instantiation. calloc alone may
// defer page faults until the first callback writes each delay line.
static float* allocate_delay(uint32_t capacity) {
    float* data = calloc(capacity + 1u, sizeof(float));
    if (data) for (uint32_t at = 0; at <= capacity; at += 1024u) ((volatile float*)data)[at] = 0;
    return data;
}

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate,
                              const char* bundle, const LV2_Feature* const* features) {
    (void)descriptor; (void)bundle; (void)features;
    if (!isfinite(rate) || rate < 8000.0 || rate > 192000.0) return NULL;
    Performance* self = (Performance*)calloc(1, sizeof(*self));
    if (!self) return NULL;
    self->rate = rate;
    self->capacity = (uint32_t)(rate * 2.0) + 1;
    self->low_alpha = expf((float)(-2.0 * 3.141592653589793 * 250.0 / rate));
    self->high_alpha = expf((float)(-2.0 * 3.141592653589793 * 2500.0 / rate));
    self->slew_alpha = 1.0f - expf((float)(-1.0 / (rate * 0.008)));
    self->delay_alpha = 1.0f - expf((float)(-1.0 / (rate * 0.04)));
    self->mode_fade_step = (float)(1.0 / (rate * .02));
    for (int channel = 0; channel < 2; ++channel) {
        for (int band = 0; band < 3; ++band) {
            self->lever_delay[channel][band] = allocate_delay(self->capacity);
            self->isolate_delay[channel][band] = allocate_delay(self->capacity);
            if (!self->lever_delay[channel][band] || !self->isolate_delay[channel][band]) goto fail;
        }
        self->release_delay[channel] = allocate_delay(self->capacity);
        if (!self->release_delay[channel]) goto fail;
    }
    return self;
fail:
    for (int channel = 0; channel < 2; ++channel) {
        for (int band = 0; band < 3; ++band) {
            free(self->lever_delay[channel][band]);
            free(self->isolate_delay[channel][band]);
        }
        free(self->release_delay[channel]);
    }
    free(self);
    return NULL;
}

static void connect_port(LV2_Handle handle, uint32_t index, void* data) {
    Performance* self = handle;
    if (!self || index >= PORT_COUNT) return;
    if (index < 2) self->input[index] = data;
    else if (index < 4) self->output[index - 2] = data;
    else self->port[index] = data;
}

static void activate(LV2_Handle handle) {
    Performance* self = handle;
    if (!self) return;
    self->heartbeat = self->stale_frames = 0;
    self->heartbeat_expired = 0;
    self->safety_mix = 1;
    self->write_pos = 0;
    self->history_frames = 0;
    self->prior_release = 0;
    self->release_envelope = 0.0f;
    self->release_dry_fade = 0.0f;
    self->beat_phase = 0.0f;
    self->last_phase_report = -1.0f;
    self->phase_initialized = 0;
    self->delay_initialized = 0;
    self->lever_mode_current = self->lever_mode_previous = 0;
    self->iso_mode_current = self->iso_mode_previous = 0;
    self->lever_mode_fade = self->iso_mode_fade = 1.0f;
    memset(self->low_state, 0, sizeof(self->low_state));
    memset(self->high_state, 0, sizeof(self->high_state));
    memset(self->filter_state, 0, sizeof(self->filter_state));
    memset(self->lever_smooth, 0, sizeof(self->lever_smooth));
    memset(self->isolate_smooth, 0, sizeof(self->isolate_smooth));
    memset(self->amount_smooth, 0, sizeof(self->amount_smooth));
}

static void run(LV2_Handle handle, uint32_t count) {
    Performance* self = handle;
    if (!self || !self->input[0] || !self->input[1] ||
        !self->output[0] || !self->output[1]) return;
    /* Optional audio-thread deadman: GUI freeze/lost owner returns to dry.
     * Zero preserves legacy hosts. Expiry latches until the next activation. */
    const uint32_t heartbeat = (uint32_t)bound(self->port[HEARTBEAT], 0, 0, 65535);
    if (!heartbeat || heartbeat != self->heartbeat) self->stale_frames = 0;
    else if (self->stale_frames < (uint32_t)self->rate) self->stale_frames += count;
    self->heartbeat = heartbeat;
    if (heartbeat && self->stale_frames > (uint32_t)(self->rate * .65)) self->heartbeat_expired = 1;
    const float safety_target = self->heartbeat_expired ? 0 : 1;
    const float lever_target[3] = {
        bound(self->port[LEVER_LOW], 0, 0, 1),
        bound(self->port[LEVER_MID], 0, 0, 1),
        bound(self->port[LEVER_HIGH], 0, 0, 1)
    };
    const float iso_target[3] = {
        bound(self->port[ISO_LOW], 0, -1, 1),
        bound(self->port[ISO_MID], 0, -1, 1),
        bound(self->port[ISO_HIGH], 0, -1, 1)
    };
    const int lever_mode = (int)roundf(bound(self->port[LEVER_MODE], 0, 0, 3));
    const int iso_mode = (int)roundf(bound(self->port[ISO_MODE], 0, 0, 5));
    const float amount_target[2] = {
        bound(self->port[LEVER_AMOUNT], .65f, 0, 1),
        bound(self->port[ISO_AMOUNT], .65f, 0, 1)
    };
    const float bpm = bound(self->port[BPM], 120, 40, 240);
    const float phase_report = bound(self->port[BEAT_PHASE], -1, -1, 1);
    const float beat_fraction = bound(self->port[BEAT_FRACTION], .5f, .0625f, 1);
    const int release = (int)roundf(bound(self->port[RELEASE_TYPE], 0, 0, 3));
    const int processing = release || lever_target[0] || lever_target[1] || lever_target[2] ||
        iso_target[0] || iso_target[1] || iso_target[2] ||
        fabsf(self->lever_smooth[0]) > 1e-5f || fabsf(self->lever_smooth[1]) > 1e-5f ||
        fabsf(self->lever_smooth[2]) > 1e-5f || fabsf(self->isolate_smooth[0]) > 1e-5f ||
        fabsf(self->isolate_smooth[1]) > 1e-5f || fabsf(self->isolate_smooth[2]) > 1e-5f;
    const int lever_signal = lever_target[0] || lever_target[1] || lever_target[2] ||
        self->lever_smooth[0] > 1e-5f || self->lever_smooth[1] > 1e-5f ||
        self->lever_smooth[2] > 1e-5f;
    const int iso_signal = iso_target[0] || iso_target[1] || iso_target[2] ||
        fabsf(self->isolate_smooth[0]) > 1e-5f || fabsf(self->isolate_smooth[1]) > 1e-5f ||
        fabsf(self->isolate_smooth[2]) > 1e-5f;
    const int effective_depth =
        (lever_signal && (amount_target[0] > 1e-5f || self->amount_smooth[0] > 1e-5f)) ||
        (iso_signal && (amount_target[1] > 1e-5f || self->amount_smooth[1] > 1e-5f));
    const float beat_seconds = 60.0f / bpm;
    if (phase_report >= 0 &&
            (!self->phase_initialized || fabsf(phase_report - self->last_phase_report) > .002f)) {
        if (!self->phase_initialized) self->beat_phase = phase_report;
        else {
            float correction = phase_report - self->beat_phase;
            if (correction > .5f) correction -= 1;
            if (correction < -.5f) correction += 1;
            self->beat_phase += correction * .35f;
            if (self->beat_phase < 0) self->beat_phase += 1;
            if (self->beat_phase >= 1) self->beat_phase -= 1;
        }
        self->last_phase_report = phase_report;
        self->phase_initialized = 1;
    }
    const float phase_step = 1.0f / ((float)self->rate * beat_seconds);
    const float band_low[3] = {55, 300, 2700};
    const float band_high[3] = {240, 2200, 10000};
    float filter_alpha[3];
    for (int band = 0; band < 3; ++band) {
        const float position = fabsf(iso_target[band]);
        const float frequency = iso_target[band] < 0 ?
            band_high[band] * powf(band_low[band] / band_high[band], position) :
            band_low[band] * powf(band_high[band] / band_low[band], position);
        filter_alpha[band] = expf((float)(-2.0 * 3.141592653589793 * frequency / self->rate));
    }
    const float delay_target = fminf((float)self->capacity - 2,
                                     fmaxf(1, (float)self->rate * beat_seconds * beat_fraction));
    if (!self->delay_initialized) {
        self->delay_smooth = delay_target;
        self->delay_initialized = 1;
    }
    if (lever_mode != self->lever_mode_current) {
        self->lever_mode_previous = self->lever_mode_current;
        self->lever_mode_current = lever_mode;
        self->lever_mode_fade = 0;
    }
    if (iso_mode != self->iso_mode_current) {
        self->iso_mode_previous = self->iso_mode_current;
        self->iso_mode_current = iso_mode;
        self->iso_mode_fade = 0;
    }
    const float release_decay = expf((float)(-1.0 / (self->rate * beat_seconds * 1.4)));
    if (release && !self->prior_release) self->release_envelope = 1.0f;
    if (release == 3 && self->prior_release != 3) self->release_dry_fade = 0.0f;
    if (!release) self->release_envelope = self->release_dry_fade = 0.0f;
    self->prior_release = release;

    for (uint32_t frame = 0; frame < count; ++frame) {
        self->safety_mix += (safety_target - self->safety_mix) * self->slew_alpha;
        const float beat_gate_early = .5f + .5f * cosf((float)(2.0 * 3.141592653589793) * self->beat_phase);
        self->beat_phase += phase_step;
        if (self->beat_phase >= 1) self->beat_phase -= 1;
        self->delay_smooth += self->delay_alpha * (delay_target - self->delay_smooth);
        const uint32_t delay = (uint32_t)self->delay_smooth;
        const float delay_fraction = self->delay_smooth - (float)delay;
        const float short_time = fmaxf(1, self->delay_smooth * .6f);
        const uint32_t short_delay = (uint32_t)short_time;
        const float short_fraction = short_time - (float)short_delay;
        uint32_t read = (self->write_pos + self->capacity - delay) % self->capacity;
        uint32_t older = (read + self->capacity - 1) % self->capacity;
        uint32_t short_read = (self->write_pos + self->capacity - short_delay) % self->capacity;
        uint32_t short_older = (short_read + self->capacity - 1) % self->capacity;
        /* capacity is an immutable zero sentinel, outside the write ring.
         * Until history fills, redirect invalid interpolation taps there.
         * This keeps activation constant-time and the hot tap path unchanged. */
        if (self->history_frames <= delay) {
            if (self->history_frames < delay) read = self->capacity;
            older = self->capacity;
        }
        if (self->history_frames <= short_delay) {
            if (self->history_frames < short_delay) short_read = self->capacity;
            short_older = self->capacity;
        }
        self->lever_mode_fade = fminf(1, self->lever_mode_fade + self->mode_fade_step);
        self->iso_mode_fade = fminf(1, self->iso_mode_fade + self->mode_fade_step);
        for (int band = 0; band < 3; ++band) {
            self->lever_smooth[band] += self->slew_alpha *
                (lever_target[band] - self->lever_smooth[band]);
            self->isolate_smooth[band] += self->slew_alpha *
                (iso_target[band] - self->isolate_smooth[band]);
        }
        for (int i = 0; i < 2; ++i) self->amount_smooth[i] +=
            self->slew_alpha * (amount_target[i] - self->amount_smooth[i]);
        const int solo_enabled = self->lever_smooth[0] + self->lever_smooth[1] +
            self->lever_smooth[2] > .001f;
        for (int channel = 0; channel < 2; ++channel) {
            const float source = isfinite(self->input[channel][frame]) ?
                self->input[channel][frame] : 0.0f;
            self->low_state[channel] = (1 - self->low_alpha) * source +
                self->low_alpha * self->low_state[channel];
            self->high_state[channel] = (1 - self->high_alpha) * source +
                self->high_alpha * self->high_state[channel];
            const float band_value[3] = {
                self->low_state[channel],
                self->high_state[channel] - self->low_state[channel],
                source - self->high_state[channel]
            };
            float processed = 0.0f;
            for (int band = 0; band < 3; ++band) {
                const float iso = self->isolate_smooth[band];
                const float attenuated = band_value[band] * (iso < 0 ? 1 + iso : 1);
                self->filter_state[channel][band] =
                    (1 - filter_alpha[band]) * band_value[band] +
                    filter_alpha[band] * self->filter_state[channel][band];
                const float filtered = iso < 0 ? self->filter_state[channel][band] :
                    band_value[band] - self->filter_state[channel][band];
                const float beat_gate = iso < 0 ? 1 - beat_gate_early : beat_gate_early;
                const float iso_echo = tap(self->isolate_delay[channel][band], read, older, delay_fraction);
                const float iso_short = tap(self->isolate_delay[channel][band], short_read, short_older, short_fraction);
                const int iso_delay_mode = (iso_mode == 1 || iso_mode == 2 ||
                    (self->iso_mode_fade < 1 &&
                     (self->iso_mode_previous == 1 || self->iso_mode_previous == 2)));
                if (iso_delay_mode) {
                    const float send = fmaxf(0, iso) * attenuated;
                    const int feedback_mode = iso_mode == 1 || iso_mode == 2 ?
                        iso_mode : self->iso_mode_previous;
                    const float next = send + iso_echo * (feedback_mode == 1 ? .32f : .22f);
                    self->isolate_delay[channel][band][self->write_pos] =
                        fabsf(next) < 1e-20f ? 0 : next;
                } else self->isolate_delay[channel][band][self->write_pos] = 0;
                const float old_isolate = isolate_result(self->iso_mode_previous, band_value[band],
                    iso, iso_echo, iso_short, self->amount_smooth[1], filtered, beat_gate);
                const float new_isolate = isolate_result(iso_mode, band_value[band],
                    iso, iso_echo, iso_short, self->amount_smooth[1], filtered, beat_gate);
                const float isolated = old_isolate * (1 - self->iso_mode_fade) +
                    new_isolate * self->iso_mode_fade;
                const float lever = self->lever_smooth[band];
                const float lever_echo = tap(self->lever_delay[channel][band], read, older, delay_fraction);
                const float lever_short = tap(self->lever_delay[channel][band], short_read, short_older, short_fraction);
                const int lever_delay_mode = (lever_mode == 1 || lever_mode == 2 ||
                    (self->lever_mode_fade < 1 &&
                     (self->lever_mode_previous == 1 || self->lever_mode_previous == 2)));
                if (lever_delay_mode) {
                    const int feedback_mode = lever_mode == 1 || lever_mode == 2 ?
                        lever_mode : self->lever_mode_previous;
                    const float next = lever * isolated + lever_echo * (feedback_mode == 1 ? .38f : .24f);
                    self->lever_delay[channel][band][self->write_pos] =
                        fabsf(next) < 1e-20f ? 0 : next;
                } else self->lever_delay[channel][band][self->write_pos] = 0;
                const float old_lever = lever_result(self->lever_mode_previous, isolated, lever,
                    lever_echo, lever_short, self->amount_smooth[0], solo_enabled);
                const float new_lever = lever_result(lever_mode, isolated, lever,
                    lever_echo, lever_short, self->amount_smooth[0], solo_enabled);
                processed += old_lever * (1 - self->lever_mode_fade) +
                    new_lever * self->lever_mode_fade;
            }
            if (!isfinite(processed)) processed = 0;
            const float tail = tap(self->release_delay[channel], read, older, delay_fraction);
            if (release) {
                self->release_delay[channel][self->write_pos] = tail * .52f;
                processed = (release == 1 ? source : release == 3 ? source * self->release_dry_fade : 0) +
                    tail * self->release_envelope * .72f;
            } else self->release_delay[channel][self->write_pos] = processed;
            /* Leave inactive audio untouched. With FX active, a continuous
             * soft knee bounds the summed dry + two wet sections below 0 dBFS.
             * This is a peak guard, not loudness matching or a master limiter. */
            if (processing && (release || effective_depth) && fabsf(processed) > .9f) {
                const float excess = fabsf(processed) - .9f;
                processed = copysignf(.9f + .1f * excess / (.1f + excess), processed);
            }
            processed = source + (processed - source) * self->safety_mix;
            self->output[channel][frame] = fmaxf(-2.0f, fminf(2.0f, processed));
            if (fabsf(self->low_state[channel]) < 1e-20f) self->low_state[channel] = 0;
            if (fabsf(self->high_state[channel]) < 1e-20f) self->high_state[channel] = 0;
        }
        if (release) self->release_envelope *= release_decay;
        if (release == 3) self->release_dry_fade = fminf(1, self->release_dry_fade + phase_step);
        if (++self->write_pos == self->capacity) self->write_pos = 0;
        if (self->history_frames < self->capacity) ++self->history_frames;
    }
}

static void cleanup(LV2_Handle handle) {
    Performance* self = handle;
    if (!self) return;
    for (int channel = 0; channel < 2; ++channel) {
        for (int band = 0; band < 3; ++band) {
            free(self->lever_delay[channel][band]);
            free(self->isolate_delay[channel][band]);
        }
        free(self->release_delay[channel]);
    }
    free(self);
}

static const LV2_Descriptor descriptor = {
    "urn:decklab:threeband-performance-native-v1", instantiate, connect_port,
    activate, run, NULL, cleanup, NULL
};

__attribute__((visibility("default")))
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
