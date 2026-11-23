#!/usr/bin/env python3
"""Turn a frame capture (CTRL -> Capture one frame, or the frame_capture_after_s cvar) into PNG images.

Usage: python -I ds2_frame_capture.py <capture> [--out DIR] [--ref <reference capture>] [--full-spans]

<capture> is the frame_capture folder (Android/data/<package>/files/logs/frame_capture) or a log zip from
CTRL -> Save logs (its logs/frame_capture/ entries are used). With --ref (for example the AYN Thor's capture of
the same place), index.html shows both side by side, resolve by resolve.

The capture (VulkanCommandProcessor::FinishResolveDump) has, for one frame:
  r###_ADDR.bin  every resolve's destination in guest memory (tiled Xenos 2D layout, guest endianness);
                 ### is the resolve's number in the frame
  e###_tNNNN.bin the EDRAM tiles that resolve read (the render target as the resolve saw it; host EDRAM layout:
                 80x16 32-bit samples per tile, 64bpp formats 40x16 x 2 words, 2x MSAA doubles the rows, 4x the
                 rows and the columns)
  t###_ADDR.bin  render-to-texture textures the frame samples (the host image, level 0, as the GPU reads it)
  index.txt      one line per file with its registers / layout; draws.txt every draw; capture.txt, cvars.txt
Writes <out>/png/*.png, <out>/index.html and prints one line per image (min / max / mean), so the first image that
goes wrong in a frame shows where the colours break.
"""
import argparse
import html
import os
import re
import sys
import zipfile

import numpy as np
from PIL import Image

HEX_KEYS = {"addr", "dest_base", "dest_info", "dest_pitch", "copy_control", "surface_info", "color_info0",
            "depth_info", "dest", "base", "host_swizzle"}

# xenos::ColorFormat (resolve destination formats): name, log2 bytes per pixel.
COPY_FORMATS = {2: ("8", 0), 3: ("1_5_5_5", 1), 4: ("5_6_5", 1), 6: ("8888", 2), 7: ("2_10_10_10", 2),
                10: ("8_8", 1), 15: ("4_4_4_4", 1), 16: ("10_11_11", 2), 17: ("11_11_10", 2), 24: ("16", 1),
                25: ("16_16", 2), 26: ("16x4", 3), 30: ("16F", 1), 31: ("16_16F", 2), 32: ("16x4F", 3),
                36: ("32F", 2), 37: ("32_32F", 3), 38: ("32x4F", 4)}
# xenos::ColorRenderTargetFormat
RT_FORMATS = {0: "8888", 1: "8888_gamma", 2: "2_10_10_10", 3: "7e3", 4: "16_16", 5: "16x4", 6: "16_16F",
              7: "16x4F", 10: "2_10_10_10_as_10x4", 12: "7e3_as_16x4", 14: "32F", 15: "32_32F"}
# VkFormat -> name, bytes per texel
VK_FORMATS = {2: ("R4G4B4A4", 2), 3: ("B4G4R4A4", 2), 4: ("R5G6B5", 2), 5: ("B5G6R5", 2), 6: ("R5G5B5A1", 2),
              8: ("A1R5G5B5", 2), 9: ("R8", 1), 10: ("R8_SNORM", 1), 13: ("R8_UINT", 1), 16: ("R8G8", 2),
              17: ("R8G8_SNORM", 2), 37: ("RGBA8", 4), 38: ("RGBA8_SNORM", 4), 41: ("RGBA8_UINT", 4),
              43: ("RGBA8_SRGB", 4), 44: ("BGRA8", 4), 51: ("ABGR8_PACK32", 4), 58: ("A2R10G10B10", 4),
              64: ("A2B10G10R10", 4), 70: ("R16", 2), 71: ("R16_SNORM", 2), 74: ("R16_UINT", 2),
              76: ("R16F", 2), 77: ("RG16", 4), 78: ("RG16_SNORM", 4), 83: ("RG16F", 4), 91: ("RGBA16", 8),
              92: ("RGBA16_SNORM", 8), 97: ("RGBA16F", 8), 98: ("R32_UINT", 4), 100: ("R32F", 4),
              101: ("RG32_UINT", 8), 103: ("RG32F", 8), 107: ("RGBA32_UINT", 16), 109: ("RGBA32F", 16),
              122: ("B10G11R11F", 4), 123: ("E5B9G9R9", 4)}


# ---------------------------------------------------------------------------------------------------------------
# Input


def parse_index_line(line):
    parts = line.split()
    entry = {"name": parts[0], "kind": parts[0][0]}
    for p in parts[1:]:
        if "=" not in p:
            continue
        k, v = p.split("=", 1)
        if k in HEX_KEYS:
            try:
                entry[k] = int(v, 16)
                continue
            except ValueError:
                pass
        try:
            entry[k] = int(v)
        except ValueError:
            entry[k] = v
    return entry


def load_capture(path, work_dir):
    """Returns (folder with the files, list of entries, header lines)."""
    if os.path.isfile(path) and zipfile.is_zipfile(path):
        folder = os.path.join(work_dir, "raw")
        os.makedirs(folder, exist_ok=True)
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                parts = info.filename.replace("\\", "/").split("/")
                if len(parts) < 2 or parts[-2] != "frame_capture" or not parts[-1]:
                    continue
                name = os.path.basename(parts[-1])
                if not re.fullmatch(r"[A-Za-z0-9_.\-]+", name):
                    continue
                with z.open(info) as src, open(os.path.join(folder, name), "wb") as dst:
                    dst.write(src.read())
    else:
        folder = path
    index = os.path.join(folder, "index.txt")
    if not os.path.isfile(index):
        sys.exit("no index.txt in %s (a frame_capture folder or a Save logs zip with one)" % path)
    entries, header = [], []
    for line in open(index, encoding="utf-8", errors="replace"):
        line = line.strip()
        if re.match(r"^[ret]\d{3}_", line):
            entries.append(parse_index_line(line))
        elif line:
            header.append(line)
    for extra in ("capture.txt",):
        p = os.path.join(folder, extra)
        if os.path.isfile(p):
            header = [l.strip() for l in open(p, encoding="utf-8", errors="replace") if l.strip()] + header
    return folder, entries, header


# ---------------------------------------------------------------------------------------------------------------
# Pixel helpers


def to_u8(x):
    return (np.clip(np.nan_to_num(x, nan=0.0, posinf=1.0, neginf=0.0), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def stretch(x):
    """Contrast stretch (depth, signed or unbounded data) to 0..1."""
    x = np.nan_to_num(x.astype(np.float64))
    lo, hi = float(x.min()), float(x.max())
    return (x - lo) / (hi - lo) if hi > lo else np.zeros_like(x)


def f7e3(v10):
    v10 = v10.astype(np.uint32)
    e = (v10 >> 7) & 7
    m = (v10 & 0x7F).astype(np.float64)
    return np.where(e == 0, m / 128.0 * 0.25, (1.0 + m / 128.0) * np.exp2(e.astype(np.float64) - 3.0))


def f20e4(v24):
    v24 = v24.astype(np.uint32)
    e = (v24 >> 20) & 15
    m = (v24 & 0xFFFFF).astype(np.float64)
    return np.where(e == 0, m / 1048576.0 * 2.0 ** -14, (1.0 + m / 1048576.0) * np.exp2(e.astype(np.float64) - 15.0))


def unpack_f11(v, bits_m):
    """Unsigned small floats (5-bit exponent): 11-bit (6 mantissa) or 10-bit (5 mantissa)."""
    v = v.astype(np.uint32)
    e = (v >> bits_m) & 31
    m = (v & ((1 << bits_m) - 1)).astype(np.float64)
    scale = float(1 << bits_m)
    return np.where(e == 0, m / scale * 2.0 ** -14, (1.0 + m / scale) * np.exp2(e.astype(np.float64) - 15.0))


def stats(rgb):
    a = np.asarray(rgb, dtype=np.float64)
    return "min=%.3g max=%.3g mean=%.3g" % (np.nanmin(a), np.nanmax(a), np.nanmean(a))


# ---------------------------------------------------------------------------------------------------------------
# Resolve destinations (guest memory)


def tiled_offsets(w, h, pitch, bpb_log2):
    ys, xs = np.mgrid[0:h, 0:w].astype(np.int64)
    pitch = (pitch + 31) & ~31
    macro = ((xs >> 5) + (ys >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((xs & 7) + ((ys & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((ys & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((ys & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((ys & 8) >> 2) + (xs >> 3)) & 3) << 6) + (offset & 0x3F))


def endian_swap(a, endian):
    a = a[: len(a) // 16 * 16]
    if endian == 1:
        return a.reshape(-1, 2)[:, ::-1].reshape(-1)
    if endian == 2:
        return a.reshape(-1, 4)[:, ::-1].reshape(-1)
    if endian == 3:
        return a.reshape(-1, 2, 2)[:, ::-1, :].reshape(-1)
    if endian == 4:
        return a.reshape(-1, 8)[:, ::-1].reshape(-1)
    if endian == 5:
        return a.reshape(-1, 16)[:, ::-1].reshape(-1)
    return a


def decode_resolve(raw, e):
    info = e["dest_info"]
    endian = info & 7
    fmt = (info >> 7) & 0x3F
    swap_rb = (info >> 24) & 1
    exp_bias = (info >> 16) & 0x3F
    exp_bias = exp_bias - 64 if exp_bias & 32 else exp_bias
    pitch = e["dest_pitch"] & 0x3FFF
    height = (e["dest_pitch"] >> 16) & 0x3FFF
    fname, bpb_log2 = COPY_FORMATS.get(fmt, ("fmt%d" % fmt, 2))
    bpp = 1 << bpb_log2
    start = e["addr"] - e["dest_base"]
    if start < 0 or not pitch or not height:
        return None, "%s (no layout: start %d, %dx%d)" % (fname, start, pitch, height)
    offs = tiled_offsets(pitch, height, pitch, bpb_log2)
    total = int(offs.max()) + bpp
    buf = np.zeros(max(total, start + len(raw)) + 64, dtype=np.uint8)
    buf[start:start + len(raw)] = np.frombuffer(raw, dtype=np.uint8)
    buf = endian_swap(buf, endian)
    idx = np.clip(offs[..., None] + np.arange(bpp), 0, len(buf) - 1)
    px = np.ascontiguousarray(buf[idx])  # h, w, bpp in little-endian order after the swap
    desc = "%s %dx%d endian=%d" % (fname, pitch, height, endian)
    if bpp == 4:
        v = px.view("<u4")[..., 0]
    if fname == "8888" and (e.get("copy_control", 0) & 7) == 4:
        # A depth resolve into k_8_8_8_8: 24-bit depth over the stencil byte.
        d = (v >> 8).astype(np.float64) / 16777215.0
        img = np.repeat(to_u8(stretch(d))[..., None], 3, -1)
        desc += " depth %s stencil=%d..%d" % (stats(d), int((v & 255).min()), int((v & 255).max()))
    elif fname == "8888":
        # Memory order after the swap: R G B A, or B G R A with the copy's red/blue swap.
        rgb = px[..., [2, 1, 0]] if swap_rb else px[..., [0, 1, 2]]
        img = rgb
        desc += " a=%d..%d" % (px[..., 3].min(), px[..., 3].max())
    elif fname == "2_10_10_10":
        c = [(v >> s) & 1023 for s in (0, 10, 20)]
        if swap_rb:
            c = c[::-1]
        img = np.stack([(x >> 2) for x in c], -1).astype(np.uint8)
    elif fname in ("16_16F",):
        f = px.view("<f2").reshape(height, pitch, 2).astype(np.float32)
        img = np.stack([to_u8(f[..., 0]), to_u8(f[..., 1]), np.zeros((height, pitch), np.uint8)], -1)
        desc += " " + stats(f)
    elif fname == "16x4F":
        f = px.view("<f2").reshape(height, pitch, 4).astype(np.float32)
        img = to_u8(f[..., :3])
        desc += " rgb %s nan=%d" % (stats(f[..., :3]), int(np.isnan(f).sum()))
    elif fname == "16_16":
        s = px.view("<i2").reshape(height, pitch, 2).astype(np.float64)
        img = np.stack([to_u8(stretch(s[..., 0])), to_u8(stretch(s[..., 1])), np.zeros((height, pitch), np.uint8)], -1)
    elif fname == "16x4":
        s = px.view("<u2").reshape(height, pitch, 4).astype(np.float64) / 65535.0
        img = to_u8(s[..., :3])
    elif fname == "32F":
        f = px.view("<f4").reshape(height, pitch)
        img = to_u8(stretch(f))
        desc += " " + stats(f)
    elif fname == "32_32F":
        f = px.view("<f4").reshape(height, pitch, 2)
        img = np.stack([to_u8(stretch(f[..., 0])), to_u8(stretch(f[..., 1])), np.zeros((height, pitch), np.uint8)], -1)
    elif fname in ("10_11_11", "11_11_10"):
        if fname == "11_11_10":
            r, g, b = unpack_f11(v & 2047, 6), unpack_f11((v >> 11) & 2047, 6), unpack_f11((v >> 22) & 1023, 5)
        else:
            r, g, b = unpack_f11(v & 1023, 5), unpack_f11((v >> 10) & 2047, 6), unpack_f11((v >> 21) & 2047, 6)
        img = to_u8(np.stack([r, g, b], -1))
    elif bpp == 1:
        img = px[..., 0]
    else:
        img = px[..., : min(3, bpp)]
        if img.shape[-1] == 2:
            img = np.concatenate([img, np.zeros_like(img[..., :1])], -1)
    if exp_bias:
        desc += " exp_bias=%d" % exp_bias
    return np.ascontiguousarray(img), desc


# ---------------------------------------------------------------------------------------------------------------
# EDRAM spans (host EDRAM buffer layout)


def decode_edram(raw, e, full=False):
    sx, sy = [int(x) for x in str(e.get("scale", "1x1")).split("x")]
    pitch, rows, span_tiles = e["pitch_tiles"], e["rows"], e["span_tiles"]
    tw, th = 80 * sx, 16 * sy
    words = np.zeros(rows * pitch * tw * th, dtype=np.uint32)
    data = np.frombuffer(raw[: len(raw) // 4 * 4], dtype="<u4")
    words[: min(len(data), len(words))] = data[: len(words)]
    grid = words.reshape(rows, pitch, th, tw).transpose(0, 2, 1, 3).reshape(rows * th, pitch * tw)
    fmt64 = e.get("fmt64", 0)
    msaa = e.get("msaa", 0)
    depth = e.get("depth", 0)
    fmt = e.get("format", 0)
    # The resolve rectangle in the span (in 32-bit words / rows).
    xs = (2 if msaa >= 2 else 1) * (2 if fmt64 else 1)
    ys = 2 if msaa >= 1 else 1
    rx, ry = e["off_x"] * xs * sx, e["off_y"] * ys * sy
    x0, y0 = (rx // (80 * sx)) * (80 * sx), (ry // (16 * sy)) * (16 * sy)
    if not full:
        w, h = e["width"] * xs * sx, e["height"] * ys * sy
        grid = grid[ry - y0: ry - y0 + h, rx - x0: rx - x0 + w]
    # 64bpp: two words per sample; MSAA: sample 0 of each pixel (top left).
    if fmt64:
        grid = grid.reshape(grid.shape[0], -1, 2)
    if msaa >= 1:
        grid = grid[::2]
    if msaa >= 2:
        grid = grid[:, ::2]
    if depth:
        d = (grid >> 8).astype(np.uint32)
        val = f20e4(d) if fmt == 1 else d / 16777215.0
        img = to_u8(stretch(val))
        desc = "depth %s %s stencil=%d..%d" % ("D24FS8" if fmt == 1 else "D24S8", stats(val),
                                               int((grid & 255).min()), int((grid & 255).max()))
        return img, desc
    name = RT_FORMATS.get(fmt, "rt%d" % fmt)
    desc = name
    if name in ("8888", "8888_gamma"):
        img = np.stack([(grid >> s) & 255 for s in (0, 8, 16)], -1).astype(np.uint8)
        desc += " a=%d..%d" % (int((grid >> 24).min()), int((grid >> 24).max()))
    elif name in ("2_10_10_10", "2_10_10_10_as_10x4"):
        img = np.stack([((grid >> s) & 1023) >> 2 for s in (0, 10, 20)], -1).astype(np.uint8)
    elif name in ("7e3", "7e3_as_16x4"):
        f = np.stack([f7e3((grid >> s) & 1023) for s in (0, 10, 20)], -1)
        img = to_u8(f)
        desc += " " + stats(f)
    elif name == "16_16":
        lo = (grid & 0xFFFF).astype(np.uint16).view(np.int16).astype(np.float64) / 32767.0 * 32.0
        hi = (grid >> 16).astype(np.uint16).view(np.int16).astype(np.float64) / 32767.0 * 32.0
        img = np.stack([to_u8(stretch(lo)), to_u8(stretch(hi)), np.zeros(lo.shape, np.uint8)], -1)
        desc += " r %s g %s" % (stats(lo), stats(hi))
    elif name == "16x4":
        c = [((grid[..., i // 2] >> (16 * (i % 2))) & 0xFFFF).astype(np.uint16).view(np.int16).astype(np.float64)
             / 32767.0 * 32.0 for i in range(3)]
        f = np.stack(c, -1)
        img = to_u8(f)
        desc += " " + stats(f)
    elif name == "16_16F":
        lo = (grid & 0xFFFF).astype(np.uint16).view(np.float16).astype(np.float32)
        hi = (grid >> 16).astype(np.uint16).view(np.float16).astype(np.float32)
        img = np.stack([to_u8(lo), to_u8(hi), np.zeros(lo.shape, np.uint8)], -1)
        desc += " r %s g %s" % (stats(lo), stats(hi))
    elif name == "16x4F":
        c = [((grid[..., i // 2] >> (16 * (i % 2))) & 0xFFFF).astype(np.uint16).view(np.float16).astype(np.float32)
             for i in range(3)]
        f = np.stack(c, -1)
        img = to_u8(f)
        desc += " " + stats(f)
    elif name == "32F":
        f = grid.astype(np.uint32).view(np.float32)
        img = to_u8(stretch(f))
        desc += " " + stats(f)
    elif name == "32_32F":
        f = grid.astype(np.uint32).view(np.float32)
        img = np.stack([to_u8(stretch(f[..., 0])), to_u8(stretch(f[..., 1])), np.zeros(f.shape[:2], np.uint8)], -1)
    else:
        img = to_u8(stretch(grid.astype(np.float64)))
    if e.get("msaa"):
        desc += " msaa=%dx (sample 0)" % (1 << e["msaa"])
    return np.ascontiguousarray(img), desc


# ---------------------------------------------------------------------------------------------------------------
# Host textures


def apply_swizzle(rgba, swizzle):
    out = []
    for i in range(4):
        s = (swizzle >> (3 * i)) & 7
        if s < 4:
            out.append(rgba[..., s])
        else:
            out.append(np.full(rgba.shape[:2], 0.0 if s == 4 else 1.0))
    return np.stack(out, -1)


def decode_texture(raw, e):
    w, h, vk = e["width"], e["height"], e["vkformat"]
    name, size = VK_FORMATS.get(vk, ("vk%d" % vk, 4))
    n = w * h
    a = np.frombuffer(raw[: n * size], dtype=np.uint8)
    if len(a) < n * size:
        return None, "%s short (%d of %d bytes)" % (name, len(a), n * size)
    a = a.reshape(h, w, size)
    one = np.ones((h, w))
    zero = np.zeros((h, w))
    if name in ("RGBA8", "RGBA8_SRGB", "ABGR8_PACK32", "RGBA8_UINT"):
        rgba = a.astype(np.float64) / 255.0
    elif name == "BGRA8":
        rgba = a[..., [2, 1, 0, 3]].astype(np.float64) / 255.0
    elif name == "RGBA8_SNORM":
        rgba = a.view(np.int8).astype(np.float64) / 127.0 * 0.5 + 0.5
    elif name in ("A2B10G10R10", "A2R10G10B10"):
        v = a.view("<u4")[..., 0]
        c = [((v >> s) & 1023) / 1023.0 for s in (0, 10, 20)] + [(v >> 30) / 3.0]
        if name == "A2R10G10B10":
            c = [c[2], c[1], c[0], c[3]]
        rgba = np.stack(c, -1)
    elif name == "B10G11R11F":
        v = a.view("<u4")[..., 0]
        rgba = np.stack([unpack_f11(v & 2047, 6), unpack_f11((v >> 11) & 2047, 6), unpack_f11((v >> 22) & 1023, 5),
                         one], -1)
    elif name in ("RGBA16F", "RG16F", "R16F"):
        f = a.view("<f2").astype(np.float64)
        rgba = np.concatenate([f, np.repeat(zero[..., None], 4 - f.shape[-1], -1)], -1)
        if f.shape[-1] < 4:
            rgba[..., 3] = 1.0
    elif name in ("RGBA16", "RG16", "R16"):
        f = a.view("<u2").astype(np.float64) / 65535.0
        rgba = np.concatenate([f, np.repeat(one[..., None], 4 - f.shape[-1], -1)], -1)
    elif name in ("RGBA16_SNORM", "RG16_SNORM"):
        f = a.view("<i2").astype(np.float64) / 32767.0
        rgba = np.concatenate([f, np.repeat(one[..., None], 4 - f.shape[-1], -1)], -1)
    elif name in ("R32F", "RG32F", "RGBA32F"):
        f = a.view("<f4").astype(np.float64)
        rgba = np.concatenate([f, np.repeat(one[..., None], 4 - f.shape[-1], -1)], -1)
    elif name in ("R32_UINT", "RG32_UINT"):
        f = a.view("<u4").astype(np.float64)
        rgba = np.concatenate([stretch(f), np.repeat(one[..., None], 4 - f.shape[-1], -1)], -1)
    elif name in ("R8", "R8_UINT"):
        rgba = np.stack([a[..., 0] / 255.0, zero, zero, one], -1)
    elif name == "R8G8":
        rgba = np.stack([a[..., 0] / 255.0, a[..., 1] / 255.0, zero, one], -1)
    elif name in ("R5G6B5", "B5G6R5"):
        v = a.view("<u2")[..., 0].astype(np.uint32)
        c = [(v >> 11) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0]
        if name == "B5G6R5":
            c = c[::-1]
        rgba = np.stack(c + [one], -1)
    else:
        rgba = np.concatenate([a[..., :min(4, size)].astype(np.float64) / 255.0,
                               np.repeat(one[..., None], 4 - min(4, size), -1)], -1)
    raw_stats = stats(rgba[..., :3])
    shown = apply_swizzle(rgba, e.get("host_swizzle", 0x688))
    rgb = shown[..., :3]
    if name in ("R32F", "RG32F", "R32_UINT"):
        rgb = np.repeat(stretch(rgba[..., :1]), 3, -1)
    desc = "%s %dx%d guest_fmt=%d raw %s swizzle=%03X" % (name, w, h, e.get("guest_format", -1), raw_stats,
                                                          e.get("host_swizzle", 0))
    return to_u8(rgb), desc


# ---------------------------------------------------------------------------------------------------------------


def convert(path, out_dir, full_spans=False):
    folder, entries, header = load_capture(path, out_dir)
    png_dir = os.path.join(out_dir, "png")
    os.makedirs(png_dir, exist_ok=True)
    # EDRAM spans that wrap around the buffer's end come in two parts.
    parts = {}
    for e in entries:
        if e["kind"] == "e":
            parts.setdefault(e["seq"], []).append(e)
    rows = []
    for e in entries:
        if e["kind"] == "e" and e.get("part", 0) != 0:
            continue
        # A copy identical to an earlier one is only named in the index (same_as=).
        path_bin = os.path.join(folder, e.get("same_as", e["name"]))
        if not os.path.isfile(path_bin):
            rows.append((e, None, "missing file"))
            continue
        raw = open(path_bin, "rb").read()
        try:
            if e["kind"] == "r":
                img, desc = decode_resolve(raw, e)
            elif e["kind"] == "e":
                for extra in parts[e["seq"]][1:]:
                    raw += open(os.path.join(folder, extra.get("same_as", extra["name"])), "rb").read()
                img, desc = decode_edram(raw, e, full_spans)
            else:
                img, desc = decode_texture(raw, e)
        except Exception as ex:  # keep going on odd formats
            img, desc = None, "decode error: %s" % ex
        png = None
        if img is not None and img.size:
            png = e["name"][:-4] + ".png"
            Image.fromarray(img).save(os.path.join(png_dir, png))
            desc = "%s mean=%.1f" % (desc, float(np.asarray(img, dtype=np.float64).mean()))
        if "same_as" in e:
            desc += " (same bytes as %s)" % e["same_as"]
        rows.append((e, png, desc))
        print("%-26s draws=%-4s %s" % (e["name"], e.get("draws", "?"), desc))
    return rows, header


def entry_caption(e):
    if e["kind"] == "r":
        return "resolve %s @%08X dest_info=%08X" % (e["name"][1:4], e["addr"], e["dest_info"])
    if e["kind"] == "e":
        return "EDRAM of resolve %03d: %s base %d pitch %d %s%s (dest @%08X, %s)" % (
            e["seq"], "depth" if e.get("depth") else "color", e.get("rt_base", 0), e.get("pitch_tiles", 0),
            "%dx%d" % (e.get("width", 0), e.get("height", 0)), " direct" if e.get("direct") else "",
            e.get("dest", 0), e.get("path", "?"))
    return "texture @%08X fetch %d, written by resolve %s" % (e.get("base", 0), e.get("fetch", -1),
                                                              e.get("written_by", "?"))


def write_html(out_dir, rows, header, ref=None):
    def cell(base, row):
        if row is None:
            return "<td></td>"
        e, png, desc = row
        img = '<a href="%s/png/%s"><img src="%s/png/%s" loading="lazy"></a>' % (base, png, base, png) if png else ""
        return "<td>%s<div><b>%s</b><br>%s<br><small>%s</small></div></td>" % (
            img, html.escape(e["name"]), html.escape(entry_caption(e)), html.escape(desc))

    def key(e, counts, by_order=False):
        # The same guest address (same resolution and options) or, failing that, the n-th entry of its kind.
        k = (e["kind"],) if by_order else (e["kind"], e.get("addr", e.get("dest", e.get("base", 0))))
        counts[k] = counts.get(k, 0) + 1
        return k + (counts[k],)

    lines = ["<!doctype html><meta charset=utf-8><title>Frame capture</title><style>",
             "body{font:13px sans-serif;background:#111;color:#ddd}img{max-width:480px;image-rendering:pixelated;"
             "background:#333}td{vertical-align:top;padding:6px;border-bottom:1px solid #333}small{color:#999}",
             "</style><h2>Frame capture</h2><pre>%s</pre>" % html.escape("\n".join(header))]
    if ref is None:
        lines.append("<table>")
        for row in rows:
            lines.append("<tr>%s</tr>" % cell(".", row))
    else:
        ref_rows, ref_header, ref_base = ref
        lines.append("<p>Left: this capture. Right: reference (%s).</p><pre>%s</pre><table>" %
                     (html.escape(ref_base), html.escape("\n".join(ref_header))))
        counts, ref_counts = {}, {}
        ref_by_key = {key(r[0], ref_counts): r for r in ref_rows}
        matched = sum(1 for row in rows if key(row[0], counts) in ref_by_key)
        by_order = matched < len(rows) // 2
        if by_order:
            lines.append("<p>Few guest addresses match (another resolution?): rows paired in frame order.</p>")
            ref_counts = {}
            ref_by_key = {key(r[0], ref_counts, True): r for r in ref_rows}
        counts = {}
        used = set()
        for row in rows:
            k = key(row[0], counts, by_order)
            other = ref_by_key.get(k)
            if other is not None:
                used.add(k)
            lines.append("<tr>%s%s</tr>" % (cell(".", row), cell(ref_base, other)))
        for k, r in ref_by_key.items():
            if k not in used:
                lines.append("<tr><td>(not in this capture)</td>%s</tr>" % cell(ref_base, r))
    lines.append("</table>")
    with open(os.path.join(out_dir, "index.html"), "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture")
    ap.add_argument("--out")
    ap.add_argument("--ref", help="a reference capture (folder or zip) to show beside it")
    ap.add_argument("--full-spans", action="store_true", help="EDRAM images of the whole tile span, not the "
                                                                "resolve rectangle")
    a = ap.parse_args()
    out = a.out or (os.path.splitext(a.capture)[0] + "_png" if a.capture.endswith(".zip")
                    else os.path.join(a.capture, "out"))
    os.makedirs(out, exist_ok=True)
    rows, header = convert(a.capture, out, a.full_spans)
    ref = None
    if a.ref:
        ref_out = os.path.join(out, "ref")
        os.makedirs(ref_out, exist_ok=True)
        print("--- reference")
        ref_rows, ref_header = convert(a.ref, ref_out, a.full_spans)
        ref = (ref_rows, ref_header, "ref")
    write_html(out, rows, header, ref)
    print("wrote %s" % os.path.join(out, "index.html"))


if __name__ == "__main__":
    main()
