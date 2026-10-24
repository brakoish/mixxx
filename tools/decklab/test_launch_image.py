"""The DeckLab loading screen's numbers match its images.

res/skins/DeckLab/boot/launch.png is the Deck Lab boot screen's last frame; the progress bar in the
skin's <LaunchImageStyle> must sit exactly on its waveform, starting at the parked playhead (the cue).
This finds the playhead and the waveform in launch.png and checks the QProgressBar margins and size.

    python -m unittest tools/decklab/test_launch_image.py   (test_build_pi.py, which CI runs, imports it)

Qt centres the QProgressBar box (margin-left + contents, no right margin) in the window, like the
background image, so the contents start at (W + margin_left - width) / 2. With the contents ending at
the waveform's right edge, that is the cue when margin_left = cue + wave_right - W, wherever the art
sits. The same for rows: margin_top = 2 * wave_y + rows - H.
"""
from pathlib import Path
import re
import struct
import unittest
import zlib

SKIN = Path(__file__).resolve().parents[2] / "res/skins/DeckLab"


def read_rgb(path):
    """8-bit RGB PNG whose rows all use filter 0 (as Deck Lab's brand/build_assets.py writes them)."""
    data = path.read_bytes()
    pos, idat = 8, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", data[pos + 8:pos + 18])
        elif kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + length]
        pos += 12 + length
    assert depth == 8 and colour == 2, path
    raw = zlib.decompress(idat)
    stride = width * 3
    rows = []
    for y in range(height):
        line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        assert line[0] == 0, "%s: row %d is not filter 0 (regenerate with build_assets.py)" % (path, y)
        rows.append(line[1:])
    return width, height, rows


def is_red(px):
    return px[0] > 200 and px[1] < 120 and px[2] < 120


class LaunchImageGeometry(unittest.TestCase):
    def test_progress_bar_sits_on_the_waveform_from_the_cue(self):
        width, height, rows = read_rgb(SKIN / "boot/launch.png")
        self.assertEqual((width, height), (800, 480))

        def pixel(x, y):
            return tuple(rows[y][x * 3:x * 3 + 3])
        # The boot playhead: red rows below the wordmark (its own playhead ends above row 220).
        red_rows = [y for y in range(220, height) if any(is_red(pixel(x, y)) for x in range(width))]
        top, bottom = red_rows[0], red_rows[-1]
        wave_y, wave_rows = top + 10, bottom - top + 1 - 20     # the playhead reaches 10 px past each side
        mid = (top + bottom) // 2
        red_cols = [x for x in range(width) if is_red(pixel(x, mid))]
        cue = (red_cols[0] + red_cols[-1] + 1) // 2
        lit = [x for x in range(width) if any(max(pixel(x, y)) > 60 and not is_red(pixel(x, y))
                                              for y in range(wave_y, wave_y + wave_rows))]
        wave_right = lit[-1] + 1

        skin = (SKIN / "skin.xml").read_text(encoding="utf-8")
        style = re.search(r"<LaunchImageStyle>(.*?)</LaunchImageStyle>", skin, re.S).group(1)
        bar = dict((k.strip(), v.strip()) for k, v in (d.split(":", 1) for d in
                   re.search(r"QProgressBar\s*\{([^}]*)\}", style).group(1).split(";") if ":" in d))
        margin = (2 * wave_y + wave_rows - height, cue + wave_right - width)
        self.assertEqual(bar["margin"], "%dpx 0 0 %dpx" % margin)
        for key, value in (("min-width", wave_right - cue), ("max-width", wave_right - cue),
                           ("min-height", wave_rows), ("max-height", wave_rows)):
            self.assertEqual(bar[key], "%dpx" % value, key)
        self.assertIn("qproperty-spacing: 0;", style)
        lw, lh, _ = read_rgb(SKIN / "boot/loading.png")
        self.assertEqual((lw, lh), (wave_right - cue, wave_rows), "the chunk image covers the bar exactly")


if __name__ == "__main__":
    unittest.main()
