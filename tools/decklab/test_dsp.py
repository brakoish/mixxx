"""Signal and continuity checks for the standalone performance LV2."""
import ctypes as c
import math
from pathlib import Path
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
assert desc and desc.contents.URI == b'urn:decklab:threeband-performance'


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

# Native owner freezes must return audio to dry on the audio thread.
for sample_rate in (44100, 48000):
    expired, original, _ = render(seconds=1, rate=sample_rate, signal_seconds=1,
        changes={0:{4:1,7:3,8:1,18:1}})
    start=int(sample_rate*.85)
    assert max(abs(a-b) for a,b in zip(expired[0][start:], original[0][start:]))<1e-4
    alive, _, _ = render(seconds=1, rate=sample_rate, signal_seconds=1,
        changes={offset:{18:offset+1,4:1,7:3,8:1} for offset in range(0,sample_rate,256)})
    assert max(abs(a-b) for a,b in zip(alive[0][start:], original[0][start:]))>.005
print('PASS: native audio-thread heartbeat expires to dry; regular updates retain the effect')
