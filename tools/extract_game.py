#!/usr/bin/env python3
"""
Standalone legal-copy extractor for Moorhuhn Adventure 2: Der Fluch des Goldes.

Extracts game files directly from either:
  1) An original Wise installer executable (Setup.EXE), OR
  2) A raw CD-ROM .bin (Mode 1/2352) or standard .iso image containing Setup.EXE.

Zero copyrighted data is included in this repository — users supply their own
original disc/installer.
"""
import argparse
import os
import re
import struct
import sys
import zlib


def extract_setup_from_iso_or_bin(image_path, out_setup_path):
    """Locate and extract Der_Fluch_des_Goldes/Setup/Setup.EXE from an ISO9660 .iso or Mode 1/2352 .bin."""
    size = os.path.getsize(image_path)
    with open(image_path, "rb") as f:
        # Detect Mode 1/2352 vs standard 2048-byte ISO
        f.seek(16 * 2352 + 16)
        sig_bin = f.read(6)
        f.seek(16 * 2048)
        sig_iso = f.read(6)

        if sig_bin == b"\x01CD001":
            sector_size, user_off = 2352, 16
        elif sig_iso == b"\x01CD001":
            sector_size, user_off = 2048, 0
        else:
            raise RuntimeError(f"Not a recognized ISO9660 .iso or Mode 1/2352 .bin image: {image_path}")

        def read_sectors(lba, nbytes):
            out = bytearray()
            rem = nbytes
            cur = lba
            while rem > 0:
                f.seek(cur * sector_size + user_off)
                chunk = f.read(min(2048, rem))
                if not chunk:
                    break
                out.extend(chunk)
                rem -= len(chunk)
                cur += 1
            return bytes(out)

        pvd = read_sectors(16, 2048)
        root_rec = pvd[156:156 + 34]
        root_lba = struct.unpack_from("<I", root_rec, 2)[0]
        root_len = struct.unpack_from("<I", root_rec, 10)[0]

        def walk_dir(lba, length, prefix=""):
            data = read_sectors(lba, length)
            pos = 0
            while pos < len(data):
                rec_len = data[pos]
                if rec_len == 0:
                    pos = ((pos // 2048) + 1) * 2048
                    continue
                rec = data[pos:pos + rec_len]
                pos += rec_len
                elba = struct.unpack_from("<I", rec, 2)[0]
                elen = struct.unpack_from("<I", rec, 10)[0]
                flags = rec[25]
                nlen = rec[32]
                name_raw = rec[33:33 + nlen]
                if name_raw in (b"\x00", b"\x01"):
                    continue
                name = name_raw.decode("latin1").split(";")[0]
                full = f"{prefix}/{name}" if prefix else name
                if flags & 2:
                    found = walk_dir(elba, elen, full)
                    if found:
                        return found
                else:
                    low = full.lower()
                    if low.endswith("setup.exe") and ("fluch" in low or "gold" in low or "mha2" in low or elen > 100_000_000):
                        return (full, elba, elen)
            return None

        hit = walk_dir(root_lba, root_len)
        if not hit:
            raise RuntimeError("Could not find Moorhuhn Adventure 2 Setup.EXE inside disc image.")
        full, elba, elen = hit
        print(f"[ISO] Found {full} ({elen:,} bytes at LBA {elba}), extracting...")
        with open(out_setup_path, "wb") as out:
            rem = elen
            cur = elba
            while rem > 0:
                f.seek(cur * sector_size + user_off)
                chunk = f.read(min(2048, rem))
                out.write(chunk)
                rem -= len(chunk)
                cur += 1
    return out_setup_path


def find_pe_overlay(data):
    """Return file offset where PE sections end (Wise payload overlay)."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    n_sec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_sz = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    sec_off = e_lfanew + 24 + opt_sz
    end = 0
    for i in range(n_sec):
        raw_sz, raw_ptr = struct.unpack_from("<II", data, sec_off + i * 40 + 16)
        end = max(end, raw_ptr + raw_sz)
    return end


def extract_wise_setup(setup_path, out_dir):
    """Extract all game files from a Wise Setup.EXE with full CRC-32 verification."""
    os.makedirs(out_dir, exist_ok=True)
    with open(setup_path, "rb") as f:
        header = f.read(1024 * 1024)
        pe_end = find_pe_overlay(header)

        # Locate Wise W32INST deflate stream near pe_end
        f.seek(pe_end)
        probe = f.read(2 * 1024 * 1024)

        # Scan for the Wise script (raw deflate stream containing %MAINDIR%)
        script_bytes = None
        data_base = None
        for off in range(0, min(len(probe), 131072)):
            try:
                d = zlib.decompressobj(-15)
                out = d.decompress(probe[off:off + 65536])
                if b"%MAINDIR%" in out and (b"mha2.exe" in out.lower() or b"mha2.dat" in out.lower()):
                    # Decompress full script
                    d2 = zlib.decompressobj(-15)
                    script_bytes = d2.decompress(probe[off:])
                    data_base = pe_end + off + (len(probe[off:]) - len(d2.unused_data))
                    break
            except zlib.error:
                continue

        if script_bytes is None:
            # Fallback: scan Wise streams starting after Wise DLLs
            pos = pe_end
            # Find Wise header marker (starts with 0x0000 or DLL name)
            idx = probe.find(b"W32INST")
            if idx >= 0:
                pos = pe_end + idx
            for off in range(0, len(probe) - 16):
                if probe[off:off + 2] in (b"\xec\xbd", b"\xed\xbd", b"\x78\x9c", b"\x78\xda"):
                    for wbits in (-15, 15):
                        try:
                            d = zlib.decompressobj(wbits)
                            out = d.decompress(probe[off:off + 262144])
                            if b"%MAINDIR%" in out and b"mha2" in out.lower():
                                d2 = zlib.decompressobj(wbits)
                                script_bytes = d2.decompress(probe[off:])
                                data_base = pe_end + off + (len(probe[off:]) - len(d2.unused_data))
                                break
                        except zlib.error:
                            pass
                    if script_bytes is not None:
                        break

        if script_bytes is None:
            raise RuntimeError("Could not locate Wise installation script in Setup.EXE.")

        # Parse Wise InstallFile opcodes (0x00) referencing %MAINDIR%\<filename>
        # Record layout before "%MAINDIR%\\":
        #   opcode(1)=0x00, flags(2), def_start(4), def_end(4), date(2), time(2), uncomp_size(4), ... crc32(4)
        # More reliably: search for every "%MAINDIR%\\" entry and read the preceding struct!
        # Wise opcode 0x00 format (40 bytes header before dest path):
        #   +0: 0x00
        #   +1: flags (2 bytes)
        #   +3: def_start (u32), +7: def_end (u32), +11: date (u16), +13: time (u16),
        #   +15: uncomp_size (u32), +19..38: 20 bytes zeros/version, +39: crc32 (u32) -> actually offset varies slightly.
        # Auto-calibrate base_delta using the first valid file (e.g. mha2.exe or readme)!
        entries = []
        for m in re.finditer(rb"%MAINDIR%\\([^\x00]+)\x00", script_bytes):
            rel = m.group(1).decode("latin1").replace("\\", "/")
            # Find the opcode header 40 bytes before "%MAINDIR%\\"
            hdr_pos = m.start() - 40
            if hdr_pos < 0:
                continue
            def_start, def_end = struct.unpack_from("<II", script_bytes, hdr_pos + 2)
            uncomp_sz = struct.unpack_from("<I", script_bytes, hdr_pos + 14)[0]
            crc32_exp = struct.unpack_from("<I", script_bytes, hdr_pos + 36)[0]
            if def_end > def_start and uncomp_sz > 0:
                entries.append((rel, def_start, def_end, uncomp_sz, crc32_exp))

        if not entries:
            raise RuntimeError("Parsed Wise script, but found no %MAINDIR% file entries.")

        # Calibrate exact Wise stream base offset using the first small entry
        file_size = os.path.getsize(setup_path)
        base_off = None
        cal_entry = min(entries, key=lambda e: e[2] - e[1])
        _, c_start, c_end, c_usz, c_crc = cal_entry
        c_len = c_end - c_start
        # Search around pe_end..pe_end+2MB for where def_start lands with valid deflate + CRC match
        f.seek(0)
        search_limit = min(file_size, pe_end + 4 * 1024 * 1024)
        for cand_base in range(pe_end, search_limit):
            if cand_base + c_end > file_size:
                break
            f.seek(cand_base + c_start)
            comp = f.read(c_len)
            try:
                raw = zlib.decompress(comp, -15)
                if len(raw) == c_usz and (zlib.crc32(raw) & 0xFFFFFFFF) == c_crc:
                    base_off = cand_base
                    break
            except zlib.error:
                continue

        if base_off is None:
            raise RuntimeError("Could not calibrate Wise stream base offset.")

        print(f"[WISE] Calibrated payload base at 0x{base_off:x}; extracting {len(entries)} files to '{out_dir}'...")
        os.makedirs(os.path.join(out_dir, "mha2.sav"), exist_ok=True)
        for rel, dstart, dend, usz, crc_exp in entries:
            dst = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(base_off + dstart)
            comp = f.read(dend - dstart)
            raw = zlib.decompress(comp, -15)
            crc_got = zlib.crc32(raw) & 0xFFFFFFFF
            if len(raw) != usz or crc_got != crc_exp:
                raise RuntimeError(f"CRC/size mismatch on {rel}: got {len(raw)}/{crc_got:08x}, expected {usz}/{crc_exp:08x}")
            with open(dst, "wb") as out_f:
                out_f.write(raw)
            print(f"  OK  {rel:<30s} {len(raw):>11,d} bytes  CRC32={crc_got:08x}")


def main():
    ap = argparse.ArgumentParser(description="Extract Moorhuhn Adventure 2 game files from Setup.EXE or CD .bin/.iso")
    ap.add_argument("source", help="Path to Setup.EXE or CD-ROM .bin/.iso image")
    ap.add_argument("-o", "--out", default="game_files", help="Output directory (default: game_files)")
    args = ap.parse_args()

    src = args.source
    tmp_setup = None
    if src.lower().endswith((".bin", ".iso")):
        tmp_setup = os.path.join(args.out, "_Setup_tmp.EXE")
        os.makedirs(args.out, exist_ok=True)
        extract_setup_from_iso_or_bin(src, tmp_setup)
        src = tmp_setup

    try:
        extract_wise_setup(src, args.out)
    finally:
        if tmp_setup and os.path.exists(tmp_setup):
            os.remove(tmp_setup)
    print(f"[DONE] Game files ready in '{args.out}'.")


if __name__ == "__main__":
    main()
