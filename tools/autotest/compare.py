"""Compare screenshots pixel by pixel.

    python tools/autotest/compare.py A.png B.png [--out diff.png]

Prints the mean absolute difference (0-255 per channel), the largest
difference and the share of pixels whose summed RGB difference exceeds 6.
With --out, writes an amplified difference image (x8) for looking at where
two renders differ.
"""
import argparse

from PIL import Image, ImageChops, ImageStat


def compare(a_path, b_path, out=None):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        return f"size differs: {a.size} vs {b.size}"
    diff = ImageChops.difference(a, b)
    mean = sum(ImageStat.Stat(diff).mean) / 3.0
    extrema = max(hi for _, hi in diff.getextrema())
    summed = [r + g + bl for r, g, bl in diff.getdata()]
    share = 100.0 * sum(1 for v in summed if v > 6) / len(summed)
    if out:
        diff.point(lambda v: min(255, v * 8)).save(out)
    return f"mean {mean:.3f}, max {extrema}, {share:.3f}% pixels differ"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--out")
    args = parser.parse_args()
    print(compare(args.a, args.b, args.out))
