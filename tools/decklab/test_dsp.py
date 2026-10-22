"""Signal and continuity checks for the standalone performance LV2.

Usage: test_dsp.py <performance.so> [<performance_reference.so>]

The reference is the unoptimised processor (tools/decklab/performance_reference.c)
that CMake builds next to the shipped one without installing it. When the
second argument is omitted it is looked up at the top of the build tree.
"""
import ctypes as c
import math
from pathlib import Path
import statistics
import sys
import time

class Descriptor(c.Structure):
    pass


Instantiate = c.CFUNCTYPE(c.c_void_p, c.POINTER(Descriptor), c.c_double,
                          c.c_char_p, c.c_void_p)
Connect = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_void_p)
Activate = c.CFUNCTYPE(None, c.c_void_p)
Run = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32)
Cleanup = c.CFUNCTYPE(None, c.c_void_p)
Descriptor._fields_ = [
    ('URI', c.c_char_p), ('instantiate', Instantiate), ('connect_port', Connect),
    ('activate', Activate), ('run', Run), ('deactivate', c.c_void_p),
    ('cleanup', Cleanup), ('extension_data', c.c_void_p),
]


def rms(values):
    return math.sqrt(sum(v*v for v in values) / len(values))

library = c.CDLL(str(Path(sys.argv[1]).resolve()))
library.lv2_descriptor.argtypes = [c.c_uint32]
library.lv2_descriptor.restype = c.POINTER(Descriptor)
desc = library.lv2_descriptor(0)
assert desc and desc.contents.URI == b'urn:decklab:threeband-performance-native-v1'


def render(freq=100, changes=None, seconds=1, rate=44100, signal_seconds=.1, amplitude=.25):
    changes = changes or {}
    n = rate * seconds
    ports = [(c.c_float * n)() for _ in range(4)]
    for i in range(min(n, int(rate * signal_seconds))):
        ports[0][i] = amplitude * math.sin(2 * math.pi * freq * i / rate)
    values = [0, 0, 0, 0, .65, 0, 0, 0, 0, .65, 120, .5, 0, -1, 0]
    controls = [c.c_float(v) for v in values]
    instance = desc.contents.instantiate(desc, rate, None, None)
    assert instance
    for i, p in enumerate(ports):
        desc.contents.connect_port(instance, i, c.cast(p, c.c_void_p))
    for i, p in enumerate(controls, 4):
        desc.contents.connect_port(instance, i, c.cast(c.pointer(p), c.c_void_p))
    desc.contents.activate(instance)
    started = time.perf_counter()
    for offset in range(0, n, 256):
        for port, value in changes.get(offset, {}).items():
            controls[port - 4].value = value
        count = min(256, n-offset)
        for channel in range(2):
            desc.contents.connect_port(instance, channel, c.cast(c.byref(ports[channel], 4*offset), c.c_void_p))
            desc.contents.connect_port(instance, channel+2, c.cast(c.byref(ports[channel+2], 4*offset), c.c_void_p))
        desc.contents.run(instance, count)
    elapsed = time.perf_counter() - started
    output = (list(ports[2]), list(ports[3]))
    desc.contents.cleanup(instance)
    assert all(math.isfinite(v) and abs(v) <= 2 for ch in output for v in ch)
    return output, (list(ports[0]), list(ports[1])), elapsed


dry, source, _ = render()
assert max(abs(a-b) for a, b in zip(dry[0], source[0])) < 1e-6
assert max(abs(x) for x in dry[1]) < 1e-6

low, _, cpu = render(changes={0: {4: 1, 7: 1, 8: 1}})
high, _, _ = render(changes={0: {6: 1, 7: 1, 8: 1}})
tail = slice(11500, 16000)
assert rms(low[0][tail]) > rms(high[0][tail]) * 4

cut, _, _ = render(changes={0: {9: -1}})
assert rms(cut[0][2000:4000]) < rms(dry[0][2000:4000]) * .6

zero_drive, _, _ = render(changes={0: {9: 1, 12: 3, 13: 0}})
assert max(abs(a-b) for a, b in zip(zero_drive[0], dry[0])) < 1e-5
zero_drive_hot, hot_source, _ = render(changes={0: {9: 1, 12: 3, 13: 0}}, amplitude=.95)
assert max(abs(a-b) for a, b in zip(zero_drive_hot[0], hot_source[0])) < 1e-5

combined, _, _ = render(changes={0: {4: 1, 7: 1, 8: 1, 9: -1}})
assert rms(combined[0][tail]) > rms(cut[0][tail]) * 4

release, _, _ = render(changes={0: {4: 1, 7: 1, 8: 1}, 11008: {4: 0, 16: 1}})
assert rms(release[0][15000:17000]) > 1e-5

mute, _, _ = render(changes={0: {4: 1, 7: 1, 8: 1}, 11008: {4: 0, 16: 2}})
assert rms(mute[0][15000:17000]) > 1e-5
assert max(abs(v) for v in mute[1]) < 1e-6

fade, fade_source, _ = render(changes={0: {4: 1, 7: 1, 8: 1},
                                       11008: {4: 0, 16: 2}, 22016: {16: 3}},
                              signal_seconds=1)
assert abs(fade[0][22016] - fade[0][22015]) < .08
assert rms(fade[0][39000:43000]) > rms(fade_source[0][39000:43000]) * .9

hot, _, _ = render(changes={0: {4: 1, 5: 1, 6: 1, 7: 1, 8: 1,
                               9: 1, 10: 1, 11: 1, 12: 1, 13: 1}},
                   signal_seconds=1, amplitude=.8)
assert max(abs(v) for v in hot[0]) <= 1.0, 'Combined FX exceeded full scale'

transition, _, _ = render(changes={0: {4: 1, 7: 1, 8: 1},
                                   11008: {15: 1}, 22016: {7: 3}},
                          signal_seconds=.8)
for change in (11008, 22016):
    assert abs(transition[0][change] - transition[0][change-1]) < .08

filter_dry, _, _ = render(freq=8000, signal_seconds=1)
filter_low, _, _ = render(freq=8000, signal_seconds=1,
                          changes={0: {9: -1, 10: -1, 11: -1, 12: 4, 13: 1}})
assert rms(filter_low[0][10000:20000]) < rms(filter_dry[0][10000:20000]) * .75

early, _, _ = render(signal_seconds=1, changes={0: {9: 1, 10: 1, 11: 1,
                                                       12: 5, 13: 1, 17: 0}})
late, _, _ = render(signal_seconds=1, changes={0: {9: -1, 10: -1, 11: -1,
                                                      12: 5, 13: 1, 17: 0}})
on_beat = slice(200, 1600)
off_beat = slice(10000, 12000)
assert rms(early[0][on_beat]) > rms(late[0][on_beat]) * 1.3
assert rms(late[0][off_beat]) > rms(early[0][off_beat]) * 1.3

switch, _, _ = render(changes={0: {9: .8, 12: 4, 13: 1}, 11008: {12: 5, 17: .25}},
                      signal_seconds=1)
assert abs(switch[0][11008] - switch[0][11007]) < .08

print('PASS: LV2 ABI, dry/stereo, bands, Filter/Ducker, release, headroom and transitions')
print('One-second stereo processing time: %.3f s' % cpu)


def check_reactivation(rate, release):
    """A reused, wrapped ring must match a new instance after activation.

    Includes fractional long/short taps and release tails before the replacement
    history has filled; silence must not expose a previous performance.
    """
    count = 1103
    ports = [(c.c_float * count)() for _ in range(4)]
    fresh_output = [(c.c_float * count)() for _ in range(2)]
    controls = [c.c_float(v) for v in
                (1, 1, 1, 2, .8, .7, .6, .5, 1, .75, 83.3, .7, 0, -1)]
    used = desc.contents.instantiate(desc, rate, None, None)
    fresh = desc.contents.instantiate(desc, rate, None, None)
    assert used and fresh
    try:
        for instance in (used, fresh):
            for index, port in enumerate(ports[:2]):
                desc.contents.connect_port(instance, index, c.cast(port, c.c_void_p))
            for index, port in enumerate(controls, 4):
                desc.contents.connect_port(instance, index, c.cast(c.pointer(port), c.c_void_p))
        for index in range(2):
            desc.contents.connect_port(used, index + 2, c.cast(ports[index + 2], c.c_void_p))
            desc.contents.connect_port(fresh, index + 2, c.cast(fresh_output[index], c.c_void_p))
        desc.contents.activate(used)
        # Fill and wrap every ring with a nonzero signal and both wet sections.
        for frame in range(count):
            ports[0][frame] = .2 * math.sin(2 * math.pi * 700 * frame / rate)
            ports[1][frame] = .15 * math.cos(2 * math.pi * 1500 * frame / rate)
        for _ in range(math.ceil(3 * rate / count)):
            desc.contents.run(used, count)
        controls[12].value = release
        desc.contents.activate(used)
        desc.contents.activate(fresh)
        for block in range(math.ceil(3 * rate / count)):
            if block == 20:
                controls[10].value = 40
                controls[11].value = 1
            elif block == 80:
                controls[10].value = 83.3
                controls[11].value = .2
            # First second is silent. Then compare real input past ring wrap.
            for frame in range(count):
                ports[0][frame] = (0 if block * count < rate else
                                   .12 * math.sin(2 * math.pi * 400 * frame / rate))
                ports[1][frame] = 0
            desc.contents.run(used, count)
            desc.contents.run(fresh, count)
            for channel in range(2):
                assert max(abs(a - b) for a, b in
                           zip(ports[channel + 2], fresh_output[channel])) < 1e-6
                if block * count < rate:
                    assert max(abs(v) for v in ports[channel + 2]) < 1e-7
    finally:
        desc.contents.cleanup(used)
        desc.contents.cleanup(fresh)


for reset_rate in (44100, 48000, 96000):
    for reset_release in (0, 1, 2):
        check_reactivation(reset_rate, reset_release)
print('PASS: repeated activation cannot leak old stereo long/short echoes or release tails, including ring wrap')

# Native owner freezes must return audio to dry on the audio thread. The
# deadman tolerates 1.5 s of missing heartbeats (GUI stalls during a track
# load), so a short stall keeps the effect and a frozen owner still goes dry.
for sample_rate in (44100, 48000):
    expired, original, _ = render(seconds=2, rate=sample_rate, signal_seconds=2,
        changes={0:{4:1,7:3,8:1,18:1}})
    stalled = slice(int(sample_rate*1.2), int(sample_rate*1.45))
    assert max(abs(a-b) for a,b in zip(expired[0][stalled], original[0][stalled]))>.005
    start=int(sample_rate*1.7)
    assert max(abs(a-b) for a,b in zip(expired[0][start:], original[0][start:]))<1e-4
    alive, _, _ = render(seconds=2, rate=sample_rate, signal_seconds=2,
        changes={offset:{18:offset+1,4:1,7:3,8:1} for offset in range(0,2*sample_rate,256)})
    assert max(abs(a-b) for a,b in zip(alive[0][start:], original[0][start:]))>.005
print('PASS: native audio-thread heartbeat survives a 1.4 s stall, expires to dry after 1.5 s; '
      'regular updates retain the effect')


# ---------------------------------------------------------------------------
# Optimised processor vs. the unoptimised reference on generated signals.
# ---------------------------------------------------------------------------
def load_descriptor(path):
    lib = c.CDLL(str(Path(path).resolve()))
    lib.lv2_descriptor.argtypes = [c.c_uint32]
    lib.lv2_descriptor.restype = c.POINTER(Descriptor)
    loaded = lib.lv2_descriptor(0)
    assert loaded and loaded.contents.URI == b'urn:decklab:threeband-performance-native-v1'
    return lib, loaded


def reference_path():
    if len(sys.argv) > 2:
        return Path(sys.argv[2])
    # CI layout: the in-source CMake build tree is the checkout itself.
    root = Path(__file__).resolve().parents[2]
    for candidate in [root / 'performance_reference.so', *sorted(root.glob('*/performance_reference.so'))]:
        if candidate.is_file():
            return candidate
    raise SystemExit('FAIL: performance_reference.so not found; pass it as the second argument')


reference_library, reference = load_descriptor(reference_path())


def stereo_signal(rate, frames, seed, amplitude=.3, gaps=(), bad_samples=()):
    """Three-band stereo test signal with silent gaps and optional NaN/inf."""
    left = (c.c_float * frames)()
    right = (c.c_float * frames)()
    f1, f2, f3 = 61.0 + seed, 690.0 + 7 * seed, 5100.0 + 31 * seed
    w1, w2, w3 = (2 * math.pi * f / rate for f in (f1, f2, f3))
    for i in range(frames):
        envelope = .55 + .45 * math.sin(2 * math.pi * .7 * i / rate)
        left[i] = amplitude * envelope * (.5 * math.sin(w1 * i) + .3 * math.sin(w2 * i) +
                                          .2 * math.sin(w3 * i))
        right[i] = amplitude * envelope * (.4 * math.sin(w1 * i + .3) + .35 * math.sin(w2 * i + 1.1) +
                                           .25 * math.sin(w3 * i + 2.0))
    for start, stop in gaps:
        for i in range(int(start * rate), min(frames, int(stop * rate))):
            left[i] = right[i] = 0.0
    for frame, value in bad_samples:
        left[int(frame * rate)] = value
        right[int(frame * rate) + 1] = value
    return left, right


def run_processor(descriptor, rate, signal, schedule, blocks):
    """Runs one instance; schedule maps seconds -> {port: value} applied at the
    first block boundary at or after that time. Returns the output arrays and
    the per-block run() times in seconds."""
    left, right = signal
    frames = len(left)
    outputs = [(c.c_float * frames)(), (c.c_float * frames)()]
    defaults = [0, 0, 0, 0, .65, 0, 0, 0, 0, .65, 120, .5, 0, -1, 0]
    controls = [c.c_float(v) for v in defaults]
    instance = descriptor.contents.instantiate(descriptor, rate, None, None)
    assert instance
    for index, port in enumerate(controls, 4):
        descriptor.contents.connect_port(instance, index, c.cast(c.pointer(port), c.c_void_p))
    descriptor.contents.activate(instance)
    events = sorted((int(at * rate), changes) for at, changes in schedule.items())
    timings = []
    offset = 0
    block = 0
    try:
        while offset < frames:
            while events and events[0][0] <= offset:
                for port, value in events.pop(0)[1].items():
                    controls[port - 4].value = value
            count = min(blocks[block % len(blocks)], frames - offset)
            block += 1
            for channel, buffer in enumerate((left, right)):
                descriptor.contents.connect_port(instance, channel,
                    c.cast(c.byref(buffer, 4 * offset), c.c_void_p))
                descriptor.contents.connect_port(instance, channel + 2,
                    c.cast(c.byref(outputs[channel], 4 * offset), c.c_void_p))
            started = time.perf_counter()
            descriptor.contents.run(instance, count)
            timings.append((count, time.perf_counter() - started))
            offset += count
    finally:
        descriptor.contents.cleanup(instance)
    return outputs, timings


LEVER = {'low': 4, 'mid': 5, 'high': 6, 'mode': 7, 'amount': 8}
ISO = {'low': 9, 'mid': 10, 'high': 11, 'mode': 12, 'amount': 13}
BPM_PORT, FRACTION, RELEASE, PHASE = 14, 15, 16, 17

SCENARIOS = {
    # name: (seconds, schedule, signal options)
    'idle': (3, {}, {}),
    'lever-echo': (4, {0: {7: 1, 4: 1}, 1.2: {5: 1}, 2.1: {4: 0, 5: 0}}, {}),
    'lever-short-echo': (4, {0: {7: 2, 6: 1, 8: .9}, 1.7: {6: 0}}, {}),
    'lever-solo': (3, {0: {7: 3, 5: 1, 8: .4}, 1.3: {8: 1}, 2.2: {5: 0}}, {}),
    'iso-cut-and-echo': (4, {0: {12: 1, 9: -.8, 10: .7}, 1.5: {9: 0, 10: 0}}, {}),
    'iso-short-echo': (3, {0: {12: 2, 11: .9, 13: 1}, 1.6: {11: 0}}, {}),
    'iso-drive': (3, {0: {12: 3, 10: .9, 13: 1}, 1.1: {10: .3}, 2.0: {10: 0}}, {'amplitude': .8}),
    'iso-filter': (3, {0: {12: 4, 9: -1, 10: -1, 11: -.5, 13: 1}, 1.4: {9: 1, 10: 1, 11: 1}}, {}),
    'iso-ducker': (4, {0: {12: 5, 9: .9, 10: -.9, 13: 1, 17: .1}, 1.0: {17: .4}, 2.0: {14: 128, 17: .9},
                       3.0: {9: 0, 10: 0}}, {}),
    'mode-crossfades': (5, {0: {7: 1, 12: 1, 4: 1, 10: .6}, .8: {7: 2, 12: 5}, 1.5: {7: 3, 12: 4},
                           2.2: {7: 0, 12: 2}, 2.9: {12: 3, 7: 1}, 3.6: {12: 0, 7: 0}}, {}),
    'release-echo': (4, {0: {7: 1, 4: 1, 5: 1}, 1.0: {4: 0, 5: 0, 16: 1}, 2.0: {16: 0}}, {}),
    'release-mute': (4, {0: {7: 2, 6: 1, 12: 1, 11: .8}, 1.1: {6: 0, 16: 2}, 2.3: {16: 0, 11: 0}}, {}),
    'release-fade-in': (4, {0: {7: 1, 4: 1}, .9: {4: 0, 16: 2}, 1.8: {16: 3}, 2.9: {16: 0}}, {}),
    'release-with-held-lever': (3, {0: {7: 1, 4: 1, 12: 2, 9: .7}, 1.0: {16: 1}, 2.0: {16: 0}}, {}),
    'tempo-and-fraction': (5, {0: {7: 1, 4: 1, 12: 2, 10: .8}, 1.0: {14: 87.5}, 2.0: {15: .25},
                              3.0: {14: 174, 15: 1}, 4.0: {15: .0625}}, {}),
    'everything-hot': (3, {0: {4: 1, 5: 1, 6: 1, 7: 1, 8: 1, 9: 1, 10: 1, 11: 1, 12: 1, 13: 1}},
                       {'amplitude': .8}),
    'idle-active-idle': (12, {0: {7: 1, 4: 1}, .6: {4: 0}, 6.5: {12: 2, 10: .9}, 7.4: {10: 0}},
                         {'gaps': ((9.5, 10.5),)}),
    'nan-and-inf-input': (3, {0: {7: 2, 5: 1, 12: 4, 9: -.6}},
                          {'bad_samples': ((.5, float('nan')), (1.5, float('inf')), (2.5, -float('inf')))}),
}

# -80 dBFS. The optimised processor re-anchors its beat phasor every 64 frames
# and snaps inaudible (< -120 dB) control tails to zero, so it is not bit exact.
EQUIVALENCE_TOLERANCE = 1e-4
worst = 0.0
for rate in (44100, 48000):
    for index, (name, (seconds, schedule, options)) in enumerate(SCENARIOS.items()):
        frames = int(seconds * rate)
        signal = stereo_signal(rate, frames, index, **options)
        blocks = (1024,) if index % 2 == 0 else (1024, 333, 64, 1)
        expected, _ = run_processor(reference, rate, signal, schedule, blocks)
        actual, _ = run_processor(desc, rate, signal, schedule, blocks)
        difference = max(abs(a - b) for channel in range(2)
                         for a, b in zip(expected[channel], actual[channel]))
        assert math.isfinite(difference), name
        worst = max(worst, difference)
        if rate == 44100:
            print('  %-24s max |optimised - reference| = %.2e' % (name, difference))
        assert difference <= EQUIVALENCE_TOLERANCE, (name, rate, difference)
print('PASS: optimised processor matches the reference within %.0e (worst %.2e) across %d scenarios '
      'at 44.1/48 kHz and odd block sizes' % (EQUIVALENCE_TOLERANCE, worst, len(SCENARIOS)))


# ---------------------------------------------------------------------------
# Processing time per 1024-frame block, reported but not gated.
# ---------------------------------------------------------------------------
BENCHMARKS = {
    'idle (all sections off)': {},
    'lever echo, low+mid': {0: {7: 1, 4: 1, 5: 1}},
    'isolator ducker': {0: {12: 5, 9: .9, 10: .9, 11: .9, 13: 1}},
    'release tail': {0: {7: 1, 4: 1}, .5: {4: 0, 16: 1}},
    'all sections hot': {0: {4: 1, 5: 1, 6: 1, 7: 2, 8: 1, 9: 1, 10: 1, 11: 1, 12: 1, 13: 1}},
}
print('Median run() time per 1024-frame stereo block at 44.1 kHz (reference -> optimised):')
bench_signal = stereo_signal(44100, 44100 * 4, 3)
for name, schedule in BENCHMARKS.items():
    medians = []
    for descriptor in (reference, desc):
        _, timings = run_processor(descriptor, 44100, bench_signal, schedule, (1024,))
        steady = [seconds for count, seconds in timings[len(timings) // 2:] if count == 1024]
        medians.append(statistics.median(steady) * 1e3)
    print('  %-26s %.3f ms -> %.3f ms (%.1fx)' % (name, medians[0], medians[1],
                                                 medians[0] / max(medians[1], 1e-9)))
