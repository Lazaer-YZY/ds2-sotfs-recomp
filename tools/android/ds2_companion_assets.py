"""Builds the second screen's art from the player's own game files: HUD pieces, status icons, effect icons and the
game's font, decoded from the Xbox 360 archives and upscaled 4x (Real-ESRGAN, tools/android/upscale_assets.py).

  out/venv/Scripts/python.exe -I tools/android/ds2_companion_assets.py [GAME_ROOT] [DISC2_ROOT] [OUT_DIR]
  out/venv/Scripts/python.exe -I tools/android/ds2_companion_assets.py --icons [...]   (development icon pack only)
    defaults: assets/game_root, assets/disc2, out/companion_assets

Writes OUT_DIR/companion/{hud,effect,font}/*.png, hud.json (each piece's size in game pixels) and font.json (the
glyph table), plus OUT_DIR/raw/ (the unscaled pieces). build_android.sh's stage step copies OUT_DIR/companion into the
debug APK's assets. Everything here is derived from the game: it never goes into git or a public APK.

Sources (DECISIONS.md, 2026-11-21 "Dual-screen HUD"):
  disc 1 GameData*.bhd/.bdt (BHF4 index + data, big-endian, unencrypted on the 360): menu/01.febnd (the in-game HUD:
    FE_HPgage_01 = emblem, HP/stamina bar frames, fills; Soul_Gauge = the soul counter's ends and S icon),
    menu/99.febnd (Icon_tex: a 9x9 grid of 26 px icons at a 27 px pitch; row 8, columns 5-8 = the status build-up
    icons), menu/99.febnd ItemSellect_02 (leather), menu/40.febnd In-game_01 (menu panel, ornament line),
    menu/tex/Icon/effect/EI_*.tpf (active effect icons), font/English/FeFont_Small.fontbnd.dcx (the HUD's font:
    CCM glyph table + DXT pages), menu/17.febnd Title + Title_Best_DC (the title screen's logo: the cover shown
    without a character, hud/title_logo.png, not upscaled).
  disc 2's Compatibility Pack (an STFS "PIRS" package holding Data/catalog/DlcData*.bhd/.bdt): the DLC's own
    menu/tex/Icon files (more effect icons, DLC item icons), read in place.
Formats: DCX = zlib behind a header; BND4/BHF4 = file tables; TPF = textures (format 5 DXT5, 3 DXT3, 0/1 DXT1), Xenos
2D-tiled in 4x4 blocks with 16-bit words byte-swapped (decoders from the LR port's tools/lr_gui_tex.py); format 9
A8R8G8B8, tiled per pixel.
"""
import json
import os
import re
import shutil
import struct
import sys
import zlib

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upscale_assets  # noqa: E402


# ------------------------------------------------------------------------------------------------ STFS (disc 2)
class Stfs:
    """Read-only STFS package (CON / LIVE / PIRS): the file table and each file's blocks."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(0x971)
        self.header_size = struct.unpack_from('>I', hdr, 0x340)[0]
        vd = hdr[0x379:0x379 + 0x24]
        self.per_table = 1 if (vd[2] & 1) else 2
        self.ft_count = vd[3] | (vd[4] << 8)
        self.ft_block = vd[5] | (vd[6] << 8) | (vd[7] << 16)
        self.base = (self.header_size + 0xFFF) & ~0xFFF

    def backing(self, block):
        b, base = block, 170
        for _ in range(3):
            b += ((block + base) // base) * self.per_table
            if block < base:
                break
            base *= 170
        return b

    def next_block(self, block):
        tbl = (block // 170) * (170 + self.per_table)
        if block >= 170:
            tbl += ((block // (170 * 170)) + 1) * self.per_table
            if block >= 170 * 170:
                tbl += self.per_table
        self.f.seek(self.base + tbl * 0x1000 + (block % 170) * 0x18)
        e = self.f.read(0x18)
        return (e[0x15] << 16) | (e[0x16] << 8) | e[0x17]

    def read_block(self, block, n=0x1000):
        self.f.seek(self.base + self.backing(block) * 0x1000)
        return self.f.read(n)

    def entries(self):
        out, blk = [], self.ft_block
        for i in range(self.ft_count):
            data = self.read_block(blk)
            for j in range(0, 0x1000, 0x40):
                e = data[j:j + 0x40]
                n = e[0x28] & 0x3F
                if n:
                    out.append(dict(name=e[:n].decode('latin-1'), dir=bool(e[0x28] & 0x80),
                                    contiguous=bool(e[0x28] & 0x40),
                                    start=e[0x2F] | (e[0x30] << 8) | (e[0x31] << 16),
                                    parent=struct.unpack_from('>h', e, 0x32)[0],
                                    size=struct.unpack_from('>I', e, 0x34)[0]))
            if i + 1 < self.ft_count:
                blk = self.next_block(blk)
        for e in out:
            parts, p = [e['name']], e['parent']
            while p >= 0:
                parts.insert(0, out[p]['name'])
                p = out[p]['parent']
            e['path'] = '/'.join(parts)
        return out

    def reader(self, e):
        return StfsFile(self, e)


class StfsFile:
    """A file inside the package, read by offset (only contiguous files: what the DLC package holds)."""

    def __init__(self, stfs, e):
        assert e['contiguous'], e['path']
        self.s, self.e = stfs, e

    def read_at(self, off, size):
        out, pos, end = [], off, min(off + size, self.e['size'])
        while pos < end:
            blk, inner = divmod(pos, 0x1000)
            chunk = self.s.read_block(self.e['start'] + blk)[inner:]
            out.append(chunk[:end - pos])
            pos += len(out[-1])
        return b''.join(out)


# ------------------------------------------------------------------------------------------------ archives
def undcx(data):
    if data[:4] != b'DCX\0':
        return data
    i = data.find(b'DCA\0')
    usize = struct.unpack_from('>I', data, data.find(b'DCS\0') + 4)[0]
    out = zlib.decompressobj().decompress(data[i + struct.unpack_from('>I', data, i + 4)[0]:])
    assert len(out) == usize
    return out


def bnd4_entries(data):
    """BND4 / BHF4 file table: (name, offset, size)."""
    assert data[:4] in (b'BND4', b'BHF4'), data[:4]
    E = '>' if data[9] == 1 else '<'
    count = struct.unpack_from(E + 'i', data, 0x0C)[0]
    hsize = struct.unpack_from(E + 'q', data, 0x20)[0]
    unicode, fmt = data[0x30], data[0x31]
    if not (data[0x0A] == 0 or ((fmt & 1) and not (fmt & 0x80))):
        fmt = int('{:08b}'.format(fmt)[::-1], 2)
    pos = 0x40
    for _ in range(count):
        p = pos + 8
        size = struct.unpack_from(E + 'q', data, p)[0]
        p += 8 + (8 if fmt & 0x20 else 0)
        if fmt & 0x10:
            off = struct.unpack_from(E + 'q', data, p)[0]
            p += 8
        else:
            off = struct.unpack_from(E + 'I', data, p)[0]
            p += 4
        p += 4 if fmt & 0x02 else 0
        noff = struct.unpack_from(E + 'I', data, p)[0]
        if unicode:
            end = noff
            while data[end:end + 2] != b'\0\0':
                end += 2
            name = data[noff:end].decode('utf-16-be' if E == '>' else 'utf-16-le')
        else:
            name = data[noff:data.index(b'\0', noff)].decode('shift-jis', 'replace')
        pos += hsize
        yield name.replace('\\', '/').lower(), off, size


class Archives:
    """Every BHF4/BDT pair of disc 1 plus disc 2's DLC pair; later sources win (the DLC's newer copies)."""

    def __init__(self, game_root, disc2_root):
        self.index = {}
        for name in sorted(os.listdir(game_root)):
            if name.lower().endswith('.bhd'):
                bdt = open(os.path.join(game_root, name[:-4] + '.bdt'), 'rb')
                for n, off, size in bnd4_entries(open(os.path.join(game_root, name), 'rb').read()):
                    self.index[n] = (bdt, off, size)
        pkg = find_package(disc2_root) if disc2_root else None
        if pkg:
            s = Stfs(pkg)
            files = {e['name'].lower(): e for e in s.entries() if not e['dir']}
            for bhd in [k for k in files if k.endswith('.bhd')]:
                bdt = s.reader(files[bhd[:-4] + '.bdt'])
                for n, off, size in bnd4_entries(s.reader(files[bhd]).read_at(0, files[bhd]['size'])):
                    self.index[n] = (bdt, off, size)
            print(f'disc 2: {os.path.basename(pkg)} ({len(files)} files)')
        else:
            print('disc 2: no package found (DLC icons missing)')

    def get(self, name):
        src, off, size = self.index[name.lower()]
        if isinstance(src, StfsFile):
            return undcx(src.read_at(off, size))
        src.seek(off)
        return undcx(src.read(size))

    def names(self, pattern):
        rx = re.compile(pattern, re.I)
        return sorted(n for n in self.index if rx.search(n))


def find_package(disc2_root):
    for d, _, files in os.walk(os.path.join(disc2_root, 'content')):
        for f in files:
            with open(os.path.join(d, f), 'rb') as fh:
                if fh.read(4) in (b'PIRS', b'LIVE', b'CON '):
                    return os.path.join(d, f)
    return None


def bnd_files(data):
    return {n.split('/')[-1]: undcx(data[off:off + size]) for n, off, size in bnd4_entries(data)}


# ------------------------------------------------------------------------------------------------ textures
def tiled_offsets(bw, bh, bpb):
    """Xenos XGAddress2DTiledOffset for every (x, y) block, in blocks."""
    aw = (bw + 31) & ~31
    log_bpp = (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2))
    y, x = np.mgrid[0:bh, 0:bw].astype(np.int64)
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4)
    off = (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7)
           + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp
    return off


def untile(data, bw, bh, bpb):
    raw = np.frombuffer(data, dtype=np.uint8)
    offs = tiled_offsets(bw, bh, bpb)
    need = (int(offs.max()) + 1) * bpb
    if len(raw) < need:
        raw = np.concatenate([raw, np.zeros(need - len(raw), np.uint8)])
    return raw.reshape(-1, bpb)[offs.reshape(-1)].reshape(bh, bw, bpb)


def rgb565(c):
    r = ((c >> 11) & 31).astype(np.uint32) * 255 // 31
    g = ((c >> 5) & 63).astype(np.uint32) * 255 // 63
    b = (c & 31).astype(np.uint32) * 255 // 31
    return np.stack([r, g, b], -1)


def dxt_color(blk, dxt1):
    c0 = blk[..., 0].astype(np.uint16) | (blk[..., 1].astype(np.uint16) << 8)
    c1 = blk[..., 2].astype(np.uint16) | (blk[..., 3].astype(np.uint16) << 8)
    bits = (blk[..., 4].astype(np.uint32) | (blk[..., 5].astype(np.uint32) << 8)
            | (blk[..., 6].astype(np.uint32) << 16) | (blk[..., 7].astype(np.uint32) << 24))
    p0, p1 = rgb565(c0), rgb565(c1)
    four = np.broadcast_to((c0 > c1) | (not dxt1), c0.shape)[..., None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], -2)
    alpha = np.full(pal.shape[:-1] + (1,), 255, np.uint32)
    if dxt1:
        alpha[..., 3, 0] = np.where(four[..., 0], 255, 0)
    pal = np.concatenate([pal, alpha], -1)
    idx = (bits[..., None] >> (2 * np.arange(16))) & 3
    return np.take_along_axis(pal, idx[..., None].astype(np.int64).repeat(4, -1), -2)


def dxt5_alpha(blk):
    a0, a1 = blk[..., 0].astype(np.int32), blk[..., 1].astype(np.int32)
    bits = np.zeros(blk.shape[:-1], np.uint64)
    for i in range(6):
        bits |= blk[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = ((bits[..., None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
    pal = [a0, a1] + [None] * 6
    six = [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [np.zeros_like(a0), np.full_like(a0, 255)]
    for i in range(6):
        pal[2 + i] = np.where(a0 > a1, ((6 - i) * a0 + (1 + i) * a1) // 7, six[i])
    return np.take_along_axis(np.stack(pal, -1), idx, -1)


TPF_FORMATS = {0: 'dxt1', 1: 'dxt1', 3: 'dxt3', 5: 'dxt5', 9: 'argb8'}


def tpf_textures(data):
    """{name: RGBA numpy image} of a TPF."""
    assert data[:4] == b'TPF\0'
    platform = data[0x0C]
    E = '<' if platform == 0 else '>'
    count = struct.unpack_from(E + 'i', data, 8)[0]
    enc, p, out = data[0x0E], 0x10, {}
    for _ in range(count):
        off, size = struct.unpack_from(E + 'Ii', data, p)
        fmt = data[p + 8]
        p += 12
        w, h = struct.unpack_from(E + 'hh', data, p)
        p += 4 + (4 if platform == 1 else 0)
        noff, has_float = struct.unpack_from(E + 'Ii', data, p)
        p += 8
        if has_float:
            p += 8 + struct.unpack_from(E + 'i', data, p + 4)[0]
        if enc == 1:
            end = noff
            while data[end:end + 2] != b'\0\0':
                end += 2
            name = data[noff:end].decode('utf-16-be' if E == '>' else 'utf-16-le')
        else:
            name = data[noff:data.index(b'\0', noff)].decode('shift-jis', 'replace')
        kind = TPF_FORMATS.get(fmt)
        if kind is None:
            print(f'  {name}: TPF format {fmt} not decoded')
            continue
        if kind == 'argb8':  # 32-bit, tiled per pixel, A R G B in byte order (the title screen's logo)
            out[name] = untile(data[off:off + w * h * 4], w, h, 4)[..., [1, 2, 3, 0]].copy()
            continue
        bpb = 8 if kind == 'dxt1' else 16
        bw, bh = (w + 3) // 4, (h + 3) // 4
        blocks = untile(data[off:off + size], bw, bh, bpb)
        blocks = blocks.reshape(bh, bw, bpb // 2, 2)[..., ::-1].reshape(bh, bw, bpb)
        if kind == 'dxt1':
            px = dxt_color(blocks, True)
        else:
            px = dxt_color(blocks[..., 8:], False)
            if kind == 'dxt5':
                px[..., 3] = dxt5_alpha(blocks[..., :8])
            else:
                a = blocks[..., :8]
                px[..., 3] = np.stack([a & 15, a >> 4], -1).reshape(bh, bw, 16).astype(np.uint32) * 17
        img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
        out[name] = img[:h, :w].astype(np.uint8)
    return out


# ------------------------------------------------------------------------------------------------ what we cut
# Pieces in source texture pixels (x0, y0, x1, y1).
PIECES = {
    # FE_HPgage_01 (256x128): the emblem, the connector + two bar frames (stretched in the middle), the end caps the
    # game puts at the end of each bar, and the fills.
    'emblem': ('FE_HPgage_01', (0, 0, 92, 88)),
    'bars': ('FE_HPgage_01', (84, 20, 253, 62)),
    'cap_hp': ('FE_HPgage_01', (180, 88, 214, 109)),
    'cap_st': ('FE_HPgage_01', (141, 92, 175, 106)),
    'fill_hp': ('FE_HPgage_01', (35, 95, 96, 105)),
    'fill_st': ('FE_HPgage_01', (35, 112, 96, 118)),
    'fill_flash': ('FE_HPgage_01', (100, 95, 112, 105)),
    'human': ('FE_HPgage_01', (190, 59, 218, 88)),
    'line_red': ('FE_HPgage_01', (158, 3, 249, 7)),
    'line_yellow': ('FE_HPgage_01', (158, 9, 249, 13)),
    'bar_dark': ('FE_HPgage_01', (156, 15, 251, 23)),
    # Soul_Gauge (64x64): the counter's left ornament, its line and the S icon.
    'soul_end': ('Soul_Gauge', (3, 1, 30, 36)),
    'soul_line': ('Soul_Gauge', (35, 4, 62, 7)),
    'soul_icon': ('Soul_Gauge', (0, 38, 26, 64)),
    # Icon_tex (256x256): status build-up icons, row 8 of the 27 px grid: poison (green drop) and curse are the
    # game's own build-up icons (Thor, DS2_STATUS_TEST); the red splat and the blue-grey one are not used by the bars.
    'tol_poison': ('Icon_tex', (162, 216, 188, 242)),
    'tol_curse': ('Icon_tex', (189, 216, 215, 242)),
    # The equipment slots (menu/01.febnd L_keyMenu, 256x128): the card's ornate frame, its dark back, the glowing
    # card, the durability lines under a card; the bronze plate an item stands on (IconBase).
    'card_frame': ('L_keyMenu', (1, 0, 80, 104)),
    'card_back': ('L_keyMenu', (81, 4, 156, 98)),
    'card_glow': ('L_keyMenu', (159, 4, 254, 124)),
    'card_line_red': ('L_keyMenu', (11, 111, 70, 113)),
    'card_line_grey': ('L_keyMenu', (11, 117, 70, 119)),
    'plate': ('IconBase', (81, 224, 150, 255)),
    # Backgrounds: the leather sheet behind the item menus, the in-game menu's framed panel, its ornament line.
    'leather': ('ItemSellect_02', (0, 0, 512, 372)),
    'panel': ('In-game_01', (16, 4, 892, 544)),
    'ornament': ('In-game_01', (0, 586, 812, 616)),
}
# Neighbouring art that falls inside a piece's rectangle, cleared (piece pixels): the dark bar above the bar frames and
# the human-form icon below them.
CLEAR = {'bars': [(66, 0, 169, 5), (66, 38, 169, 42)]}
# Build-up icons the game composes (a hexagon with an icon inside) that no atlas holds whole: toxic (the poison bulb of
# waku_03's attack icons in the game's red) and petrification (Icon_tex's white crystal), each in the poison icon's gold
# rim over waku_02's dark hexagon. (rim piece, inner rect in the rim's 26 px, icon source + rect, tint or None)
COMPOSED = {
    'tol_toxic': (('waku_03', (963, 10, 981, 27)), (205, 105, 125)),
    'tol_petrify': (('Icon_tex', (135, 162, 161, 188)), None),
}
TEXTURE_SOURCES = {
    'FE_HPgage_01': ('menu/01.febnd.dcx', 'FE_HPgage_01.tpf'),
    'Soul_Gauge': ('menu/01.febnd.dcx', 'Soul_Gauge.tpf'),
    'Icon_tex': ('menu/99.febnd.dcx', 'Icon_tex.tpf'),
    'ItemSellect_02': ('menu/99.febnd.dcx', 'ItemSellect_02.tpf'),
    'In-game_01': ('menu/40.febnd.dcx', 'In-game_01.tpf'),
    'waku_02': ('menu/99.febnd.dcx', 'waku_02.tpf'),
    'L_keyMenu': ('menu/01.febnd.dcx', 'L_keyMenu.tpf'),
    'IconBase': ('menu/99.febnd.dcx', 'IconBase.tpf'),
    'waku_03': ('menu/99.febnd.dcx', 'waku_03.tpf'),
}
# Pieces drawn large: their upscale is brought down to this many pixels at most on the long side (APK size, memory).
MAX_SIDE = {'leather': 1600, 'panel': 1600, 'ornament': 1600}
FONT_CODES = [c for c in range(0x20, 0x7F)] + [0xD7, 0xB7, 0x2026]


# The cover the second screen shows without a character (title screen, main menu, loading between areas): the title
# screen's logo, "DARK", "SOULS" and "II" from Title (menu/17, 512x512 A8R8G8B8, opaque on black: the game adds it)
# and "SCHOLAR OF THE FIRST SIN" from Title_Best_DC, in one line like the game's logo. Kept at the game's size (the
# second screen draws it at ~0.95x).
TITLE_LOGO_SIZE = (1249, 210)
TITLE_LOGO = [  # texture, source rectangle, position in the logo
    ('title.tpf', (0, 8, 512, 148), (12, 0)),             # DARK (its letters x 88-496)
    ('title.tpf', (0, 150, 512, 290), (519, 0)),          # SOULS (35-492)
    ('title.tpf', (0, 296, 150, 436), (1044, 0)),         # II (13-105)
    ('title_best_dc.tpf', (0, 10, 640, 52), (308, 150)),  # SCHOLAR OF THE FIRST SIN (5-628)
]


def compose_title(arc):
    files = bnd_files(arc.get('menu/17.febnd.dcx'))
    canvas = np.zeros((TITLE_LOGO_SIZE[1], TITLE_LOGO_SIZE[0], 3), np.float32)
    for tpf, (x0, y0, x1, y1), (px, py) in TITLE_LOGO:
        img = next(iter(tpf_textures(files[tpf]).values()))[y0:y1, x0:x1, :3].astype(np.float32)
        h, w = img.shape[0], min(img.shape[1], canvas.shape[1] - px)
        canvas[py:py + h, px:px + w] += img[:, :w]  # additive, as the title screen draws it
    canvas = canvas.clip(0, 255)
    alpha = canvas.max(2)  # black -> transparent, so it sits on any background
    rgb = canvas * 255 / np.maximum(alpha, 1)[..., None]
    return np.dstack([rgb, alpha]).clip(0, 255).astype(np.uint8)


def cut_pieces(arc, raw_dir):
    textures, bundles = {}, {}
    for tex, (bnd, name) in TEXTURE_SOURCES.items():
        if bnd not in bundles:
            bundles[bnd] = bnd_files(arc.get(bnd))
        textures[tex] = tpf_textures(bundles[bnd][name.lower()] if name.lower() in bundles[bnd] else bundles[bnd][name])
        textures[tex] = next(iter(textures[tex].values()))
    sizes = {}
    os.makedirs(os.path.join(raw_dir, 'hud'), exist_ok=True)
    for piece, img in compose_icons(textures).items():
        Image.fromarray(img).save(os.path.join(raw_dir, 'hud', piece + '.png'))
        sizes[piece] = [img.shape[1], img.shape[0]]
    for piece, (tex, (x0, y0, x1, y1)) in PIECES.items():
        img = textures[tex][y0:y1, x0:x1].copy()
        for cx0, cy0, cx1, cy1 in CLEAR.get(piece, []):
            img[cy0:cy1, cx0:cx1] = 0
        Image.fromarray(img).save(os.path.join(raw_dir, 'hud', piece + '.png'))
        sizes[piece] = [x1 - x0, y1 - y0]
    return sizes


def compose_icons(textures):
    """COMPOSED icons at 26x26 game pixels."""
    rim = textures['Icon_tex'][216:242, 162:188].astype(np.float32)  # the poison icon: gold rim, olive inside, drop
    hexa = Image.fromarray(textures['waku_02'][91:122, 788:819]).resize((20, 20), Image.LANCZOS)
    out = {}
    for name, ((tex, (x0, y0, x1, y1)), tint) in COMPOSED.items():
        base = Image.fromarray(rim.astype(np.uint8))
        base.alpha_composite(hexa, (3, 3))  # covers the drop and the olive inside the rim
        icon = textures[tex][y0:y1, x0:x1].astype(np.float32)
        if tint is None:  # the crystal: keep its bright pixels (the cell's dark square drops out)
            lum = icon[..., :3].mean(-1)
            inner = icon[5:21, 5:21]  # inside the cell's frame
            icon = inner.copy()
            icon[..., 3] = np.clip((inner[..., :3].mean(-1) - 38) * 2.6, 0, 255) * (inner[..., 3] / 255)
            size = 19
        else:  # a recoloured icon: its brightness times the tint
            lum = icon[..., :3].mean(-1, keepdims=True) / 255
            icon[..., :3] = np.clip(lum * 1.35 * np.array(tint, np.float32), 0, 255)
            size = 17
        im = Image.fromarray(icon.astype(np.uint8))
        im.thumbnail((size, size), Image.LANCZOS)
        base.alpha_composite(im, ((26 - im.size[0]) // 2, (26 - im.size[1]) // 2))
        out[name] = np.array(base)
    return out


def fnv64(data):
    """FNV-1a 64 of a texture's data as the game holds it (ds2_hud_sprites.cpp hashes the same 16 KB)."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def tpf_raw(data):
    """(width, height, raw texture bytes) of a TPF's first texture."""
    off, size = struct.unpack_from('>Ii', data, 0x10)
    w, h = struct.unpack_from('>hh', data, 0x1C)
    return w, h, data[off:off + size]


def cut_effects(arc, raw_dir):
    """Effect icons as PNGs, and {hash of their data: id} to recognise them among what the game draws."""
    os.makedirs(os.path.join(raw_dir, 'effect'), exist_ok=True)
    ids, hashes = [], {}
    for name in arc.names(r'^menu/tex/icon/effect/ei_\d+\.tpf$'):
        data = arc.get(name)
        img = next(iter(tpf_textures(data).values()))
        icon = int(re.search(r'ei_(\d+)', name).group(1))
        Image.fromarray(img).save(os.path.join(raw_dir, 'effect', '%04d.png' % icon))
        ids.append(icon)
        hashes['%x' % fnv64(tpf_raw(data)[2][:0x4000])] = icon
    return ids, hashes


ICON_SIZE = (192, 384)  # the equipment card draws a 64x128 icon at ~188x376 on the Thor's 1240x1080 screen


def cut_icons(arc, raw_dir):
    """Every 64x128 item icon (disc 1 and the DLC) as raw/icon/<hash>.png, the hash being what the running game's HUD
    texture hashes to (ds2_hud_sprites.cpp: FNV-1a 64 of the first 16 KB, still tiled), so the second screen finds the
    upscaled icon for whatever the HUD draws. Returns the count."""
    d = os.path.join(raw_dir, 'icon')
    os.makedirs(d, exist_ok=True)
    seen = set()
    for name in arc.names(r'^menu/tex/icon/ic_\d+\.tpf$'):
        data = arc.get(name)
        w, h, raw = tpf_raw(data)
        if (w, h) != (64, 128) or len(raw) < 0x4000:
            continue
        key = '%016x' % fnv64(raw[:0x4000])
        if key in seen:
            continue
        seen.add(key)
        Image.fromarray(next(iter(tpf_textures(data).values()))).save(os.path.join(d, key + '.png'))
    return len(seen)


def build_icons(game_root, disc2, out):
    """Development builds' icon pack (the owner, 2026-11-22: icons rough): Real-ESRGAN 4x of every item icon, brought to
    the card's size as WebP in companion/icon/. Slow (a few minutes on the GPU); public APKs ship none and enlarge the
    icon the game draws on the device (CompanionIcons, Lanczos)."""
    raw, final = os.path.join(out, 'raw_icons'), os.path.join(out, 'companion', 'icon')
    shutil.rmtree(os.path.join(raw, 'icon'), ignore_errors=True)
    shutil.rmtree(final, ignore_errors=True)
    arc = Archives(game_root, disc2 if os.path.isdir(disc2) else None)
    n = cut_icons(arc, raw)
    print(f'{n} item icons')
    up = os.path.join(raw, 'up')
    names = sorted(os.listdir(os.path.join(raw, 'icon')))
    if not os.path.isdir(up) or sorted(os.listdir(up)) != names:  # an earlier run's upscales are reused
        shutil.rmtree(up, ignore_errors=True)
        upscale_assets.run(os.path.join(raw, 'icon'), up)
    os.makedirs(final, exist_ok=True)
    for f in names:  # method 4: ~25 ms and ~15 KB an icon (method 6 took over a second each)
        im = Image.open(os.path.join(up, f)).convert('RGBA').resize(ICON_SIZE, Image.LANCZOS)
        im.save(os.path.join(final, f[:-4] + '.webp'), 'WEBP', quality=90, method=4)
    total = sum(os.path.getsize(os.path.join(final, f)) for f in os.listdir(final))
    print(f'wrote {len(os.listdir(final))} icons to {final} ({total / 1e6:.1f} MB)')


def cut_font(game_root, raw_dir):
    files = bnd_files(undcx(open(os.path.join(game_root, 'font', 'English', 'FeFont_Small.fontbnd.dcx'), 'rb').read()))
    ccm = files['fefont_small.ccm'] if 'fefont_small.ccm' in files else files['FeFont_Small.ccm']
    regoff, glyoff = struct.unpack_from('>II', ccm, 0x14)
    glyphs, p = {}, glyoff
    while p + 0x18 <= len(ccm):
        code, ro, page, pre, width, adv = struct.unpack_from('>IIHhhh', ccm, p)
        if ro < regoff or ro >= glyoff:
            break
        glyphs[code] = [page] + list(struct.unpack_from('>HHHH', ccm, ro)) + [pre, width, adv]
        p += 0x18
    used = {c: glyphs[c] for c in FONT_CODES if c in glyphs}
    pages = sorted({g[0] for g in used.values()})
    os.makedirs(os.path.join(raw_dir, 'font'), exist_ok=True)
    for page in pages:
        key = [k for k in files if k.lower().startswith('fefont_small_%04d' % page)][0]
        img = next(iter(tpf_textures(files[key]).values()))
        Image.fromarray(img).save(os.path.join(raw_dir, 'font', 'page%d.png' % page))
    return {'line_height': struct.unpack_from('>H', ccm, 8)[0], 'pages': pages,
            'glyphs': {str(c): g for c, g in used.items()}}


def main():
    args = [a for a in sys.argv[1:] if a != '--icons']
    game_root = args[0] if len(args) > 0 else 'assets/game_root'
    disc2 = args[1] if len(args) > 1 else 'assets/disc2'
    out = args[2] if len(args) > 2 else 'out/companion_assets'
    if '--icons' in sys.argv:  # only the icon pack (companion/icon), the rest stays
        build_icons(game_root, disc2, out)
        return
    raw, final = os.path.join(out, 'raw'), os.path.join(out, 'companion')
    shutil.rmtree(raw, ignore_errors=True)
    for part in ('hud', 'effect', 'font', 'hud.json', 'font.json'):  # keeps an icon pack built with --icons
        q = os.path.join(final, part)
        shutil.rmtree(q, ignore_errors=True) if os.path.isdir(q) else (os.remove(q) if os.path.exists(q) else None)
    arc = Archives(game_root, disc2 if os.path.isdir(disc2) else None)
    sizes = cut_pieces(arc, raw)
    effects, effect_hashes = cut_effects(arc, raw)
    font = cut_font(game_root, raw)
    print(f'{len(sizes)} HUD pieces, {len(effects)} effect icons, font pages {font["pages"]}, {len(font["glyphs"])} glyphs')
    for part in ('hud', 'effect', 'font'):
        upscale_assets.run(os.path.join(raw, part), os.path.join(final, part))
    for piece, side in MAX_SIDE.items():
        p = os.path.join(final, 'hud', piece + '.png')
        im = Image.open(p)
        k = side / max(im.size)
        if k < 1:
            im.resize((round(im.size[0] * k), round(im.size[1] * k)), Image.LANCZOS).save(p)
    logo = compose_title(arc)
    Image.fromarray(logo, 'RGBA').save(os.path.join(final, 'hud', 'title_logo.png'))
    sizes['title_logo'] = [logo.shape[1], logo.shape[0]]
    with open(os.path.join(final, 'hud.json'), 'w', newline='') as f:
        json.dump({'version': 2, 'pieces': sizes, 'effects': effects, 'effect_hashes': effect_hashes}, f, indent=1)
    with open(os.path.join(final, 'font.json'), 'w', newline='') as f:
        json.dump(font, f)
    print('wrote', final)


if __name__ == '__main__':
    main()
