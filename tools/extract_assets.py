#!/usr/bin/env python3
"""
Moorhuhn Adventure 2: Der Fluch des Goldes — mha2.dat Asset Extractor & HD Pack Helper.

Reverse-engineered virtual filesystem (VDrv) and image format (JPG-in-_img & IT3 RLE sprites).
Extracts room backgrounds, sprites (with full alpha transparency), audio, and scripts from
a user-supplied game_files/mha2.dat.

Usage:
  python3 tools/extract_assets.py --list
  python3 tools/extract_assets.py --backgrounds -o extracted_assets
  python3 tools/extract_assets.py --all -o extracted_assets
  python3 tools/extract_assets.py --filter x/001/ -o extracted_assets
"""
import argparse
import fnmatch
import os
import struct
import subprocess
import sys
import zlib


def descramble(data: bytes) -> bytes:
    """Exact port of FUN_004d9a00 (VDrv directory/entry header descrambler)."""
    b = bytearray(data)
    n = len(b)
    if n <= 0:
        return bytes(b)
    b[0] = (~b[0]) & 0xFF
    u10 = n - 1
    if u10 == 0:
        return bytes(b)

    # Pass 1
    u1 = 0
    u8 = (~u10) & 0xFFFFFFFF
    for i in range(u10):
        b[1 + i] ^= (u1 & 0xFF)
        rot = ((u8 & 1) << 1) | 5
        u8 >>= 1
        if u8 == 0:
            u8 = (~u10) & 0xFFFFFFFF
        u1 = ((((u1 >> rot) | (u1 << (32 - rot))) & 0xFFFFFFFF) + 1) & 0xFFFFFFFF
        if u1 == 0:
            u1 = 0x5A3C96E7

    # Pass 2
    half = u10 >> 1
    for i in range(half):
        x = b[1 + 2 * i]
        b[1 + 2 * i] = b[1 + 2 * i + 1] ^ 0xAA
        b[1 + 2 * i + 1] = x ^ 0x55

    # Pass 3
    u1 = 0
    u8 = u10
    for i in range(u10):
        b[1 + i] ^= (u1 & 0xFF)
        rot = (6 if (u8 & 1) else 0) + 11
        u8 >>= 1
        if u8 == 0:
            u8 = u10
        u1 = ((((u1 << rot) | (u1 >> (32 - rot))) & 0xFFFFFFFF) + 1) & 0xFFFFFFFF
        if u1 == 0:
            u1 = 0x5A3C96E7

    # Pass 4
    for i in range(half):
        x = b[1 + i]
        y = b[u10 - i]
        b[1 + i] = y ^ 0x0F
        b[u10 - i] = x ^ 0xF0

    return bytes(b)


def write_png_rgba(path: str, width: int, height: int, rgba: bytes):
    """Pure-stdlib RGBA PNG writer (zero external dependencies)."""
    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    raw_rows = bytearray()
    stride = width * 4
    for y in range(height):
        raw_rows.append(0)  # Filter type 0 (None)
        raw_rows.extend(rgba[y * stride : (y + 1) * stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw_rows), 6))
        + chunk(b"IEND", b"")
    )
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as out:
        out.write(png)


class Mha2Archive:
    def __init__(self, dat_path: str):
        self.dat_path = dat_path
        self.f = open(dat_path, "rb")
        hdr = self.f.read(0x4C)
        if hdr[:4] != b"MHA2":
            raise RuntimeError(f"Not a valid MHA2 archive: {dat_path}")
        self.ver, self.fsize, self.idx_off = struct.unpack_from("<III", hdr, 0x40)
        self.entries = []
        self.f.seek(self.idx_off)
        self._read_tree("")

    def _read_tree(self, prefix: str):
        while self.f.tell() < self.fsize:
            h0 = self.f.read(16)
            if len(h0) < 16:
                break
            typ = struct.unpack_from("<I", h0, 0)[0]
            h1 = descramble(self.f.read(16))
            _, has_next, _, has_child = struct.unpack("<IIII", h1)
            if typ == 4:
                name = descramble(self.f.read(0x20)).split(b"\x00", 1)[0].decode("latin1")
                path = f"{prefix}/{name}" if prefix else name
                if has_child:
                    self._read_tree(path)
            elif typ == 3:
                payload = descramble(self.f.read(0x58))
                name = payload[:0x40].split(b"\x00", 1)[0].decode("latin1")
                flags, ft_lo, ft_hi, csz, usz, off = struct.unpack_from("<IIIIII", payload, 0x40)
                path = f"{prefix}/{name}" if prefix else name
                self.entries.append({
                    "path": path,
                    "flags": flags,
                    "csize": csz,
                    "usize": usz,
                    "offset": off,
                })
            else:
                break
            if has_next == 0:
                break

    def read_entry(self, entry) -> bytes:
        self.f.seek(entry["offset"])
        raw = self.f.read(entry["csize"])
        if entry["flags"] & 1:
            return zlib.decompress(raw)
        return raw

    def close(self):
        self.f.close()


def decode_it3_frame(raw: bytes, frame_off: int, palette, canvas_w: int = 0, canvas_h: int = 0):
    """Decode one IT3 RLE frame into (width, height, rgba_bytes).

    If canvas_w/canvas_h (from the IT3 t0 DirectDraw cell table) are provided and larger
    than the tight RLE bounding box (w, h), the frame is placed at top-left (0, 0) on a
    transparent (canvas_w x canvas_h) RGBA canvas, matching mha2.exe FUN_004de0d0 exactly.
    """
    w, h, colorkey, _px_cnt, _ctrl_cnt, bpp, has_alpha, _stride, _pad, _pal_cnt, _pal_off, data_off = (
        struct.unpack_from("<12I", raw, frame_off)
    )
    if w == 0 or h == 0 or data_off >= len(raw):
        return None
    out_w = canvas_w if canvas_w >= w else w
    out_h = canvas_h if canvas_h >= h else h
    out = bytearray(out_w * out_h * 4)
    pos = data_off
    x = 0
    y = 0
    n_raw = len(raw)
    while pos < n_raw and y < h:
        b = raw[pos]
        pos += 1
        if b == 0:
            break
        if (b & 0x80) == 0:
            x += b
        else:
            cnt = b & 0x7F
            if cnt == 0:
                y += 1
                x = 0
                continue
            for _ in range(cnt):
                if bpp == 1:
                    if pos >= n_raw:
                        break
                    idx = raw[pos]
                    pos += 1
                    rgb = palette[idx] if idx < len(palette) else 0
                    r = (rgb >> 16) & 0xFF
                    g = (rgb >> 8) & 0xFF
                    bl = rgb & 0xFF
                    if has_alpha:
                        a = raw[pos] if pos < n_raw else 255
                        pos += 1
                    else:
                        a = 255
                elif bpp == 2:
                    if pos + 2 > n_raw:
                        break
                    p16 = struct.unpack_from("<H", raw, pos)[0]
                    pos += 2
                    r = ((p16 >> 11) & 0x1F) * 255 // 31
                    g = ((p16 >> 5) & 0x3F) * 255 // 63
                    bl = (p16 & 0x1F) * 255 // 31
                    if has_alpha:
                        a = raw[pos] if pos < n_raw else 255
                        pos += 1
                    else:
                        a = 255
                elif bpp == 3:
                    if pos + 3 > n_raw:
                        break
                    bl, g, r = raw[pos], raw[pos + 1], raw[pos + 2]
                    pos += 3
                    if has_alpha:
                        a = raw[pos] if pos < n_raw else 255
                        pos += 1
                    else:
                        a = 255
                elif bpp == 4:
                    if pos + 4 > n_raw:
                        break
                    bl, g, r, a0 = raw[pos], raw[pos + 1], raw[pos + 2], raw[pos + 3]
                    pos += 4
                    if has_alpha:
                        a = raw[pos] if pos < n_raw else a0
                        pos += 1
                    else:
                        a = 255
                else:
                    return None
                if 0 <= x < out_w and 0 <= y < out_h:
                    di = (y * out_w + x) * 4
                    out[di] = r
                    out[di + 1] = g
                    out[di + 2] = bl
                    out[di + 3] = a
                x += 1
    return (out_w, out_h, bytes(out))


def extract_img_entry(raw: bytes, out_base: str, convert_jpg_to_png: bool = True, upscale: int = 1) -> int:
    """Extract a ._img container (JPEG background or IT3 multi-frame sprite). Returns count of files written."""
    if len(raw) < 0x50:
        return 0
    img_type = struct.unpack_from("<I", raw, 0)[0]
    written = 0
    if img_type == 0:
        # JPEG-backed image (room backgrounds, portraits, full-screen art)
        joff, jsz = struct.unpack_from("<II", raw, 0x40)
        if joff + jsz <= len(raw) and jsz > 4:
            jpg = raw[joff : joff + jsz]
            if convert_jpg_to_png or upscale > 1:
                png_path = out_base + ".png"
                os.makedirs(os.path.dirname(png_path), exist_ok=True)
                cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "mjpeg", "-i", "pipe:0"]
                if upscale > 1:
                    cmd += ["-vf", f"scale=iw*{upscale}:ih*{upscale}:flags=lanczos,unsharp=5:5:0.75:5:5:0.0"]
                cmd.append(png_path)
                p = subprocess.run(
                    cmd,
                    input=jpg,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                )
                if p.returncode == 0:
                    written += 1
                else:
                    jpg_path = out_base + ".jpg"
                    with open(jpg_path, "wb") as out:
                        out.write(jpg)
                    written += 1
            else:
                jpg_path = out_base + ".jpg"
                os.makedirs(os.path.dirname(jpg_path), exist_ok=True)
                with open(jpg_path, "wb") as out:
                    out.write(jpg)
                written += 1
        return written

    if img_type == 1:
        # IT3 RLE sprite (1 or more frames)
        pal_off, pal_cnt = struct.unpack_from("<II", raw, 0x28)
        palette = []
        if pal_off and pal_cnt and pal_off + pal_cnt * 4 <= len(raw):
            palette = list(struct.unpack_from(f"<{pal_cnt}I", raw, pal_off))
        t0_off, t0_cnt, tbl1_off, tbl1_cnt = struct.unpack_from("<4I", raw, 0x38)
        if tbl1_off and tbl1_cnt:
            for fi in range(tbl1_cnt):
                foff = struct.unpack_from("<I", raw, tbl1_off + fi * 4)[0]
                if not foff:
                    continue
                cw, ch = 0, 0
                if t0_off and fi < t0_cnt and t0_off + (fi + 1) * 17 <= len(raw):
                    _, _, cw, ch = struct.unpack_from("<4I", raw, t0_off + fi * 17)
                dec = decode_it3_frame(raw, foff, palette, cw, ch)
                if dec:
                    w, h, rgba = dec
                    suffix = "" if tbl1_cnt == 1 else f"_f{fi:03d}"
                    write_png_rgba(out_base + suffix + ".png", w, h, rgba)
                    written += 1
    return written


def main():
    ap = argparse.ArgumentParser(description="Extract assets from Moorhuhn Adventure 2 mha2.dat")
    ap.add_argument("--dat", default="game_files/mha2.dat", help="Path to mha2.dat")
    ap.add_argument("-o", "--out", default="extracted_assets", help="Output directory")
    ap.add_argument("--list", action="store_true", help="List archive entries and exit")
    ap.add_argument("--backgrounds", action="store_true", help="Extract all JPEG backgrounds (objBg + full images)")
    ap.add_argument("--images", action="store_true", help="Extract all ._img assets as PNGs")
    ap.add_argument("--all", action="store_true", help="Extract all files (images as PNG + raw audio/data)")
    ap.add_argument("--filter", default="*", help="Glob filter on VDrv path (e.g. 'x/001/*' or '*objBg*')")
    ap.add_argument("--keep-jpg", action="store_true", help="Write raw .jpg instead of converting JPEG backgrounds to .png")
    ap.add_argument("--upscale", type=int, default=1, choices=[1, 2, 3, 4],
                    help="Upscale extracted backgrounds by Nx (e.g. --upscale 2 for 1600x1200 starter hd_assets/)")
    args = ap.parse_args()

    arc = Mha2Archive(args.dat)
    try:
        matched = [e for e in arc.entries if fnmatch.fnmatch(e["path"], args.filter)]
        if args.list:
            for e in matched:
                comp = "ZLIB" if (e["flags"] & 1) else "RAW "
                print(f"{comp} {e['usize']:>9d}  {e['path']}")
            print(f"\nTotal: {len(matched)} / {len(arc.entries)} files")
            return

        if not (args.backgrounds or args.images or args.all):
            args.backgrounds = True

        n_files = 0
        for e in matched:
            path = e["path"]
            if path.endswith("._img"):
                raw = arc.read_entry(e)
                img_type = struct.unpack_from("<I", raw, 0)[0] if len(raw) >= 4 else -1
                if args.backgrounds and not (args.images or args.all):
                    if img_type != 0:
                        continue
                rel_no_ext = path[:-5]  # strip "._img"
                out_base = os.path.join(args.out, rel_no_ext)
                n_files += extract_img_entry(raw, out_base, convert_jpg_to_png=(not args.keep_jpg), upscale=args.upscale)
            elif args.all:
                raw = arc.read_entry(e)
                dst = os.path.join(args.out, path)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as out:
                    out.write(raw)
                n_files += 1
        print(f"[DONE] Extracted {n_files} files into '{args.out}'.")
    finally:
        arc.close()


if __name__ == "__main__":
    main()
