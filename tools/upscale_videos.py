#!/usr/bin/env python3
"""
Moorhuhn Adventure 2 — AI Neural FMV Cutscene Remaster Tool (Real-ESRGAN Video + FFmpeg).

Upscales game_files/*.avi cutscenes (mha2il.avi, mha2ol.avi, mha2sl.avi) on your GPU
using realesr-animevideov3 and writes clean high-bitrate H.264 MP4 replacements into
hd_assets/videos/<name>.mp4, which runtime/win32_avi.c loads automatically in sync!

Usage:
  python3 tools/upscale_videos.py                          # Upscale all 800x600 cutscenes 2x (1600x1200)
  python3 tools/upscale_videos.py --scale 2 --only mha2il  # Upscale intro cutscene only
  python3 tools/upscale_videos.py --scale 4                # Full 4x (3200x2400) cutscenes
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upscale_ai import detect_best_gpu, find_realesrgan_cmd


def probe_fps(path):
    p = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=r_frame_rate", "-of", "default=nw=1:nk=1", path],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True
    )
    txt = (p.stdout or "").strip()
    if "/" in txt:
        num, den = txt.split("/", 1)
        try:
            return float(num) / float(den)
        except Exception:
            pass
    return 25.0


def upscale_one_video(avi_path, out_mp4, cmd_prefix, gpu_id, scale, model, threads, crf):
    fps = probe_fps(avi_path)
    work = tempfile.mkdtemp(prefix="mha2_vid_ai_")
    in_dir = os.path.join(work, "frames_in")
    out_dir = os.path.join(work, "frames_out")
    os.makedirs(in_dir, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)

    try:
        t0 = time.time()
        print(f"[VID] Extracting frames from {avi_path} ({fps:.2f} fps)...")
        # Mild deblock filter before neural upscale cleans Cinepak 4x4 macroblocks
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", avi_path,
             "-vf", "deblock=filter=weak:block=4", "-q:v", "2",
             os.path.join(in_dir, "f_%06d.jpg")],
            check=True
        )
        frames = sorted(glob.glob(os.path.join(in_dir, "f_*.jpg")))
        n_frames = len(frames)
        print(f"[VID] Upscaling {n_frames} frames with {model} {scale}x on GPU {gpu_id}...", flush=True)
        # Process in chunks of 500 frames so VRAM & queues stay rock-solid
        chunk_sz = 500
        for ci in range(0, n_frames, chunk_sz):
            sub_in = os.path.join(work, f"cin_{ci}")
            sub_out = os.path.join(work, f"cout_{ci}")
            os.makedirs(sub_in, exist_ok=True)
            os.makedirs(sub_out, exist_ok=True)
            batch = frames[ci : ci + chunk_sz]
            for fp in batch:
                shutil.move(fp, os.path.join(sub_in, os.path.basename(fp)))
            cmd = cmd_prefix + [
                "-i", sub_in,
                "-o", sub_out,
                "-n", model,
                "-s", str(scale),
                "-t", "256",
                "-f", "jpg",
                "-j", threads,
            ]
            if gpu_id != "auto":
                cmd += ["-g", str(gpu_id)]
            print(f"[VID]   chunk {ci + 1}..{ci + len(batch)} / {n_frames}...", flush=True)
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for fp in glob.glob(os.path.join(sub_out, "f_*.jpg")):
                shutil.move(fp, os.path.join(out_dir, os.path.basename(fp)))
            shutil.rmtree(sub_in, ignore_errors=True)
            shutil.rmtree(sub_out, ignore_errors=True)

        os.makedirs(os.path.dirname(out_mp4), exist_ok=True)
        print(f"[VID] Encoding HD cutscene -> {out_mp4}...", flush=True)
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error",
             "-framerate", f"{fps:.6f}",
             "-i", os.path.join(out_dir, "f_%06d.jpg"),
             "-c:v", "libx264", "-preset", "medium", "-crf", str(crf),
             "-pix_fmt", "yuv420p", "-movflags", "+faststart",
             out_mp4],
            check=True
        )
        dt = time.time() - t0
        sz_mb = os.path.getsize(out_mp4) / (1024 * 1024)
        print(f"[VID] Done: {out_mp4} ({sz_mb:.1f} MB) in {dt:.1f}s.")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="AI Neural Remaster for Moorhuhn Adventure 2 AVI cutscenes")
    ap.add_argument("--game-dir", default="game_files", help="Directory containing .avi files")
    ap.add_argument("-o", "--out", default="hd_assets/videos", help="Output directory (default: hd_assets/videos)")
    ap.add_argument("--only", default="", help="Only process AVI matching substring (e.g. 'mha2il')")
    ap.add_argument("--include-small", action="store_true", help="Also upscale small *h.avi variants")
    ap.add_argument("--scale", type=int, default=2, choices=[2, 3, 4], help="Upscale factor (default: 2 -> 1600x1200)")
    ap.add_argument("--model", default="realesr-animevideov3", help="Model (default: realesr-animevideov3)")
    ap.add_argument("--gpu", default="auto", help="Vulkan GPU ID (default: auto)")
    ap.add_argument("--threads", default="1:1:1", help="load:proc:save threads (default: 1:1:1)")
    ap.add_argument("--crf", type=int, default=16, help="H.264 CRF quality (default: 16)")
    args = ap.parse_args()

    cmd_prefix = find_realesrgan_cmd()
    gpu_id = detect_best_gpu(cmd_prefix) if args.gpu == "auto" else args.gpu

    avis = sorted(glob.glob(os.path.join(args.game_dir, "*.avi")))
    for avi in avis:
        stem = os.path.splitext(os.path.basename(avi))[0]
        if args.only and args.only.lower() not in stem.lower():
            continue
        if not args.include_small and stem.endswith("h"):
            continue
        out_mp4 = os.path.join(args.out, stem + ".mp4")
        upscale_one_video(avi, out_mp4, cmd_prefix, gpu_id, args.scale, args.model, args.threads, args.crf)


if __name__ == "__main__":
    main()
