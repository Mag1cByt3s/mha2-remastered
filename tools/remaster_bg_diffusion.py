#!/usr/bin/env python3
"""
Moorhuhn Adventure 2 - Generative 4K Room Background Remaster (SDXL + ControlNet-Tile)
Produces true modern-game 4K PBR-like textures with zero baked-in 2005 jaggies while
locking 100% pixel alignment to interactive hotspots and foreground props.

Usage:
  LD_LIBRARY_PATH="$(dirname $(find /nix/store -maxdepth 3 -name libstdc++.so.6 2>/dev/null | head -n1)):/run/opengl-driver/lib" \
    work/diffusion_venv/bin/python tools/remaster_bg_diffusion.py \
      --samples work/diffusion_samples \
      hd_assets/l/001/img/xobjBg.png hd_assets/l/005/img/objBg.png hd_assets/l/010/img/objBg.png
"""
import argparse
import os
import sys
import time
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import cv2
import torch
from diffusers import (
    ControlNetModel,
    StableDiffusionXLControlNetImg2ImgPipeline,
    DPMSolverMultistepScheduler,
    AutoencoderKL,
)

BASE_MODEL = "RunDiffusion/Juggernaut-XL-v9"
CONTROLNET_MODEL = "xinsir/controlnet-tile-sdxl-1.0"

PROMPT = (
    "modern AAA stylized 3D point-and-click adventure game environment, Unreal Engine 5 render, "
    "crisp physically based materials, fine wood grain, carved weathered stone, rich woven fabric, "
    "clean smooth anti-aliased edges, intricate sharp 4K textures, rich cinematic lighting, "
    "subtle raytraced ambient occlusion, faithful remaster"
)
NEG_PROMPT = (
    "blurry, low resolution, jpeg artifacts, aliasing, jagged staircase edges, pixelated, "
    "muddy, smeary oil paint, plastic sheen, text, watermark, signature, extra objects, "
    "changed layout, distorted geometry, noisy grain"
)


def match_lowfreq_color(remastered: Image.Image, reference: Image.Image, sigma: float = 96.0, mix: float = 0.65) -> Image.Image:
    """Keep the remastered room's exact global lighting & color temperature aligned with
    the original so foreground sprites (Moorhuhn, props, doors) blend seamlessly."""
    rem = np.asarray(remastered, dtype=np.float32)
    ref = np.asarray(reference.resize(remastered.size, Image.LANCZOS), dtype=np.float32)

    # Compute low-frequency illumination delta in Lab space
    rem_lab = cv2.cvtColor(rem / 255.0, cv2.COLOR_RGB2LAB)
    ref_lab = cv2.cvtColor(ref / 255.0, cv2.COLOR_RGB2LAB)

    k = int(sigma * 3) | 1
    rem_low = cv2.GaussianBlur(rem_lab, (k, k), sigma)
    ref_low = cv2.GaussianBlur(ref_lab, (k, k), sigma)

    out_lab = rem_lab + (ref_low - rem_low) * mix
    out_lab[..., 0] = np.clip(out_lab[..., 0], 0.0, 100.0)
    out_lab[..., 1] = np.clip(out_lab[..., 1], -128.0, 127.0)
    out_lab[..., 2] = np.clip(out_lab[..., 2], -128.0, 127.0)

    out_rgb = cv2.cvtColor(out_lab, cv2.COLOR_LAB2RGB)
    return Image.fromarray(np.clip(out_rgb * 255.0, 0, 255).astype(np.uint8))


def load_pipeline():
    print(f"[diffusion] Loading ControlNet: {CONTROLNET_MODEL}", flush=True)
    controlnet = ControlNetModel.from_pretrained(
        CONTROLNET_MODEL,
        torch_dtype=torch.float16,
    )
    print(f"[diffusion] Loading SDXL base:  {BASE_MODEL}", flush=True)
    pipe = StableDiffusionXLControlNetImg2ImgPipeline.from_pretrained(
        BASE_MODEL,
        controlnet=controlnet,
        torch_dtype=torch.float16,
        variant="fp16",
        use_safetensors=True,
    )
    pipe.scheduler = DPMSolverMultistepScheduler.from_config(
        pipe.scheduler.config,
        use_karras_sigmas=True,
        algorithm_type="dpmsolver++",
    )
    pipe.vae.enable_tiling()
    pipe.vae.enable_slicing()
    pipe.enable_model_cpu_offload()
    return pipe


def make_feather_mask(w: int, h: int, overlap_l: int, overlap_r: int, overlap_t: int, overlap_b: int) -> np.ndarray:
    """Cosine-feathered 2D weight mask for seamless tile blending."""
    wx = np.ones(w, dtype=np.float32)
    wy = np.ones(h, dtype=np.float32)
    if overlap_l > 0:
        t = np.linspace(0.0, np.pi * 0.5, overlap_l, dtype=np.float32)
        wx[:overlap_l] = np.sin(t) ** 2
    if overlap_r > 0:
        t = np.linspace(0.0, np.pi * 0.5, overlap_r, dtype=np.float32)
        wx[-overlap_r:] = np.cos(t) ** 2
    if overlap_t > 0:
        t = np.linspace(0.0, np.pi * 0.5, overlap_t, dtype=np.float32)
        wy[:overlap_t] = np.sin(t) ** 2
    if overlap_b > 0:
        t = np.linspace(0.0, np.pi * 0.5, overlap_b, dtype=np.float32)
        wy[-overlap_b:] = np.cos(t) ** 2
    return np.outer(wy, wx)[..., None]


def remaster_image(pipe, src_img: Image.Image, seed: int = 42,
                   s1_strength: float = 0.46, s2_strength: float = 0.36) -> Image.Image:
    orig_w, orig_h = src_img.size

    # Stage 1: Global pass at 1600x896 (exact 2x of 800x450, divisible by 8)
    # Eliminates baked-in 2005 1x jaggies & synthesizes coherent modern PBR materials.
    s1_w = ((orig_w // 2) // 8) * 8
    s1_h = ((orig_h // 2) // 8) * 8
    s1_in = src_img.resize((s1_w, s1_h), Image.LANCZOS)

    gen = torch.Generator(device="cuda").manual_seed(seed)
    print(f"  [stage 1] Global pass {s1_w}x{s1_h} (strength={s1_strength})...", flush=True)
    t0 = time.time()
    s1_out = pipe(
        prompt=PROMPT,
        negative_prompt=NEG_PROMPT,
        image=s1_in,
        control_image=s1_in,
        strength=s1_strength,
        controlnet_conditioning_scale=0.72,
        num_inference_steps=32,
        guidance_scale=6.5,
        generator=gen,
    ).images[0]
    print(f"  [stage 1] Done in {time.time() - t0:.1f}s", flush=True)

    # Stage 2: Seamless 3x2 tiled 4K micro-detail pass at SDXL's native ~1152x1024 sweet spot
    s2_base = s1_out.resize((orig_w, orig_h), Image.LANCZOS)
    ctrl_full = Image.blend(src_img.resize((orig_w, orig_h), Image.LANCZOS), s2_base, 0.65)

    overlap = 224
    # 3 columns across 3200px (~1200px each), 2 rows across 1800px (~1016px each)
    x_ranges = [
        (0, 1216),
        (992, 2208),
        (1984, orig_w),
    ]
    y_ranges = [
        (0, 1016),
        (792, orig_h),
    ]

    accum = np.zeros((orig_h, orig_w, 3), dtype=np.float32)
    weight = np.zeros((orig_h, orig_w, 1), dtype=np.float32)

    total_tiles = len(x_ranges) * len(y_ranges)
    tile_idx = 0
    for yi, (y0, y1) in enumerate(y_ranges):
        for xi, (x0, x1) in enumerate(x_ranges):
            tile_idx += 1
            tw, th = x1 - x0, y1 - y0
            rw = (tw // 8) * 8
            rh = (th // 8) * 8
            tile_in = s2_base.crop((x0, y0, x1, y1))
            tile_ctrl = ctrl_full.crop((x0, y0, x1, y1))
            if (rw, rh) != (tw, th):
                tile_in = tile_in.resize((rw, rh), Image.LANCZOS)
                tile_ctrl = tile_ctrl.resize((rw, rh), Image.LANCZOS)

            t1 = time.time()
            print(f"  [stage 2] Tile {tile_idx}/{total_tiles} ({rw}x{rh}, strength={s2_strength})...", flush=True)
            tgen = torch.Generator(device="cuda").manual_seed(seed + tile_idx)
            tile_out = pipe(
                prompt=PROMPT,
                negative_prompt=NEG_PROMPT,
                image=tile_in,
                control_image=tile_ctrl,
                strength=s2_strength,
                controlnet_conditioning_scale=0.76,
                num_inference_steps=30,
                guidance_scale=6.5,
                generator=tgen,
            ).images[0]
            if tile_out.size != (tw, th):
                tile_out = tile_out.resize((tw, th), Image.LANCZOS)
            print(f"  [stage 2] Tile {tile_idx}/{total_tiles} done in {time.time() - t1:.1f}s", flush=True)

            ov_l = overlap if xi > 0 else 0
            ov_r = overlap if xi < len(x_ranges) - 1 else 0
            ov_t = overlap if yi > 0 else 0
            ov_b = overlap if yi < len(y_ranges) - 1 else 0
            mask = make_feather_mask(tw, th, ov_l, ov_r, ov_t, ov_b)

            accum[y0:y1, x0:x1] += np.asarray(tile_out, dtype=np.float32) * mask
            weight[y0:y1, x0:x1] += mask

    blended = np.clip(accum / np.maximum(weight, 1e-6), 0, 255).astype(np.uint8)
    out_img = Image.fromarray(blended)
    out_img = match_lowfreq_color(out_img, src_img, sigma=96.0, mix=0.65)
    return out_img


def draw_label(im: Image.Image, text: str, xy=(24, 20)):
    draw = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype("/run/current-system/sw/share/X11/fonts/DejaVuSans-Bold.ttf", 34)
    except Exception:
        font = ImageFont.load_default()
    bbox = draw.textbbox(xy, text, font=font)
    pad = 12
    draw.rectangle((bbox[0] - pad, bbox[1] - pad, bbox[2] + pad, bbox[3] + pad), fill=(15, 18, 28))
    draw.text(xy, text, fill=(255, 224, 120), font=font)


def save_comparisons(before: Image.Image, after: Image.Image, out_dir: str, tag: str):
    os.makedirs(out_dir, exist_ok=True)
    # 1. Full remastered 4K output
    after.save(os.path.join(out_dir, f"{tag}_after_4k.png"), compress_level=3)

    # 2. Full-room 50/50 vertical split comparison (1920x1080 for quick full-screen viewing)
    w, h = 1920, 1080
    b_rs = before.resize((w, h), Image.LANCZOS)
    a_rs = after.resize((w, h), Image.LANCZOS)
    split = Image.new("RGB", (w, h))
    split.paste(b_rs.crop((0, 0, w // 2, h)), (0, 0))
    split.paste(a_rs.crop((w // 2, 0, w, h)), (w // 2, 0))
    draw = ImageDraw.Draw(split)
    draw.line([(w // 2, 0), (w // 2, h)], fill=(255, 215, 64), width=4)
    draw_label(split, "BEFORE: GAN 4x (NMKD-Siax)", (24, 20))
    draw_label(split, "AFTER: SDXL ControlNet-Tile 4K", (w // 2 + 24, 20))
    split.save(os.path.join(out_dir, f"{tag}_split.png"))

    # 3. Side-by-side full room (top = BEFORE, bottom = AFTER) at 1600x1800
    sw, sh = 1600, 900
    stack = Image.new("RGB", (sw, sh * 2 + 6), (255, 215, 64))
    b_st = before.resize((sw, sh), Image.LANCZOS)
    a_st = after.resize((sw, sh), Image.LANCZOS)
    draw_label(b_st, f"BEFORE (GAN 4x) - {tag}", (20, 16))
    draw_label(a_st, f"AFTER (SDXL ControlNet-Tile 4K) - {tag}", (20, 16))
    stack.paste(b_st, (0, 0))
    stack.paste(a_st, (0, sh + 6))
    stack.save(os.path.join(out_dir, f"{tag}_full_compare.png"))

    # 4. Native 1:1 4K detail crops (3 regions: top-center, mid-right, bottom-left)
    bw, bh = before.size
    cw, ch = 720, 540
    crops = [
        ("Detail 1 (Top Center)", (bw // 2 - cw // 2, 0)),
        ("Detail 2 (Mid Right)",  (int(bw * 0.62), int(bh * 0.25))),
        ("Detail 3 (Bottom Left)", (int(bw * 0.08), int(bh * 0.58))),
    ]
    sheet = Image.new("RGB", (cw * 2 + 8, (ch + 8) * len(crops)), (25, 28, 38))
    for i, (name, (cx, cy)) in enumerate(crops):
        cx = max(0, min(cx, bw - cw))
        cy = max(0, min(cy, bh - ch))
        cb = before.crop((cx, cy, cx + cw, cy + ch))
        ca = after.crop((cx, cy, cx + cw, cy + ch))
        draw_label(cb, f"BEFORE 1:1 - {name}", (12, 10))
        draw_label(ca, f"AFTER 1:1 - {name}", (12, 10))
        y = i * (ch + 8)
        sheet.paste(cb, (0, y))
        sheet.paste(ca, (cw + 8, y))
    sheet.save(os.path.join(out_dir, f"{tag}_zoom_1to1.png"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", required=True, help="Output directory for before/after comparisons")
    ap.add_argument("--s1-strength", type=float, default=0.46)
    ap.add_argument("--s2-strength", type=float, default=0.36)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("inputs", nargs="+", help="Input 4K room background PNGs")
    args = ap.parse_args()

    pipe = load_pipeline()
    for p in args.inputs:
        room = os.path.basename(os.path.dirname(os.path.dirname(p)))
        base = os.path.splitext(os.path.basename(p))[0]
        tag = f"room_{room}_{base}"
        print(f"\n[remaster] Processing {p} -> {tag}", flush=True)
        src = Image.open(p).convert("RGB")
        out = remaster_image(pipe, src, seed=args.seed,
                             s1_strength=args.s1_strength, s2_strength=args.s2_strength)
        save_comparisons(src, out, args.samples, tag)
        print(f"[remaster] Saved comparisons for {tag} in {args.samples}/", flush=True)


if __name__ == "__main__":
    main()
