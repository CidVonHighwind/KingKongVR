"""Check native per-eye rendering (the eye images) against a reference
image of the window alone (off-axis), for one frame capture.

    python tools/autotest/check_projection.py <capture dir> [--out compare.png] [--align]

The capture holds eye_L.png (the left eye rendered with the headset's view
and field of view), window_reference_L.png (the same frame's left eye rendered
as exactly the window rectangle, off-axis, same 1:1 depth model) and, in
frame.txt, the window outline in eye_L as texture coordinates. Warping
window_reference_L onto that outline must reproduce eye_L inside the window if
the projection math is right.
Prints the mean difference inside the window (0-255) and the share of pixels
that differ clearly; --out writes eye_L | warped reference | difference x4.
--align also searches small shifts and scales of the outline: the best one
must be shift 0,0 and scale 1.00 (tools/autotest/suite.py uses check()).
"""
import argparse
import os
import re

import numpy as np
from PIL import Image, ImageChops, ImageDraw


def perspective_coeffs(src, dst):
    """Coefficients for Image.transform(PERSPECTIVE): output (dst) -> input (src)."""
    rows, rhs = [], []
    for (x, y), (u, v) in zip(dst, src):
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        rhs += [u, v]
    return np.linalg.solve(np.array(rows, float), np.array(rhs, float)).tolist()


def outline(capture):
    text = open(os.path.join(capture, "frame.txt"), encoding="latin-1").read()
    match = re.search(r"projection outline L \(uv[^)]*\):((?: [-\d.]+,[-\d.]+)+)", text)
    if not match:
        return None
    return [tuple(map(float, p.split(","))) for p in match.group(1).split()]


def _difference(eye, reference, dst):
    """Mean difference and clear-difference share inside the window polygon."""
    rw, rh = reference.size
    src = [(0, 0), (rw, 0), (rw, rh), (0, rh)]
    warped = reference.transform(eye.size, Image.PERSPECTIVE, perspective_coeffs(src, dst),
                            Image.BILINEAR)
    mask = Image.new("L", eye.size, 0)
    cx = sum(x for x, _ in dst) / 4
    cy = sum(y for _, y in dst) / 4
    inner = [(cx + (x - cx) * 0.98, cy + (y - cy) * 0.98) for x, y in dst]
    ImageDraw.Draw(mask).polygon(inner, fill=255)
    m = np.array(mask) > 0
    diff = np.abs(np.array(eye, int) - np.array(warped, int))
    diff = diff.sum(axis=2) if diff.ndim == 3 else diff * 3
    if not m.any():
        return None, None, warped
    return diff[m].mean() / 3.0, (diff[m] > 60).mean() * 100.0, warped


def check(capture, align=False):
    """Result dict: corners, mean, clear_pct and (align) best_shift, best_scale."""
    uv = outline(capture)
    if uv is None:
        return {"error": "no projection outline (capture not in projection mode?)"}
    if len(uv) != 4:
        return {"error": f"outline has {len(uv)} points (window clipped by the near plane)"}
    eye = Image.open(os.path.join(capture, "eye_L.png")).convert("RGB")
    reference = Image.open(os.path.join(capture, "window_reference_L.png")).convert("RGB")
    ew, eh = eye.size
    dst = [(u * ew, v * eh) for u, v in uv]
    if float(np.array(eye.convert("L")).std()) < 2.0:
        return {"error": "eye image is blank (capture during a black screen?)"}
    mean, clear, warped = _difference(eye, reference, dst)
    result = {"corners": [(round(x), round(y)) for x, y in dst], "mean": mean,
              "clear_pct": clear, "_warped": warped, "_eye": eye}
    if align and mean is not None:
        # Half resolution, grey: shifts of -2/0/+2 px (full resolution) and
        # scales 0.995/1/1.005 around the window centre.
        small_eye = eye.convert("L").resize((ew // 2, eh // 2))
        small_reference = reference.convert("L").resize(
            (reference.size[0] // 2, reference.size[1] // 2))
        cx = sum(x for x, _ in dst) / 4
        cy = sum(y for _, y in dst) / 4
        best = None
        for scale in (0.995, 1.0, 1.005):
            for dx in (-2, 0, 2):
                for dy in (-2, 0, 2):
                    d = [((cx + (x - cx) * scale + dx) / 2, (cy + (y - cy) * scale + dy) / 2)
                         for x, y in dst]
                    value, _, _ = _difference(small_eye, small_reference, d)
                    if value is not None and (best is None or value < best[0]):
                        best = (value, (dx, dy), scale)
        if best:
            result["best_shift"] = list(best[1])
            result["best_scale"] = best[2]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture")
    parser.add_argument("--out")
    parser.add_argument("--align", action="store_true")
    args = parser.parse_args()
    r = check(args.capture, args.align)
    if "error" in r:
        raise SystemExit(r["error"])
    print(f"window corners in eye_L (px): {r['corners']}")
    print(f"inside the window: mean difference {r['mean']:.2f}, "
          f"{r['clear_pct']:.2f}% of pixels differ clearly")
    if "best_shift" in r:
        print(f"best alignment: shift {r['best_shift']} px, scale {r['best_scale']:.3f}")
    if args.out:
        eye, warped = r["_eye"], r["_warped"]
        ew, eh = eye.size
        d = ImageChops.difference(eye, warped).point(lambda v: min(255, v * 4))
        sheet = Image.new("RGB", (ew * 3 // 2, eh // 2))
        for i, im in enumerate([eye, warped, d]):
            sheet.paste(im.resize((ew // 2, eh // 2)), (i * ew // 2, 0))
        sheet.save(args.out)


if __name__ == "__main__":
    main()
