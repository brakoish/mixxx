/* Deck Lab three-band performance LV2. Stereo, per-deck, no RT allocations.
 *
 * Hot-path notes (perf-engine, see DECKLAB-NEXT-perf-engine.md):
 * - Per-run state lives in locals; ring buffers are restrict-qualified.
 * - Ring indices wrap with a compare instead of a per-frame modulo.
 * - The ducker beat gate uses a rotating phasor re-anchored to the exact
 *   cosf() every PHASOR_ANCHOR_FRAMES frames, and only while mode 5 is heard.
 * - Echo taps are read only by delay modes, and only for bands whose ring
 *   holds an echo or receives a send; a silent ring is not rewritten.
 * - Old/new mode crossfades are evaluated only while a fade is running.
 * - Release discards the band output, so only the ring state is maintained.
 * - Smoothed controls that reached their fixed point stay constant per block.
 * - With every section inactive, no release and silent echo rings, the block
 *   takes a dry path that keeps all state but skips the band processing.
 * Output matches tools/decklab/performance_reference.c within the tolerance
 * checked by tools/decklab/test_dsp.py. Built without -ffast-math so the
 * isfinite() NaN guards keep working. */
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

/* Audio-thread deadman. The native UI bumps the heartbeat every 100 ms; when
 * it stops changing for this long the effect returns to dry and stays dry
 * until the next activation. It was 0.65 s, which a GUI stall during a track
 * load could exceed, latching the effect dry mid-set. 1.5 s still returns a frozen
 * or lost owner to dry well inside two seconds. */
#define HEARTBEAT_TIMEOUT_SECONDS 1.5
/* A smoothed lever/isolator below this (-120 dB) whose target is zero snaps to
 * exactly zero, so idle detection does not depend on flush-to-zero. */
#define SNAP_EPSILON 1e-6f
/* The beat-gate phasor restarts from an exact cosf()/sinf() this often. */
#define PHASOR_ANCHOR_FRAMES 64u

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
    /* Per band, frames until both channels' lever / isolator echo rings are
     * known to hold only zeros. Reset to capacity by any non-zero write.
     * Bookkeeping only: never changes the output. */
    uint32_t lever_ring_live[3], iso_ring_live[3];
} Performance;

static float bound(const float* value, float fallback, float low, float high) {
    if (!value || !isfinite(*value)) return fallback;
    return fminf(high, fmaxf(low, *value));
}

static inline float soft_drive(float input, float amount) {
    const float gain = 1.0f + amount * 7.0f;
    /* Bounded rational soft clip with unity slope at zero. */
    const float shaped = (input * gain) / (1.0f + fabsf(input * gain)) *
        (1.0f + 1.0f / gain);
    return input + amount * (shaped - input);
}

static inline float tap(const float* restrict buffer, uint32_t newer, uint32_t older,
                        float fraction) {
    return buffer[newer] * (1.0f - fraction) + buffer[older] * fraction;
}

static inline float slew(float value, float target, float alpha) {
    return value + alpha * (target - value);
}

/* (position - delay) mod capacity for position < capacity, delay <= capacity. */
static inline uint32_t ring_back(uint32_t position, uint32_t delay, uint32_t capacity) {
    const uint32_t index = position + capacity - delay;
    return index >= capacity ? index - capacity : index;
}

static inline uint32_t ring_prev(uint32_t index, uint32_t capacity) {
    return index ? index - 1 : capacity - 1;
}

static inline float isolate_result(int mode, float input, float isolate,
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

static inline float lever_result(int mode, float input, float lever, float echo,
                                 float short_echo, float amount, int solo_enabled) {
    if (mode == 1) return input + amount * echo;
    if (mode == 2) return input + amount * (.7f * echo + .3f * short_echo);
    if (mode == 3 && solo_enabled) return input * (1 - amount * (1 - lever));
    return input;
}

static inline int is_delay_mode(int mode) {
    return mode == 1 || mode == 2;
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
    /* Rings keep stale history across activation (taps are fenced by
     * history_frames instead), so treat them as live for one full cycle. */
    for (int band = 0; band < 3; ++band)
        self->lever_ring_live[band] = self->iso_ring_live[band] = self->capacity;
}

static void run(LV2_Handle handle, uint32_t count) {
    Performance* const self = handle;
    if (!self || !self->input[0] || !self->input[1] ||
        !self->output[0] || !self->output[1]) return;
    const double rate = self->rate;
    const uint32_t capacity = self->capacity;
    /* Optional audio-thread deadman: GUI freeze/lost owner returns to dry.
     * Zero preserves legacy hosts. Expiry latches until the next activation. */
    const uint32_t heartbeat = (uint32_t)bound(self->port[HEARTBEAT], 0, 0, 65535);
    if (!heartbeat || heartbeat != self->heartbeat) self->stale_frames = 0;
    else if (self->stale_frames < (uint32_t)(rate * 2.0)) self->stale_frames += count;
    self->heartbeat = heartbeat;
    if (heartbeat && self->stale_frames > (uint32_t)(rate * HEARTBEAT_TIMEOUT_SECONDS))
        self->heartbeat_expired = 1;
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
    for (int band = 0; band < 3; ++band) {
        if (lever_target[band] == 0 && fabsf(self->lever_smooth[band]) < SNAP_EPSILON)
            self->lever_smooth[band] = 0;
        if (iso_target[band] == 0 && fabsf(self->isolate_smooth[band]) < SNAP_EPSILON)
            self->isolate_smooth[band] = 0;
    }
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
    const float phase_step = 1.0f / ((float)rate * beat_seconds);
    const float band_low[3] = {55, 300, 2700};
    const float band_high[3] = {240, 2200, 10000};
    float filter_alpha[3];
    for (int band = 0; band < 3; ++band) {
        const float position = fabsf(iso_target[band]);
        const float frequency = iso_target[band] < 0 ?
            band_high[band] * powf(band_low[band] / band_high[band], position) :
            band_low[band] * powf(band_high[band] / band_low[band], position);
        filter_alpha[band] = expf((float)(-2.0 * 3.141592653589793 * frequency / rate));
    }
    const float delay_target = fminf((float)capacity - 2,
                                     fmaxf(1, (float)rate * beat_seconds * beat_fraction));
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
    const float release_decay = expf((float)(-1.0 / (rate * beat_seconds * 1.4)));
    if (release && !self->prior_release) self->release_envelope = 1.0f;
    if (release == 3 && self->prior_release != 3) self->release_dry_fade = 0.0f;
    if (!release) self->release_envelope = self->release_dry_fade = 0.0f;
    self->prior_release = release;

    /* Working copy of the state. */
    const float slew_alpha = self->slew_alpha;
    const float delay_alpha = self->delay_alpha;
    const float low_alpha = self->low_alpha;
    const float high_alpha = self->high_alpha;
    const float mode_fade_step = self->mode_fade_step;
    const int lever_previous = self->lever_mode_previous;
    const int iso_previous = self->iso_mode_previous;
    float safety_mix = self->safety_mix;
    float beat_phase = self->beat_phase;
    float delay_smooth = self->delay_smooth;
    float lever_mode_fade = self->lever_mode_fade;
    float iso_mode_fade = self->iso_mode_fade;
    float release_envelope = self->release_envelope;
    float release_dry_fade = self->release_dry_fade;
    uint32_t write_pos = self->write_pos;
    uint32_t history_frames = self->history_frames;
    uint32_t lever_live[3], iso_live[3];
    memcpy(lever_live, self->lever_ring_live, sizeof(lever_live));
    memcpy(iso_live, self->iso_ring_live, sizeof(iso_live));
    int rings_silent = 1;
    for (int band = 0; band < 3; ++band)
        if (lever_live[band] || iso_live[band]) rings_silent = 0;
    float lever_smooth[3], isolate_smooth[3], amount_smooth[2];
    float low_state[2], high_state[2], filter_state[2][3];
    memcpy(lever_smooth, self->lever_smooth, sizeof(lever_smooth));
    memcpy(isolate_smooth, self->isolate_smooth, sizeof(isolate_smooth));
    memcpy(amount_smooth, self->amount_smooth, sizeof(amount_smooth));
    memcpy(low_state, self->low_state, sizeof(low_state));
    memcpy(high_state, self->high_state, sizeof(high_state));
    memcpy(filter_state, self->filter_state, sizeof(filter_state));
    float* restrict const release_ring[2] = {self->release_delay[0], self->release_delay[1]};

    /* A smoothed value at its fixed point stays there for the whole block
     * (the targets are constant per block), so its update can be skipped
     * without changing a single bit. */
    const int safety_settled = slew(safety_mix, safety_target, slew_alpha) == safety_mix;
    const int delay_settled = slew(delay_smooth, delay_target, delay_alpha) == delay_smooth;
    int sections_settled = 1, sections_inactive = 1;
    for (int band = 0; band < 3; ++band) {
        if (slew(lever_smooth[band], lever_target[band], slew_alpha) != lever_smooth[band] ||
                slew(isolate_smooth[band], iso_target[band], slew_alpha) != isolate_smooth[band])
            sections_settled = 0;
        if (lever_target[band] != 0 || iso_target[band] != 0 ||
                lever_smooth[band] != 0 || isolate_smooth[band] != 0)
            sections_inactive = 0;
    }
    for (int i = 0; i < 2; ++i)
        if (slew(amount_smooth[i], amount_target[i], slew_alpha) != amount_smooth[i])
            sections_settled = 0;

    if (!release && sections_inactive && rings_silent) {
        /* Dry path. With levers released, isolators centred and both echo
         * ring sets silent, every lever and isolator mode returns its band
         * unchanged, so the output is the band sum exactly as the full path
         * computes it. All state the full path needs later keeps running. */
        const float* restrict const input[2] = {self->input[0], self->input[1]};
        float* const output[2] = {self->output[0], self->output[1]};
        for (uint32_t frame = 0; frame < count; ++frame) {
            if (!safety_settled) safety_mix = slew(safety_mix, safety_target, slew_alpha);
            beat_phase += phase_step;
            if (beat_phase >= 1) beat_phase -= 1;
            if (!delay_settled) delay_smooth = slew(delay_smooth, delay_target, delay_alpha);
            lever_mode_fade = fminf(1, lever_mode_fade + mode_fade_step);
            iso_mode_fade = fminf(1, iso_mode_fade + mode_fade_step);
            if (!sections_settled) {
                for (int i = 0; i < 2; ++i)
                    amount_smooth[i] = slew(amount_smooth[i], amount_target[i], slew_alpha);
            }
            for (int channel = 0; channel < 2; ++channel) {
                const float raw = input[channel][frame];
                const float source = isfinite(raw) ? raw : 0.0f;
                low_state[channel] = (1 - low_alpha) * source + low_alpha * low_state[channel];
                high_state[channel] = (1 - high_alpha) * source + high_alpha * high_state[channel];
                const float band_value[3] = {
                    low_state[channel],
                    high_state[channel] - low_state[channel],
                    source - high_state[channel]
                };
                float processed = 0.0f;
                for (int band = 0; band < 3; ++band) {
                    filter_state[channel][band] =
                        (1 - filter_alpha[band]) * band_value[band] +
                        filter_alpha[band] * filter_state[channel][band];
                    processed += band_value[band];
                }
                if (!isfinite(processed)) processed = 0;
                release_ring[channel][write_pos] = processed;
                processed = source + (processed - source) * safety_mix;
                output[channel][frame] = fmaxf(-2.0f, fminf(2.0f, processed));
                if (fabsf(low_state[channel]) < 1e-20f) low_state[channel] = 0;
                if (fabsf(high_state[channel]) < 1e-20f) high_state[channel] = 0;
            }
            if (++write_pos == capacity) write_pos = 0;
            if (history_frames < capacity) ++history_frames;
        }
    } else {
        const float* restrict const input[2] = {self->input[0], self->input[1]};
        float* const output[2] = {self->output[0], self->output[1]};
        /* Beat gate phasor, only advanced while the ducker is audible. */
        const double step_angle = 2.0 * 3.141592653589793 * (double)phase_step;
        const double rotate_cos = cos(step_angle);
        const double rotate_sin = sin(step_angle);
        double gate_cos = 1.0, gate_sin = 0.0;
        uint32_t anchor_countdown = 0;
        /* Delay taps, constant while the delay time is settled. */
        uint32_t delay = (uint32_t)delay_smooth;
        float delay_fraction = delay_smooth - (float)delay;
        float short_time = fmaxf(1, delay_smooth * .6f);
        uint32_t short_delay = (uint32_t)short_time;
        float short_fraction = short_time - (float)short_delay;

        for (uint32_t frame = 0; frame < count; ++frame) {
            if (!safety_settled) safety_mix = slew(safety_mix, safety_target, slew_alpha);
            const float gate_phase = beat_phase;
            beat_phase += phase_step;
            if (beat_phase >= 1) beat_phase -= 1;
            if (!delay_settled) {
                delay_smooth = slew(delay_smooth, delay_target, delay_alpha);
                delay = (uint32_t)delay_smooth;
                delay_fraction = delay_smooth - (float)delay;
                short_time = fmaxf(1, delay_smooth * .6f);
                short_delay = (uint32_t)short_time;
                short_fraction = short_time - (float)short_delay;
            }
            lever_mode_fade = fminf(1, lever_mode_fade + mode_fade_step);
            iso_mode_fade = fminf(1, iso_mode_fade + mode_fade_step);
            if (!sections_settled) {
                for (int band = 0; band < 3; ++band) {
                    lever_smooth[band] = slew(lever_smooth[band], lever_target[band], slew_alpha);
                    isolate_smooth[band] = slew(isolate_smooth[band], iso_target[band], slew_alpha);
                }
                for (int i = 0; i < 2; ++i)
                    amount_smooth[i] = slew(amount_smooth[i], amount_target[i], slew_alpha);
            }
            const int solo_enabled = lever_smooth[0] + lever_smooth[1] + lever_smooth[2] > .001f;
            const int iso_fading = iso_mode_fade < 1;
            const int lever_fading = lever_mode_fade < 1;
            const int iso_delay = is_delay_mode(iso_mode) ||
                (iso_fading && is_delay_mode(iso_previous));
            const int lever_delay = is_delay_mode(lever_mode) ||
                (lever_fading && is_delay_mode(lever_previous));
            const int iso_short = iso_mode == 2 || (iso_fading && iso_previous == 2);
            const int lever_short = lever_mode == 2 || (lever_fading && lever_previous == 2);
            const float iso_feedback =
                (is_delay_mode(iso_mode) ? iso_mode : iso_previous) == 1 ? .32f : .22f;
            const float lever_feedback =
                (is_delay_mode(lever_mode) ? lever_mode : lever_previous) == 1 ? .38f : .24f;
            const int gate_needed = iso_mode == 5 || (iso_fading && iso_previous == 5);
            float beat_gate_early = 0.0f;
            if (gate_needed) {
                if (!anchor_countdown) {
                    const float angle = (float)(2.0 * 3.141592653589793) * gate_phase;
                    gate_cos = cosf(angle);
                    gate_sin = sinf(angle);
                    anchor_countdown = PHASOR_ANCHOR_FRAMES;
                }
                beat_gate_early = .5f + .5f * (float)gate_cos;
                const double next_cos = gate_cos * rotate_cos - gate_sin * rotate_sin;
                gate_sin = gate_sin * rotate_cos + gate_cos * rotate_sin;
                gate_cos = next_cos;
                --anchor_countdown;
            } else {
                anchor_countdown = 0;
            }
            uint32_t read = 0, older = 0, short_read = 0, short_older = 0;
            if (iso_delay || lever_delay || release) {
                read = ring_back(write_pos, delay, capacity);
                older = ring_prev(read, capacity);
                short_read = ring_back(write_pos, short_delay, capacity);
                short_older = ring_prev(short_read, capacity);
                /* capacity is an immutable zero sentinel, outside the write
                 * ring. Until history fills, redirect invalid interpolation
                 * taps there. This keeps activation constant-time. */
                if (history_frames <= delay) {
                    if (history_frames < delay) read = capacity;
                    older = capacity;
                }
                if (history_frames <= short_delay) {
                    if (history_frames < short_delay) short_read = capacity;
                    short_older = capacity;
                }
            }
            int wrote_iso[3] = {0, 0, 0}, wrote_lever[3] = {0, 0, 0};
            for (int channel = 0; channel < 2; ++channel) {
                const float raw = input[channel][frame];
                const float source = isfinite(raw) ? raw : 0.0f;
                low_state[channel] = (1 - low_alpha) * source + low_alpha * low_state[channel];
                high_state[channel] = (1 - high_alpha) * source + high_alpha * high_state[channel];
                const float band_value[3] = {
                    low_state[channel],
                    high_state[channel] - low_state[channel],
                    source - high_state[channel]
                };
                float processed = 0.0f;
                for (int band = 0; band < 3; ++band) {
                    const float input_band = band_value[band];
                    const float iso = isolate_smooth[band];
                    const float attenuated = input_band * (iso < 0 ? 1 + iso : 1);
                    filter_state[channel][band] =
                        (1 - filter_alpha[band]) * input_band +
                        filter_alpha[band] * filter_state[channel][band];
                    const float filtered = iso < 0 ? filter_state[channel][band] :
                        input_band - filter_state[channel][band];
                    float* restrict const iso_ring = self->isolate_delay[channel][band];
                    float iso_echo = 0.0f, iso_short_echo = 0.0f;
                    /* A silent ring without a send stays silent: its taps
                     * and feedback write are exactly zero. */
                    if (iso_delay && (iso > 0 || iso_live[band])) {
                        iso_echo = tap(iso_ring, read, older, delay_fraction);
                        if (iso_short)
                            iso_short_echo = tap(iso_ring, short_read, short_older, short_fraction);
                        const float send = fmaxf(0, iso) * attenuated;
                        const float next = send + iso_echo * iso_feedback;
                        const float stored = fabsf(next) < 1e-20f ? 0 : next;
                        iso_ring[write_pos] = stored;
                        wrote_iso[band] |= stored != 0;
                    } else if (iso_live[band]) {
                        iso_ring[write_pos] = 0;
                    }
                    const float lever = lever_smooth[band];
                    /* In release the band output is discarded; the isolator
                     * result is still needed when it feeds a lever echo. */
                    float isolated = input_band;
                    if (!release || (lever_delay && lever != 0)) {
                        const float beat_gate = iso < 0 ? 1 - beat_gate_early : beat_gate_early;
                        const float new_isolate = isolate_result(iso_mode, input_band, iso,
                            iso_echo, iso_short_echo, amount_smooth[1], filtered, beat_gate);
                        if (iso_fading) {
                            const float old_isolate = isolate_result(iso_previous, input_band,
                                iso, iso_echo, iso_short_echo, amount_smooth[1], filtered,
                                beat_gate);
                            isolated = old_isolate * (1 - iso_mode_fade) +
                                new_isolate * iso_mode_fade;
                        } else {
                            isolated = new_isolate;
                        }
                    }
                    float* restrict const lever_ring = self->lever_delay[channel][band];
                    float lever_echo = 0.0f, lever_short_echo = 0.0f;
                    if (lever_delay && (lever != 0 || lever_live[band])) {
                        lever_echo = tap(lever_ring, read, older, delay_fraction);
                        if (lever_short)
                            lever_short_echo = tap(lever_ring, short_read, short_older, short_fraction);
                        const float next = lever * isolated + lever_echo * lever_feedback;
                        const float stored = fabsf(next) < 1e-20f ? 0 : next;
                        lever_ring[write_pos] = stored;
                        wrote_lever[band] |= stored != 0;
                    } else if (lever_live[band]) {
                        lever_ring[write_pos] = 0;
                    }
                    if (!release) {
                        const float new_lever = lever_result(lever_mode, isolated, lever,
                            lever_echo, lever_short_echo, amount_smooth[0], solo_enabled);
                        if (lever_fading) {
                            const float old_lever = lever_result(lever_previous, isolated, lever,
                                lever_echo, lever_short_echo, amount_smooth[0], solo_enabled);
                            processed += old_lever * (1 - lever_mode_fade) +
                                new_lever * lever_mode_fade;
                        } else {
                            processed += new_lever;
                        }
                    }
                }
                if (!isfinite(processed)) processed = 0;
                if (release) {
                    const float tail = tap(release_ring[channel], read, older, delay_fraction);
                    release_ring[channel][write_pos] = tail * .52f;
                    processed = (release == 1 ? source : release == 3 ? source * release_dry_fade : 0) +
                        tail * release_envelope * .72f;
                } else release_ring[channel][write_pos] = processed;
                /* Leave inactive audio untouched. With FX active, a continuous
                 * soft knee bounds the summed dry + two wet sections below 0 dBFS.
                 * This is a peak guard, not loudness matching or a master limiter. */
                if (processing && (release || effective_depth) && fabsf(processed) > .9f) {
                    const float excess = fabsf(processed) - .9f;
                    processed = copysignf(.9f + .1f * excess / (.1f + excess), processed);
                }
                processed = source + (processed - source) * safety_mix;
                output[channel][frame] = fmaxf(-2.0f, fminf(2.0f, processed));
                if (fabsf(low_state[channel]) < 1e-20f) low_state[channel] = 0;
                if (fabsf(high_state[channel]) < 1e-20f) high_state[channel] = 0;
            }
            for (int band = 0; band < 3; ++band) {
                if (wrote_iso[band]) iso_live[band] = capacity;
                else if (iso_live[band]) --iso_live[band];
                if (wrote_lever[band]) lever_live[band] = capacity;
                else if (lever_live[band]) --lever_live[band];
            }
            if (release) release_envelope *= release_decay;
            if (release == 3) release_dry_fade = fminf(1, release_dry_fade + phase_step);
            if (++write_pos == capacity) write_pos = 0;
            if (history_frames < capacity) ++history_frames;
        }
    }

    self->safety_mix = safety_mix;
    self->beat_phase = beat_phase;
    self->delay_smooth = delay_smooth;
    self->lever_mode_fade = lever_mode_fade;
    self->iso_mode_fade = iso_mode_fade;
    self->release_envelope = release_envelope;
    self->release_dry_fade = release_dry_fade;
    self->write_pos = write_pos;
    self->history_frames = history_frames;
    memcpy(self->lever_ring_live, lever_live, sizeof(lever_live));
    memcpy(self->iso_ring_live, iso_live, sizeof(iso_live));
    memcpy(self->lever_smooth, lever_smooth, sizeof(lever_smooth));
    memcpy(self->isolate_smooth, isolate_smooth, sizeof(isolate_smooth));
    memcpy(self->amount_smooth, amount_smooth, sizeof(amount_smooth));
    memcpy(self->low_state, low_state, sizeof(low_state));
    memcpy(self->high_state, high_state, sizeof(high_state));
    memcpy(self->filter_state, filter_state, sizeof(filter_state));
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
