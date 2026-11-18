# SPDX-License-Identifier: Apache-2.0
"""Every decoder against the corpus in tests/assets/decoders.

Good files: the device's pixels are compared with Pillow's decoding of the
same file, an independent implementation. Lossless formats must match
exactly; blended transparency within rounding; lossy formats closely.

Bad files: truncated, damaged, or lying about their size. The device must
refuse them or stop cleanly, and be none the worse afterwards.
"""
import io
import json
import struct
import zlib
from pathlib import Path

import pytest
from PIL import Image

import lab
from test_api import Api

pytestmark = pytest.mark.live

CORPUS = Path(__file__).resolve().parents[1] / "assets" / "decoders"
TYPE = {".png": "png", ".gif": "gif", ".webp": "webp", ".jpg": "jpeg", ".bmp": "bmp"}
BACKGROUND = (0x20, 0x40, 0x60)

GOOD = sorted(p.relative_to(CORPUS / "good").as_posix() for p in (CORPUS / "good").rglob("*.*"))
BAD = sorted(p.relative_to(CORPUS / "bad").as_posix() for p in (CORPUS / "bad").rglob("*.*"))

# The frame delays each animated file declares (tests/assets/make_corpus.py).
DECLARED_MS = {
    "png/apng_sprite.png": [100, 50, 200, 100, 70],
    "png/apng_over.png": [80, 80, 80, 80],
    "png/apng_opaque.png": [40, 40, 120, 40],
    "png/apng_short_delays.png": [0, 10, 20, 100],
    "gif/opaque_anim.gif": [40, 80, 120, 200],
    "gif/sprite_disposal1.gif": [100, 60, 60, 60, 150],
    "gif/sprite_disposal2.gif": [100, 60, 60, 60, 150],
    "gif/sprite_disposal3.gif": [100, 60, 60, 60, 150],
    "gif/short_delays.gif": [0, 10, 20, 100],
    "webp/anim_lossless_alpha.webp": [100, 50, 200, 100, 70],
    "webp/anim_opaque.webp": [40, 40, 120, 40],
    "webp/anim_short_delays.webp": [0, 10, 20, 100],
    "webp/anim_lossy.webp": [60, 60, 60, 60],
}
# Formats that lose information: compared by closeness, not equality.
LOSSY = {"jpeg/", "webp/lossy", "webp/anim_lossy"}


def shown_for(declared_ms: int) -> int:
    """The browser rule (decision D37): 10 ms or less is shown for 100 ms."""
    return 100 if declared_ms <= 10 else declared_ms


def blend(rgba: Image.Image, background) -> bytes:
    """RGBA onto a background colour, rounding to nearest, as RGB bytes."""
    out = bytearray()
    for r, g, b, a in rgba.get_flattened_data():
        if a == 255:
            out += bytes((r, g, b))
        else:
            out += bytes((c * a + bg * (255 - a) + 127) // 255 for c, bg in zip((r, g, b), background))
    return bytes(out)


def png_chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def apng_frames(path: Path) -> list[Image.Image]:
    """An APNG's frames composited as the specification says, as RGBA images.

    Pillow's own APNG reader is not used for this: where a translucent pixel
    is drawn "over" another translucent pixel it keeps the alpha of the new
    one, while the specification asks for true alpha compositing (two layers
    of 63% opacity make 86%). Pillow only decodes each frame's pixels here.
    """
    raw = path.read_bytes()
    chunks, pos = [], 8
    while pos < len(raw):
        length, kind = struct.unpack(">I4s", raw[pos:pos + 8])
        chunks.append((kind, raw[pos + 8:pos + 8 + length]))
        pos += 12 + length
    ihdr = next(body for kind, body in chunks if kind == b"IHDR")
    width, height = struct.unpack(">II", ihdr[:8])
    shared = [(k, b) for k, b in chunks if k in (b"PLTE", b"tRNS", b"gAMA", b"sRGB")]

    frames, control, data = [], None, b""
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    previous = None

    def finish():
        nonlocal canvas, previous
        if control is None or not data:
            return
        _, w, h, x, y, _, _, dispose, blend = struct.unpack(">IIIIIHHBB", control)
        png = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">II", w, h) + ihdr[8:]) +
               b"".join(png_chunk(k, b) for k, b in shared) + png_chunk(b"IDAT", data) + png_chunk(b"IEND", b""))
        part = Image.open(io.BytesIO(png)).convert("RGBA")
        before = canvas.copy()
        if blend == 0:
            canvas.paste(part, (x, y))
        else:
            region = canvas.crop((x, y, x + w, y + h))
            canvas.paste(Image.alpha_composite(region, part), (x, y))
        frames.append(canvas.copy())
        if dispose == 1:
            canvas.paste(Image.new("RGBA", (w, h), (0, 0, 0, 0)), (x, y))
        elif dispose == 2:
            canvas = previous.copy() if previous is not None else before
        if dispose != 2:
            previous = canvas.copy()

    for kind, body in chunks:
        if kind == b"fcTL":
            finish()
            control, data = body, b""
        elif kind == b"IDAT" and control is not None:
            data += body
        elif kind == b"fdAT":
            data += body[4:]
    finish()
    return frames


def reference_frames(name: str, background) -> list[bytes]:
    """An independent decoding of the file: every frame, composited, as RGB bytes."""
    if name.startswith("png/apng_"):
        return [blend(frame, background) for frame in apng_frames(CORPUS / "good" / name)]
    image = Image.open(CORPUS / "good" / name)
    if name == "png/gray16.png":
        image = image.point(lambda v: v / 257).convert("L")
    if name == "jpeg/larger_than_screen.jpg":
        image = image.reduce(2)  # the device scales this one to the panel while decoding
    frames = []
    for index in range(getattr(image, "n_frames", 1)):
        image.seek(index)
        frames.append(blend(image.convert("RGBA"), background))
    return frames


def difference(a: bytes, b: bytes) -> tuple[int, float]:
    assert len(a) == len(b), f"{len(a)} bytes against {len(b)}"
    deltas = [abs(x - y) for x, y in zip(a, b)]
    return max(deltas), sum(deltas) / len(deltas)


class Decoder:
    def __init__(self, api: Api):
        self.api = api

    def describe(self, folder: str, name: str, frames: int = 64):
        data = (CORPUS / folder / name).read_bytes()
        kind = TYPE[Path(name).suffix]
        return self.api.call("POST", f"/api/v2/test/decode?type={kind}&frames={frames}", data, timeout=60)

    def frame(self, name: str, index: int) -> bytes:
        data = (CORPUS / "good" / name).read_bytes()
        kind = TYPE[Path(name).suffix]
        reply = self.api.call("POST", f"/api/v2/test/decode?type={kind}&frame={index}", data, timeout=60)
        assert reply.status == 200, reply.body[:200]
        return reply.body


@pytest.fixture(scope="module")
def decoder(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    api.call("PATCH", "/api/v2/settings", {"background_color": "#%02x%02x%02x" % BACKGROUND})
    yield Decoder(api)
    api.call("PATCH", "/api/v2/settings", {"background_color": "#000000"})


def test_the_corpus_is_what_the_generator_makes():
    assert len(GOOD) >= 39 and len(BAD) >= 41
    assert {name.split("/")[0] for name in GOOD} == {"png", "gif", "webp", "jpeg", "bmp"}
    assert set(DECLARED_MS) <= set(GOOD)


@pytest.mark.parametrize("name", GOOD)
def test_good_file_decodes_to_what_pillow_sees(decoder, uart, name):
    expected = reference_frames(name, BACKGROUND)
    info = decoder.describe("good", name).data
    width, height = Image.open(CORPUS / "good" / name).size
    if name == "jpeg/larger_than_screen.jpg":
        width, height = width // 2, height // 2
    assert (info["width"], info["height"]) == (width, height)
    assert info["frame_count"] == len(expected)
    assert info["stack_used"] < 12 * 1024, "decoding must stay light on stack"

    lossy = any(name.startswith(prefix) for prefix in LOSSY)
    for index, wanted in enumerate(expected):
        got = decoder.frame(name, index)
        worst, mean = difference(got, wanted)
        where = f"{name} frame {index}: worst {worst}, mean {mean:.2f}"
        if name == "jpeg/baseline_444.jpg":
            # The hardware decoder with full-resolution colour: all but exact.
            assert mean < 0.5 and worst <= 4, where
        elif name == "jpeg/baseline.jpg":
            # The hardware decoder with subsampled colour. It repeats each
            # colour sample where libjpeg interpolates, and this picture has
            # harsh colour edges; brightness still matches closely.
            assert mean < 6.0, where
        elif lossy:
            limit = 6.0 if "larger_than_screen" in name else 3.0
            assert mean < limit and worst < 80, where
        elif info["has_transparency"]:
            assert worst <= 2, where  # rounding in the blend
        else:
            assert worst == 0, where

    if len(expected) > 1:
        shown = [shown_for(ms) for ms in DECLARED_MS[name]]
        delays = [frame["delay_ms"] for frame in info["frames"]]
        assert delays[:len(shown)] == shown
        assert info["source_consumed"] is False, "an animation keeps reading the file"
        assert info["reset_ok"] is True, "after a reset the first frame comes out the same"
        if name.startswith("gif/"):
            # GIF starts again by itself; the second pass repeats the first.
            assert info["decoded"] == 64 and info["stopped"] == ""
            crcs = [frame["crc"] for frame in info["frames"]]
            assert crcs[len(expected):2 * len(expected)] == crcs[:len(expected)]
            assert delays[len(expected):2 * len(expected)] == shown
        else:
            assert info["decoded"] == len(expected) and info["stopped"] == "ESP_ERR_INVALID_STATE"
    else:
        assert info["decoded"] == 1
        assert info["frames"][0]["delay_ms"] == 100, "a still is shown in steps of 100 ms"


def test_a_transparent_still_follows_the_background_colour(decoder):
    name = "png/rgba_soft_alpha.png"
    try:
        for colour in ((255, 255, 255), (0, 0, 0), (200, 30, 90)):
            decoder.api.call("PATCH", "/api/v2/settings", {"background_color": "#%02x%02x%02x" % colour})
            worst, _ = difference(decoder.frame(name, 0), reference_frames(name, colour)[0])
            assert worst <= 2, colour
        # And an animation, on its next frame.
        name = "webp/anim_lossless_alpha.webp"
        worst, _ = difference(decoder.frame(name, 2), reference_frames(name, (200, 30, 90))[2])
        assert worst <= 2
    finally:
        decoder.api.call("PATCH", "/api/v2/settings", {"background_color": "#%02x%02x%02x" % BACKGROUND})


@pytest.mark.parametrize("name", BAD)
def test_bad_file_is_refused_or_stops_cleanly(decoder, uart, name):
    reply = decoder.describe("bad", name)
    if reply.status == 422:
        reply.error(422, "NOT_DECODED")
    else:
        # Enough of the file was intact to open it. It must then either stop
        # with an error or produce frames of the size it announced; what the
        # frames show is not specified.
        info = reply.data
        assert 0 < info["width"] <= 4096 and 0 < info["height"] <= 4096
        assert info["decoded"] <= 64
    must_refuse = ("claims_", "not_an_image", "empty_ish", "truncated_header")
    if any(mark in name for mark in must_refuse) and "claims_5000_wide" not in name:
        assert reply.status == 422, f"{name} should not have opened"
    assert uart.command("mem")["level"] == "ok"


def test_decoding_everything_again_leaves_no_memory_behind(decoder, uart):
    def psram_free() -> int:
        return decoder.describe("good", "png/one_pixel.png").data["psram_free"]

    before = psram_free()
    for _ in range(2):
        for name in GOOD:
            if "larger_than_screen" not in name:
                assert decoder.describe("good", name).status == 200
        for name in BAD:
            decoder.describe("bad", name)
    after = psram_free()
    assert before - after < 64 * 1024, f"PSRAM free went from {before} to {after}"
    assert uart.command("mem")["level"] == "ok"
