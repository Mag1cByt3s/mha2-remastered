#!/usr/bin/env python3
"""
Moorhuhn Adventure 2 — AI Neural 4x Super-Resolution Pack Builder (Real-ESRGAN Vulkan).

Extracts original 1x assets from game_files/mha2.dat and batch-upscales them on your
GPU via realesrgan-ncnn-vulkan into hd_assets/ (4K backgrounds + 4x RGBA sprites).

Usage:
  python3 tools/upscale_ai.py --backgrounds             # 4x neural 4K backgrounds (realesrgan-x4plus)
  python3 tools/upscale_ai.py --sprites                 # 4x neural RGBA sprites (realesrgan-x4plus-anime)
  python3 tools/upscale_ai.py --all                     # Both 4K backgrounds + 4x RGBA sprites
  python3 tools/upscale_ai.py --all --sprites-scope all # Include all 8,660 animation frames too
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_assets import Mha2Archive, decode_it3_frame, write_png_rgba


def find_upscaler_cmd():
    for bin_name, nix_pkg in (
        ("upscayl-bin", "nixpkgs#upscayl-ncnn"),
        ("realesrgan-ncnn-vulkan", "nixpkgs#realesrgan-ncnn-vulkan"),
    ):
        exe = shutil.which(bin_name)
        if exe:
            return [exe]
    if shutil.which("nix"):
        return ["nix", "shell", "nixpkgs#upscayl-ncnn", "-c", "upscayl-bin"]
    raise RuntimeError("upscayl-bin / realesrgan-ncnn-vulkan not found (and 'nix' is not in PATH).")


def ensure_custom_model(model_dir, model_name):
    """Ensure custom NCNN .param/.bin model exists in model_dir (auto-downloads from Upscayl if missing)."""
    builtin = {"realesrgan-x4plus", "realesrgan-x4plus-anime", "realesr-animevideov3"}
    if model_name in builtin:
        return None
    os.makedirs(model_dir, exist_ok=True)
    param_p = os.path.join(model_dir, f"{model_name}.param")
    bin_p = os.path.join(model_dir, f"{model_name}.bin")
    if os.path.exists(param_p) and os.path.exists(bin_p):
        return model_dir

    # Official Upscayl open-source NCNN model mirrors
    bases = [
        "https://raw.githubusercontent.com/upscayl/custom-models/main/models",
        "https://raw.githubusercontent.com/upscayl/upscayl/main/resources/models",
    ]
    for base in bases:
        p1 = subprocess.run(["curl", "-fsSL", f"{base}/{model_name}.param", "-o", param_p])
        p2 = subprocess.run(["curl", "-fsSL", f"{base}/{model_name}.bin", "-o", bin_p])
        if p1.returncode == 0 and p2.returncode == 0 and os.path.getsize(bin_p) > 1024:
            print(f"[MODEL] Downloaded NCNN model '{model_name}' into '{model_dir}'.", flush=True)
            return model_dir
    raise RuntimeError(f"Could not find or download model '{model_name}' in '{model_dir}'.")


# Backwards-compatible alias for tools/upscale_videos.py
find_realesrgan_cmd = find_upscaler_cmd


def detect_best_gpu(cmd_prefix):
    """Pick discrete NVIDIA/AMD Vulkan GPU over integrated Intel/llvmpipe."""
    try:
        p = subprocess.run(
            cmd_prefix + ["-i", "/nonexistent_probe_file.png", "-o", "/tmp/probe_out.png", "-v"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20
        )
        out = p.stdout or ""
    except Exception:
        return "auto"

    gpus = re.findall(r"\[(\d+)\s+([^\]]+)\]", out)
    seen = {}
    for gid, name in gpus:
        if gid not in seen:
            seen[gid] = name.strip()
    if not seen:
        return "auto"

    # Prefer NVIDIA, then AMD/Radeon, then first non-llvmpipe
    for pat in ("NVIDIA", "GeForce", "RTX", "Radeon", "AMD"):
        for gid, name in seen.items():
            if pat.lower() in name.lower():
                print(f"[GPU] Selected Vulkan device {gid}: {name}")
                return gid
    for gid, name in seen.items():
        if "llvmpipe" not in name.lower():
            print(f"[GPU] Selected Vulkan device {gid}: {name}")
            return gid
    return "auto"


def png_dimensions(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        if len(hdr) >= 24 and hdr[:8] == b"\x89PNG\r\n\x1a\n":
            return struct.unpack(">II", hdr[16:24])
    return (0, 0)


def run_realesrgan_batch(cmd_prefix, in_dir, out_dir, model, scale, gpu_id, threads, chunk_label,
                         timeout_s=300, model_dir=None):
    os.makedirs(out_dir, exist_ok=True)
    cmd = cmd_prefix + [
        "-i", in_dir,
        "-o", out_dir,
        "-n", model,
        "-s", str(scale),
        "-t", "256",
        "-f", "png",
        "-j", threads,
    ]
    if model_dir:
        cmd += ["-m", model_dir]
    if gpu_id != "auto":
        cmd += ["-g", str(gpu_id)]
    t0 = time.time()
    print(f"[AI] Upscaling {chunk_label} ({model} {scale}x, gpu={gpu_id}, threads={threads})...", flush=True)
    try:
        p = subprocess.run(cmd, timeout=timeout_s, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    except subprocess.TimeoutExpired:
        print(f"[WARN] Batch timed out after {timeout_s}s ({chunk_label}); keeping completed outputs & retrying/falling back.", flush=True)
        return False
    if p.returncode != 0:
        print(f"[WARN] upscaler returned {p.returncode} on {chunk_label}; keeping completed outputs.", flush=True)
        return False
    dt = time.time() - t0
    print(f"[AI] Finished {chunk_label} in {dt:.1f}s.", flush=True)
    return True


def upscale_backgrounds(arc, out_dir, cmd_prefix, gpu_id, model, scale, threads,
                        min_w=640, min_h=440, model_dir=None):
    stage_root = tempfile.mkdtemp(prefix="mha2_bg_ai_")
    in_dir = os.path.join(stage_root, "in")
    out_stage = os.path.join(stage_root, "out")
    os.makedirs(in_dir, exist_ok=True)
    mapping = []

    try:
        print("[BG] Extracting 1x original backgrounds from mha2.dat...")
        idx = 0
        for e in arc.entries:
            path = e["path"]
            if not path.endswith("._img"):
                continue
            raw = arc.read_entry(e)
            if len(raw) < 0x50:
                continue
            img_type = struct.unpack_from("<I", raw, 0)[0]
            if img_type != 0:
                continue
            joff, jsz = struct.unpack_from("<II", raw, 0x40)
            if joff + jsz > len(raw) or jsz <= 4:
                continue
            jpg = raw[joff : joff + jsz]
            rel_png = path[:-5] + ".png"
            stage_png = os.path.join(in_dir, f"bg_{idx:05d}.png")
            p = subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error", "-f", "mjpeg", "-i", "pipe:0", stage_png],
                input=jpg, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
            )
            if p.returncode != 0 or not os.path.exists(stage_png):
                continue
            w, h = png_dimensions(stage_png)
            if w < min_w or h < min_h:
                os.remove(stage_png)
                continue
            mapping.append((f"bg_{idx:05d}.png", rel_png, w, h))
            idx += 1

        print(f"[BG] Staged {len(mapping)} full backgrounds (>= {min_w}x{min_h}) for {scale}x neural upscaling.")
        if not mapping:
            return 0

        run_realesrgan_batch(
            cmd_prefix, in_dir, out_stage, model, scale, gpu_id, threads,
            f"{len(mapping)} backgrounds", timeout_s=900, model_dir=model_dir
        )

        # Remove stale background cache so the engine rebuilds it cleanly
        cache_file = os.path.join(out_dir, ".hd_index.cache")
        if os.path.exists(cache_file):
            os.remove(cache_file)

        installed = 0
        for fname, rel_png, ow, oh in mapping:
            src_p = os.path.join(out_stage, fname)
            if not os.path.exists(src_p):
                continue
            dst_p = os.path.join(out_dir, rel_png)
            os.makedirs(os.path.dirname(dst_p), exist_ok=True)
            shutil.move(src_p, dst_p)
            installed += 1
        print(f"[BG] Installed {installed} {scale}x neural backgrounds into '{out_dir}'.")
        return installed
    finally:
        shutil.rmtree(stage_root, ignore_errors=True)


def upscale_jpeg_props(arc, out_dir, cmd_prefix, gpu_id, bg_model, spr_model, scale, threads,
                       bg_model_dir=None, spr_model_dir=None):
    """Upscale ALL remaining img_type==0 JPEG assets (split outdoor rooms objBgO/objBgU/objWolken,
    doors, mirrors, Death/Golem strips, minigame art, savegame thumbnails) into hd_assets/sprites/
    so 100.0% of all 348 JPEG images in mha2.dat are high-definition!"""
    spr_out_root = os.path.join(out_dir, "sprites")
    os.makedirs(spr_out_root, exist_ok=True)
    stage_root = tempfile.mkdtemp(prefix="mha2_jprop_ai_")
    bg_in = os.path.join(stage_root, "bg_in")
    bg_out = os.path.join(stage_root, "bg_out")
    pr_in = os.path.join(stage_root, "pr_in")
    pr_out = os.path.join(stage_root, "pr_out")
    os.makedirs(bg_in, exist_ok=True)
    os.makedirs(pr_in, exist_ok=True)
    bg_map = []
    pr_map = []
    tall_2x = []

    try:
        print("[JPROP] Extracting all remaining 1x JPEG props, split rooms & thumbnails from mha2.dat...", flush=True)
        idx = 0
        for e in arc.entries:
            path = e["path"]
            if not path.endswith("._img"):
                continue
            raw = arc.read_entry(e)
            if len(raw) < 0x50 or struct.unpack_from("<I", raw, 0)[0] != 0:
                continue
            joff, jsz = struct.unpack_from("<II", raw, 0x40)
            if joff + jsz > len(raw) or jsz <= 4:
                continue
            jpg = raw[joff : joff + jsz]
            rel_png = path[:-5] + ".png"
            tmp_png = os.path.join(stage_root, f"tmp_{idx:05d}.png")
            p = subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error", "-f", "mjpeg", "-i", "pipe:0", tmp_png],
                input=jpg, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
            )
            if p.returncode != 0 or not os.path.exists(tmp_png):
                continue
            w, h = png_dimensions(tmp_png)
            # Skip full backgrounds already handled by upscale_backgrounds (>= 640x440)
            if w >= 640 and h >= 440:
                os.remove(tmp_png)
                continue
            if w < 16 or h < 16:
                os.remove(tmp_png)
                continue

            fname = f"jp_{idx:05d}.png"
            idx += 1
            # Cap ultra-tall animation strips so 4x height stays <= 16384 (OpenGL texture limit)
            if h * scale > 16384 or w * scale > 16384:
                tall_2x.append((tmp_png, rel_png, w, h))
                continue

            low = path.lower()
            if "objbg" in low or "objwolken" in low or "objbruecke" in low:
                shutil.move(tmp_png, os.path.join(bg_in, fname))
                bg_map.append((fname, rel_png))
            else:
                shutil.move(tmp_png, os.path.join(pr_in, fname))
                pr_map.append((fname, rel_png))

        installed = 0
        if bg_map:
            run_realesrgan_batch(
                cmd_prefix, bg_in, bg_out, bg_model, scale, gpu_id, threads,
                f"{len(bg_map)} split-room JPEG backgrounds", timeout_s=300, model_dir=bg_model_dir
            )
            for fname, rel_png in bg_map:
                src_p = os.path.join(bg_out, fname)
                if os.path.exists(src_p) and os.path.getsize(src_p) > 0:
                    dst_p = os.path.join(spr_out_root, rel_png)
                    os.makedirs(os.path.dirname(dst_p), exist_ok=True)
                    shutil.move(src_p, dst_p)
                    installed += 1

        if pr_map:
            run_realesrgan_batch(
                cmd_prefix, pr_in, pr_out, spr_model, scale, gpu_id, threads,
                f"{len(pr_map)} JPEG props/doors/thumbnails", timeout_s=300, model_dir=spr_model_dir
            )
            for fname, rel_png in pr_map:
                src_p = os.path.join(pr_out, fname)
                if os.path.exists(src_p) and os.path.getsize(src_p) > 0:
                    dst_p = os.path.join(spr_out_root, rel_png)
                    os.makedirs(os.path.dirname(dst_p), exist_ok=True)
                    shutil.move(src_p, dst_p)
                    installed += 1

        if tall_2x:
            t_in = os.path.join(stage_root, "tall_in")
            t_out = os.path.join(stage_root, "tall_out")
            os.makedirs(t_in, exist_ok=True)
            t_map = []
            for ti, (tmp_p, rel_png, ow, oh) in enumerate(tall_2x):
                fn = f"t_{ti:04d}.png"
                shutil.move(tmp_p, os.path.join(t_in, fn))
                t_map.append((fn, rel_png, ow, oh))
            run_realesrgan_batch(
                cmd_prefix, t_in, t_out, spr_model, 4, gpu_id, threads,
                f"{len(t_map)} tall JPEG strips (downscaled to 2x)", timeout_s=300, model_dir=spr_model_dir
            )
            for fn, rel_png, ow, oh in t_map:
                src_p = os.path.join(t_out, fn)
                dst_p = os.path.join(spr_out_root, rel_png)
                os.makedirs(os.path.dirname(dst_p), exist_ok=True)
                if os.path.exists(src_p) and os.path.getsize(src_p) > 0 and ow * 2 <= 16384 and oh * 2 <= 16384:
                    subprocess.run(
                        ["ffmpeg", "-y", "-loglevel", "error", "-i", src_p,
                         "-vf", f"scale={ow*2}:{oh*2}:flags=lanczos", dst_p],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                    )
                    if os.path.exists(dst_p):
                        installed += 1

        cache_file = os.path.join(out_dir, ".hd_sprite_index.cache")
        if os.path.exists(cache_file):
            os.remove(cache_file)
        print(f"[JPROP] Installed {installed} HD JPEG props/split-rooms/thumbnails into '{spr_out_root}'.", flush=True)
        return installed
    finally:
        shutil.rmtree(stage_root, ignore_errors=True)


def should_include_sprite(vpath, frame_cnt, scope):
    if scope == "all":
        return True
    # 'core' scope: all single-frame sprites (buttons, inventory, static props),
    # all cursors (g/img/mc*), all inventory icons (inv*), and key character/menu sprites (<= 16 frames)
    low = vpath.lower()
    if "/mc" in low or "/inv" in low or low.startswith("x/") or low.startswith("g/"):
        return True
    if frame_cnt <= 16:
        return True
    return False


def upscale_sprites(arc, out_dir, cmd_prefix, gpu_id, model, scale, threads,
                    scope="all", batch_size=300, model_dir=None, force=False):
    spr_out_root = os.path.join(out_dir, "sprites")
    os.makedirs(spr_out_root, exist_ok=True)
    stage_root = tempfile.mkdtemp(prefix="mha2_spr_ai_")
    installed = 0

    try:
        print(f"[SPR] Extracting 1x RGBA IT3 sprites (scope={scope}, force={force}) from mha2.dat...", flush=True)
        batch_idx = 0
        cur_batch = []

        def flush_batch(items, bnum):
            nonlocal installed
            if not items:
                return
            in_dir = os.path.join(stage_root, f"in_{bnum:03d}")
            out_stage = os.path.join(stage_root, f"out_{bnum:03d}")
            run_realesrgan_batch(
                cmd_prefix, in_dir, out_stage, model, scale, gpu_id, threads,
                f"sprite batch #{bnum + 1} ({len(items)} sprites)", timeout_s=180, model_dir=model_dir
            )
            missing = []
            for fname, rel_png in items:
                src_p = os.path.join(out_stage, fname)
                dst_p = os.path.join(spr_out_root, rel_png)
                if os.path.exists(src_p) and os.path.getsize(src_p) > 0:
                    os.makedirs(os.path.dirname(dst_p), exist_ok=True)
                    shutil.move(src_p, dst_p)
                    installed += 1
                else:
                    missing.append((fname, rel_png))
            # Retry any missing files once in a clean sub-batch, then Lanczos fallback if still missing
            if missing:
                retry_in = os.path.join(stage_root, f"rin_{bnum:03d}")
                retry_out = os.path.join(stage_root, f"rout_{bnum:03d}")
                os.makedirs(retry_in, exist_ok=True)
                for fname, _ in missing:
                    shutil.copy2(os.path.join(in_dir, fname), os.path.join(retry_in, fname))
                run_realesrgan_batch(
                    cmd_prefix, retry_in, retry_out, model, scale, gpu_id, "1:1:1",
                    f"sprite retry #{bnum + 1} ({len(missing)} sprites)", timeout_s=120, model_dir=model_dir
                )
                for fname, rel_png in missing:
                    src_p = os.path.join(retry_out, fname)
                    dst_p = os.path.join(spr_out_root, rel_png)
                    os.makedirs(os.path.dirname(dst_p), exist_ok=True)
                    if os.path.exists(src_p) and os.path.getsize(src_p) > 0:
                        shutil.move(src_p, dst_p)
                        installed += 1
                    else:
                        # Clean Lanczos 4x fallback so zero sprites are skipped
                        subprocess.run(
                            ["ffmpeg", "-y", "-loglevel", "error", "-i", os.path.join(in_dir, fname),
                             "-vf", f"scale=iw*{scale}:ih*{scale}:flags=lanczos", dst_p],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                        )
                        if os.path.exists(dst_p):
                            installed += 1
                shutil.rmtree(retry_in, ignore_errors=True)
                shutil.rmtree(retry_out, ignore_errors=True)
            shutil.rmtree(in_dir, ignore_errors=True)
            shutil.rmtree(out_stage, ignore_errors=True)

        cur_in_dir = os.path.join(stage_root, f"in_{batch_idx:03d}")
        os.makedirs(cur_in_dir, exist_ok=True)
        local_i = 0

        for e in arc.entries:
            path = e["path"]
            if not path.endswith("._img"):
                continue
            raw = arc.read_entry(e)
            if len(raw) < 0x50:
                continue
            img_type = struct.unpack_from("<I", raw, 0)[0]
            if img_type != 1:
                continue
            pal_off, pal_cnt = struct.unpack_from("<II", raw, 0x28)
            palette = list(struct.unpack_from(f"<{pal_cnt}I", raw, pal_off)) if (pal_off and pal_cnt and pal_off + pal_cnt * 4 <= len(raw)) else []
            t0_off, t0_cnt, tbl_off, tbl_cnt = struct.unpack_from("<4I", raw, 0x38)
            if not tbl_off or not tbl_cnt:
                continue

            # Composite bitmap font atlases (fntWhite/fntGray/fntYellow/fntOtherSpeakers/fntBig/objFont)
            # into a single full-surface RGBA sheet matching mha2.exe's DirectDraw surface layout!
            if "/fnt" in path.lower() or path.endswith("/objFont._img"):
                _, _, sw, sh = struct.unpack_from("<4I", raw, 0)
                if sw > 0 and sh > 0:
                    rel_png = path[:-5] + ".png"
                    dst_existing = os.path.join(spr_out_root, rel_png)
                    if not force and os.path.exists(dst_existing):
                        ew, eh = png_dimensions(dst_existing)
                        if ew == sw * scale and eh == sh * scale:
                            installed += 1
                            continue
                    canvas = bytearray(sw * sh * 4)
                    for fi in range(min(t0_cnt, tbl_cnt)):
                        foff = struct.unpack_from("<I", raw, tbl_off + fi * 4)[0]
                        if not foff:
                            continue
                        cx, cy, cw, ch = struct.unpack_from("<4I", raw, t0_off + fi * 17)
                        dec = decode_it3_frame(raw, foff, palette, cw, ch)
                        if not dec:
                            continue
                        fw, fh, rgba = dec
                        for y in range(fh):
                            dy = cy + y
                            if dy < 0 or dy >= sh:
                                continue
                            for x in range(fw):
                                dx = cx + x
                                if dx < 0 or dx >= sw:
                                    continue
                                si = (y * fw + x) * 4
                                if rgba[si + 3] > 0:
                                    di = (dy * sw + dx) * 4
                                    canvas[di : di + 4] = rgba[si : si + 4]
                    fname = f"s_{local_i:05d}.png"
                    write_png_rgba(os.path.join(cur_in_dir, fname), sw, sh, bytes(canvas))
                    cur_batch.append((fname, rel_png))
                    local_i += 1
                    if len(cur_batch) >= batch_size:
                        flush_batch(cur_batch, batch_idx)
                        batch_idx += 1
                        cur_batch = []
                        local_i = 0
                continue
            if not should_include_sprite(path, tbl_cnt, scope):
                continue

            rel_base = path[:-5]
            for fi in range(tbl_cnt):
                foff = struct.unpack_from("<I", raw, tbl_off + fi * 4)[0]
                if not foff:
                    continue
                cw, ch = 0, 0
                if t0_off and fi < t0_cnt and t0_off + (fi + 1) * 17 <= len(raw):
                    _, _, cw, ch = struct.unpack_from("<4I", raw, t0_off + fi * 17)
                dec = decode_it3_frame(raw, foff, palette, cw, ch)
                if not dec:
                    continue
                w, h, rgba = dec
                if w < 16 or h < 16:
                    continue
                suffix = "" if tbl_cnt == 1 else f"_f{fi:03d}"
                rel_png = rel_base + suffix + ".png"
                dst_existing = os.path.join(spr_out_root, rel_png)
                if not force and os.path.exists(dst_existing):
                    ew, eh = png_dimensions(dst_existing)
                    if ew == w * scale and eh == h * scale:
                        installed += 1
                        continue
                fname = f"s_{local_i:05d}.png"
                write_png_rgba(os.path.join(cur_in_dir, fname), w, h, rgba)
                cur_batch.append((fname, rel_png))
                local_i += 1
                if len(cur_batch) >= batch_size:
                    flush_batch(cur_batch, batch_idx)
                    batch_idx += 1
                    cur_batch = []
                    local_i = 0
                    cur_in_dir = os.path.join(stage_root, f"in_{batch_idx:03d}")
                    os.makedirs(cur_in_dir, exist_ok=True)

        flush_batch(cur_batch, batch_idx)

        cache_file = os.path.join(out_dir, ".hd_sprite_index.cache")
        if os.path.exists(cache_file):
            os.remove(cache_file)
        print(f"[SPR] Installed {installed} {scale}x neural RGBA sprites into '{spr_out_root}'.")
        return installed
    finally:
        shutil.rmtree(stage_root, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Build 4x Real-ESRGAN AI Neural HD Pack for Moorhuhn Adventure 2")
    ap.add_argument("--dat", default="game_files/mha2.dat", help="Path to mha2.dat")
    ap.add_argument("-o", "--out", default="hd_assets", help="Target HD pack directory (default: hd_assets)")
    ap.add_argument("--backgrounds", action="store_true", help="AI-upscale all full backgrounds (menus & rooms)")
    ap.add_argument("--jpeg-props", action="store_true", help="AI-upscale remaining JPEG props, split rooms & thumbnails")
    ap.add_argument("--sprites", action="store_true", help="AI-upscale RGBA IT3 sprites (characters, props, inventory, cursors)")
    ap.add_argument("--all", action="store_true", help="AI-upscale everything (backgrounds + JPEG props + IT3 sprites)")
    ap.add_argument("--sprites-scope", choices=["core", "all"], default="all",
                    help="Sprite scope: 'core' (~2k key sprites) or 'all' (all 8,660 frames, default)")
    ap.add_argument("--bg-model", default="4x_NMKD-Siax_200k",
                    help="Model for backgrounds (default: 4x_NMKD-Siax_200k)")
    ap.add_argument("--spr-model", default="ultrasharp-4x",
                    help="Model for sprites (default: ultrasharp-4x)")
    ap.add_argument("--model-dir", default="tools/models",
                    help="Directory for custom NCNN .param/.bin models (default: tools/models)")
    ap.add_argument("--force", action="store_true",
                    help="Overwrite existing upscaled sprites instead of skipping")
    ap.add_argument("--scale", type=int, default=4, choices=[2, 4], help="Upscale factor (default: 4)")
    ap.add_argument("--gpu", default="auto", help="Vulkan GPU ID (default: auto-detect NVIDIA/AMD)")
    ap.add_argument("--threads", default="1:1:1", help="load:proc:save threads (default: 1:1:1)")
    args = ap.parse_args()

    if not (args.backgrounds or args.jpeg_props or args.sprites or args.all):
        args.all = True

    cmd_prefix = find_upscaler_cmd()
    gpu_id = detect_best_gpu(cmd_prefix) if args.gpu == "auto" else args.gpu

    arc = Mha2Archive(args.dat)
    try:
        bg_mdir = ensure_custom_model(args.model_dir, args.bg_model) if (args.backgrounds or args.jpeg_props or args.all) else None
        spr_mdir = ensure_custom_model(args.model_dir, args.spr_model) if (args.jpeg_props or args.sprites or args.all) else None
        if args.backgrounds or args.all:
            upscale_backgrounds(arc, args.out, cmd_prefix, gpu_id, args.bg_model, args.scale,
                                args.threads, model_dir=bg_mdir)
        if args.jpeg_props or args.all:
            upscale_jpeg_props(arc, args.out, cmd_prefix, gpu_id, args.bg_model, args.spr_model,
                               args.scale, args.threads, bg_model_dir=bg_mdir, spr_model_dir=spr_mdir)
        if args.sprites or args.all:
            upscale_sprites(arc, args.out, cmd_prefix, gpu_id, args.spr_model, args.scale,
                            args.threads, scope=args.sprites_scope, model_dir=spr_mdir, force=args.force)
    finally:
        arc.close()


if __name__ == "__main__":
    main()
