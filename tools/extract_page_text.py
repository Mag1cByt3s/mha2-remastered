#!/usr/bin/env python3
"""Page text for the in-game page reader, read locally from the player's own HD images.

Each page (tools/text_screens.py PAGE_BOXES) is read by three OCR vision models of
independent families, one at a time on the GPU:
  - PaddleOCR-VL (Apache-2.0; also gives the paragraph structure)
  - Qwen3-VL-4B-Instruct (Apache-2.0)
  - GLM-OCR (MIT)
The readings are aligned word by word. Every word needs a clear majority: at least two
families agree and lead every alternative by a full vote. Only if every word of every
page of a screen passes, hd_assets/<stem>.txt gets the text; otherwise the file is left
empty and the reader offers the zoom view for that screen instead. Models from the same
family make the same mistakes, which is why one model per family votes.

Benchmarked against hand transcriptions (CER per model: GLM-OCR 0.15 %, Qwen3-VL-4B
0.17 %, PaddleOCR-VL 1.0 %): the vote accepted 9 of 13 screens, all letter-perfect, and
rejected the occluded word, the handwritten caption and the pseudo-Arabic notice board.

Reader file format (UTF-8): one paragraph per line, "---" separates pages, an empty file
means "zoom view only". Files with text are never overwritten without --force (they may
be hand-corrected); empty files are redone on every run.

Usage (nix-shell nix/assets-shell.nix; models download on first use, ~11 GB):
  python3 tools/extract_page_text.py [--force] [--hd-dir hd_assets] [stem ...]
"""
import argparse
import difflib
import gc
import os
import re
import sys
from collections import Counter

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from text_screens import NO_READER, PAGE_BOXES, TEXT_SCREENS

PROMPT_DE = ("Transkribiere den gesamten Text auf diesem Bild exakt Zeichen für Zeichen, "
             "in Lesereihenfolge. Gib nur den Text aus, ohne Kommentar und ohne Formatierung.")

# (cache name, family, Hugging Face model, prompt, input scale). The first one is the
# structural primary: its paragraphs are kept, its words corrected by the vote.
ENGINES = (
    ("paddleocr_vl", "paddle", "PaddlePaddle/PaddleOCR-VL", "OCR:", 0.5),
    ("qwen3vl_4b", "qwen3", "Qwen/Qwen3-VL-4B-Instruct", PROMPT_DE, 1.0),
    ("glm_ocr", "glm", "zai-org/GLM-OCR", "Text Recognition:", 1.0),
)
MIN_FAMILIES = 2.0   # winner needs this many families ...
MARGIN = 1.0         # ... and must lead the runner-up by a full family


def page_crops(hd_dir, stem):
    im = Image.open(os.path.join(hd_dir, stem + ".webp")).convert("RGB")
    s = im.width / 800.0
    return [im.crop((round(a * s), round(b * s), round(c * s), round(d * s)))
            for a, b, c, d in PAGE_BOXES[stem]]


class VisionOCR:
    """One transformers image-text-to-text model, greedy decoding, bf16 on the GPU."""

    def __init__(self, model_id, prompt, scale):
        import torch
        from transformers import AutoModelForImageTextToText, AutoProcessor
        self.torch, self.prompt, self.scale = torch, prompt, scale
        self.dev = "cuda" if torch.cuda.is_available() else "cpu"
        self.proc = AutoProcessor.from_pretrained(model_id)
        self.model = AutoModelForImageTextToText.from_pretrained(
            model_id, dtype=torch.bfloat16 if self.dev == "cuda" else torch.float32).to(self.dev).eval()

    def read(self, img):
        if self.scale != 1.0:
            img = img.resize((round(img.width * self.scale), round(img.height * self.scale)), Image.LANCZOS)
        msgs = [{"role": "user", "content": [{"type": "image", "image": img},
                                             {"type": "text", "text": self.prompt}]}]
        inputs = self.proc.apply_chat_template(msgs, add_generation_prompt=True, tokenize=True,
                                               return_dict=True, return_tensors="pt").to(self.dev)
        if "pixel_values" in inputs:
            inputs["pixel_values"] = inputs["pixel_values"].to(self.model.dtype)
        with self.torch.inference_mode():
            out = self.model.generate(**inputs, max_new_tokens=2048, do_sample=False)
        return self.proc.batch_decode(out[:, inputs["input_ids"].shape[1]:], skip_special_tokens=True)[0]

    def close(self):
        del self.model
        gc.collect()
        if self.dev == "cuda":
            self.torch.cuda.empty_cache()


# ---------------------------------------------------------------- voting
_FOLD = str.maketrans({"„": '"', "“": '"', "”": '"', "«": '"', "»": '"', "‚": "'", "‘": "'",
                       "’": "'", "–": "-", "—": "-", "…": "...", "ﬁ": "fi", "ﬂ": "fl"})


def paragraphs(raw):
    """OCR text -> list of paragraphs, each a list of words. Layout hyphenation is joined,
    markdown noise dropped; blank lines and line breaks both end a paragraph here (only
    the primary's structure is used)."""
    raw = re.sub(r"```\w*|\*\*|^#+\s*", "", raw, flags=re.M).replace("\r", "")
    raw = re.sub(r"(\w)-\n\s*(?=[a-zäöüß])", r"\1", raw)
    return [line.split() for line in raw.split("\n") if line.strip()]


def vote_page(readings, weights):
    """Word-level vote. readings[i] = list of paragraphs (word lists); readings[0] is the
    primary. Returns (paragraphs of voted words, number of words without clear majority)."""
    prim_paras = readings[0]
    prim = [w for p in prim_paras for w in p]
    if not prim:
        return [], 1   # primary read nothing: no structure to vote into
    para_of = [i for i, p in enumerate(prim_paras) for _ in p]
    key = lambda w: w.translate(_FOLD)
    pk = [key(w) for w in prim]
    cands = [[((pk[i],), (prim[i],), weights[0])] for i in range(len(prim))]
    gaps = [[] for _ in range(len(prim) + 1)]
    for other, wt in zip(readings[1:], weights[1:]):
        ow = [w for p in other for w in p]
        ok = [key(w) for w in ow]
        for op, a, b, c, d in difflib.SequenceMatcher(None, pk, ok, autojunk=False).get_opcodes():
            if op == "equal" or (op == "replace" and b - a == d - c):
                for k in range(b - a):
                    cands[a + k].append(((ok[c + k],), (ow[c + k],), wt))
            elif op in ("replace", "delete"):
                cands[a].append((tuple(ok[c:d]), tuple(ow[c:d]), wt))
                for k in range(a + 1, b):
                    cands[k].append(((), (), wt))
            elif op == "insert":
                gaps[a].append((tuple(ok[c:d]), tuple(ow[c:d]), wt))

    def winner(items, absent_weight=0.0):
        tally, surface = Counter(), {}
        for k, s, wt in items:
            tally[k] += wt
            surface.setdefault(k, s)
        if absent_weight:
            tally[None] += absent_weight
        ranked = tally.most_common(2)
        k, w = ranked[0]
        second = ranked[1][1] if len(ranked) > 1 else 0.0
        clear = w >= MIN_FAMILIES - 1e-6 and w - second >= MARGIN - 1e-6
        return (surface.get(k, ()) if k is not None else ()), clear

    total = sum(weights)
    out = [[] for _ in prim_paras]
    unsure = 0
    for i in range(len(prim) + 1):
        if gaps[i]:
            span, clear = winner(gaps[i], total - sum(wt for _, _, wt in gaps[i]))
            unsure += not clear
            out[para_of[min(i, len(prim) - 1)]].extend(span)
        if i < len(prim):
            span, clear = winner(cands[i])
            unsure += not clear
            out[para_of[i]].extend(span)
    return [" ".join(p) for p in out if p], unsure


def _is_caps(w):
    letters = [c for c in w if c.isalpha()]
    return len(letters) >= 2 and all(c.isupper() for c in letters)


def layout(paras):
    """Restore structure the primary flattens (words are never changed): list items
    ("-Es braucht", or "- Ebenso" after a word without punctuation) start a new line,
    and a leading ALL-CAPS heading/caption of two or more words gets a line of its own."""
    out = []
    for para in paras:
        words = para.split()
        caps = 0
        while caps < len(words) and _is_caps(words[caps]):
            caps += 1
        if 2 <= caps < len(words):
            out.append(" ".join(words[:caps]))
            words = words[caps:]
        line = []
        for i, w in enumerate(words):
            item = (len(w) > 1 and w[0] == "-" and w[1].isupper()) or \
                   (w == "-" and i + 1 < len(words) and words[i + 1][:1].isupper()
                    and line and line[-1][-1:].isalpha())
            if item and line:
                out.append(" ".join(line))
                line = []
            line.append(w)
        if line:
            out.append(" ".join(line))
    return out


# ---------------------------------------------------------------- driver
def cache_path(cache_dir, engine, stem, page):
    return os.path.join(cache_dir, engine, f"{stem.replace('/', '_')}_p{page}.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("stems", nargs="*", help="limit to these stems (default: all reader screens)")
    ap.add_argument("--hd-dir", default="hd_assets")
    ap.add_argument("--out-dir", help="write the .txt files here instead of next to the images")
    ap.add_argument("--cache-dir", default="work/page_ocr", help="raw OCR readings (reused on reruns)")
    ap.add_argument("--force", action="store_true", help="overwrite existing (possibly hand-corrected) texts")
    args = ap.parse_args()
    out_dir = args.out_dir or args.hd_dir

    stems = []
    for stem in args.stems or [s for s in TEXT_SCREENS if s not in NO_READER]:
        dst = os.path.join(out_dir, stem + ".txt")
        if not os.path.exists(os.path.join(args.hd_dir, stem + ".webp")):
            print(f"  [skip] {stem}: HD image missing (run upscale_transformer.py --apply-text-screens)")
        elif os.path.exists(dst) and os.path.getsize(dst) > 0 and not args.force:
            print(f"  [keep] {dst} has text (use --force to redo)")
        else:
            stems.append(stem)
    if not stems:
        return

    # 1) read every page with every engine (one model in GPU memory at a time)
    for name, _, model_id, prompt, scale in ENGINES:
        todo = [(stem, i, img) for stem in stems for i, img in enumerate(page_crops(args.hd_dir, stem))
                if not os.path.exists(cache_path(args.cache_dir, name, stem, i))]
        if not todo:
            continue
        print(f"[ocr] {model_id}: {len(todo)} pages", flush=True)
        ocr = VisionOCR(model_id, prompt, scale)
        for stem, i, img in todo:
            p = cache_path(args.cache_dir, name, stem, i)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as f:
                f.write(ocr.read(img))
        ocr.close()

    # 2) vote per page; a screen gets text only if all of its pages are certain
    fams = Counter(fam for _, fam, *_ in ENGINES)
    weights = [1.0 / fams[fam] for _, fam, *_ in ENGINES]
    accepted = 0
    for stem in stems:
        pages, unsure = [], 0
        for i in range(len(PAGE_BOXES[stem])):
            readings = [paragraphs(open(cache_path(args.cache_dir, name, stem, i), encoding="utf-8").read())
                        for name, *_ in ENGINES]
            paras, u = vote_page(readings, weights)
            pages.append("\n".join(layout(paras)))
            unsure += u
        dst = os.path.join(out_dir, stem + ".txt")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8") as f:
            if unsure == 0:
                f.write("\n---\n".join(pages) + "\n")
        accepted += unsure == 0
        print(f"  [{'text' if unsure == 0 else 'zoom'}] {dst}"
              + ("" if unsure == 0 else f"  ({unsure} uncertain words -> zoom view only)"))
    print(f"[done] {accepted}/{len(stems)} screens with reliable text, the rest use the zoom view")


if __name__ == "__main__":
    main()
