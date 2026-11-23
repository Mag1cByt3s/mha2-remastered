#!/usr/bin/env python3
"""
Moorhuhn Adventure 2 — True 4K FMV Cutscene Remaster (4x-ClearRealityV1 Streaming GPU Pipeline)
Streams frames directly from ffmpeg -> RTX GPU (4x-ClearRealityV1 FP16) -> ffmpeg H.264 MP4
at native 3200x2400 (4x of 800x600) with zero temp-frame disk bloat!
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
import numpy as np
import torch
import spandrel
import spandrel_extra_arches

spandrel_extra_arches.install()


def probe_video(path: str):
    p = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,r_frame_rate,nb_frames",
         "-of", "csv=p=0", path],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=True,
    )
    parts = p.stdout.strip().split(",")
    w, h = int(parts[0]), int(parts[1])
    fps = parts[2]
    nb = int(parts[3]) if len(parts) > 3 and parts[3].isdigit() else 0
    return w, h, fps, nb


@torch.inference_mode()
def remaster_video(desc, src_avi: str, dst_mp4: str, batch_size: int = 4, device: str = "cuda", dtype=torch.float16):
    w, h, fps, nb = probe_video(src_avi)
    scale = desc.scale
    ow, oh = w * scale, h * scale
    print(f"\n[video] {src_avi} ({w}x{h} @ {fps} fps, ~{nb} frames) -> {dst_mp4} ({ow}x{oh})", flush=True)

    os.makedirs(os.path.dirname(dst_mp4), exist_ok=True)
    tmp_mp4 = dst_mp4 + ".tmp.mp4"

    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-i", src_avi,
         "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"],
        stdout=subprocess.PIPE,
    )
    enc = subprocess.Popen(
        ["ffmpeg", "-y", "-v", "error",
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{ow}x{oh}", "-r", fps, "-i", "pipe:0",
         "-i", src_avi,
         "-map", "0:v:0", "-map", "1:a?",
         "-c:v", "libx264", "-preset", "medium", "-crf", "17", "-pix_fmt", "yuv420p",
         "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart",
         tmp_mp4],
        stdin=subprocess.PIPE,
    )

    frame_bytes = w * h * 3
    done = 0
    t0 = time.time()
    batch = []

    def flush_batch(frames_list):
        nonlocal done
        arr = np.stack(frames_list, axis=0)  # (B, H, W, 3) uint8
        ten = torch.from_numpy(arr).to(device=device, dtype=dtype).permute(0, 3, 1, 2) / 255.0
        pred = desc(ten).clamp_(0.0, 1.0)
        out = (pred.permute(0, 2, 3, 1) * 255.0 + 0.5).to(torch.uint8).cpu().numpy()
        enc.stdin.write(out.tobytes())
        done += len(frames_list)

    exc = None
    try:
        while True:
            buf = dec.stdout.read(frame_bytes)
            if not buf or len(buf) < frame_bytes:
                break
            batch.append(np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3))
            if len(batch) >= batch_size:
                flush_batch(batch)
                batch.clear()
                if done % 200 == 0:
                    el = time.time() - t0
                    rate = done / max(el, 1e-3)
                    eta = (nb - done) / max(rate, 1e-3) if nb > done else 0
                    print(f"  [{done}/{nb or '?'}] {rate:.1f} fps ({el:.0f}s elapsed, ~{eta:.0f}s left)", flush=True)
        if batch:
            flush_batch(batch)
            batch.clear()
    except Exception as e:
        exc = e
        raise
    finally:
        if dec.stdout:
            dec.stdout.close()
        if enc.stdin:
            try:
                enc.stdin.close()
            except BrokenPipeError:
                pass
        dec_ret = dec.wait()
        enc_ret = enc.wait()
        if exc is not None or dec_ret != 0 or enc_ret != 0 or done == 0:
            if os.path.exists(tmp_mp4):
                try:
                    os.remove(tmp_mp4)
                except OSError:
                    pass
        if exc is None:
            if dec_ret != 0:
                raise RuntimeError(f"ffmpeg decoder failed with exit code {dec_ret}")
            if enc_ret != 0:
                raise RuntimeError(f"ffmpeg encoder failed with exit code {enc_ret}")
            if done == 0:
                raise RuntimeError(f"decoded 0 frames from {src_avi}")

    os.replace(tmp_mp4, dst_mp4)
    el = time.time() - t0
    sz_mb = os.path.getsize(dst_mp4) / (1024 * 1024)
    print(f"[video] Done: {dst_mp4} ({done} frames, {sz_mb:.1f} MB) in {el:.1f}s ({done/max(el,1e-3):.1f} fps)", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="tools/models/sr/4x-ClearRealityV1.safetensors")
    ap.add_argument("--out-dir", default="hd_assets/videos")
    ap.add_argument("--batch", type=int, default=4)
    ap.add_argument("--only", nargs="*", default=["mha2il", "mha2ol", "mha2sl"])
    args = ap.parse_args()

    device = "cuda" if torch.cuda.is_available() else "cpu"
    dtype = torch.float16 if device == "cuda" else torch.float32
    dtype_label = "FP16" if dtype == torch.float16 else "FP32"
    print(f"[video] Loading {args.model} in {dtype_label} on {device}...", flush=True)
    desc = spandrel.ModelLoader().load_from_file(args.model)
    desc.to(device=device, dtype=dtype).eval()

    for name in args.only:
        src = os.path.join("game_files", f"{name}.avi")
        dst = os.path.join(args.out_dir, f"{name}.mp4")
        if not os.path.exists(src):
            print(f"[video] Missing {src}, skipping", flush=True)
            continue
        remaster_video(desc, src, dst, batch_size=args.batch, device=device, dtype=dtype)

    # Symlink low-bitrate/50fps variants (ih/oh/sh) -> 4K master MP4s so every cutscene path uses 4K!
    for alias, target in (("mha2ih.mp4", "mha2il.mp4"), ("mha2oh.mp4", "mha2ol.mp4"), ("mha2sh.mp4", "mha2sl.mp4")):
        apth = os.path.join(args.out_dir, alias)
        tpth = os.path.join(args.out_dir, target)
        if os.path.exists(tpth):
            if os.path.lexists(apth):
                os.remove(apth)
            os.symlink(target, apth)
    print("[video] All 4K cutscenes ready!", flush=True)


if __name__ == "__main__":
    main()
