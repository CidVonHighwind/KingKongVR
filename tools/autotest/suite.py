"""Regression checks for the render path, no headset needed.

    python tools/autotest/suite.py --name <result>                 correctness run (~1 min)
    python tools/autotest/suite.py --name <result> --only performance
    python tools/autotest/suite.py --check tests/results/<result>.json

Every run starts the game through game\PlayKingKong.bat (run.ps1) with
the window kept active for scripted input, the simulated headset
with head rotation and native per-eye rendering, and neutralises debug
switches from the user's kkvr.ini. Results go to tests/results/<result>.json
and are checked right away.

correctness  one game start (correctness.txt) with glitch_catch=1 and verify=1:
             frame captures in the language menu before/after F3 x3, main menu,
             loading/chapter select, gameplay x2 -> projection check (aligned,
             mean difference; the bigger-window capture checks rendering after
             a window resize); glitch hits; wrong vertex data and filter skips
performance  gameplay_measure.txt without checks: frame times after gameplay starts
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GAME = os.path.join(ROOT, "game")
sys.path.insert(0, HERE)
import check_projection  # noqa: E402

BASE_INI = [
    # keep_active=1: scripted input only reaches the game while its window is
    # active, and a window started from a tool is not in the foreground.
    "test.keep_active=1",
    "vr.simulate_headset=1",
    # execute_live: the live reference image glitch_catch compares with.
    "render.execute_live=1",
    "render.glitch_catch=0",
    "render.verify=0",
    "test.sim_head_rotation=15",
    # Debug switches and player settings a kkvr.ini may hold, at their
    # defaults (--ini overrides them: it comes after these).
    "test.flat=0",
    "test.share_eye_targets=0",
    "test.dump_passes=0",
    "test.output_grid=0",
    "render.render_scale=1.0",
    "display.preview_zoom=1",
    "display.fps_limit=120",
]


def game_running():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    return "kingkong9d.exe" in out


def captures():
    d = os.path.join(GAME, "captures")
    if not os.path.isdir(d):
        return set()
    return {n for n in os.listdir(d) if n.startswith("20")}


def run_game(script, ini, timeout):
    if game_running():
        raise SystemExit("the game is running: close it first")
    before = captures()
    cmd = ["powershell", "-File", os.path.join(HERE, "run.ps1"), "-Script",
           os.path.join(HERE, script), "-Timeout", str(timeout), "-Ini", ",".join(BASE_INI + ini)]
    started = time.time()
    subprocess.run(cmd, capture_output=True, text=True)
    log = open(os.path.join(GAME, "kkvr.log"), encoding="latin-1").read()
    new = sorted(captures() - before)
    return log, [os.path.join(GAME, "captures", n) for n in new], time.time() - started


def after_gameplay(log):
    """Log text after the autotest gameplay wait finished ('' if never)."""
    i = log.find("wait done")
    return log[i:] if i >= 0 else ""


def run_correctness():
    """One game start: captures (geometry, window size), glitch catcher, verify."""
    log, caps, secs = run_game("correctness.txt", ["render.glitch_catch=1", "render.verify=1"], 150)
    out = {"seconds": round(secs), "gameplay_reached": "wait done" in log}
    labels = ["language_menu", "language_menu_bigger_window", "main_menu", "loading_or_chapter",
              "gameplay_1", "gameplay_2"]
    out["captures"] = {}
    for label, cap in zip(labels, caps):
        r = check_projection.check(cap, align=True)
        r.pop("_warped", None)
        r.pop("_eye", None)
        out["captures"][label] = {"dir": os.path.basename(cap), **r}
    checked = re.findall(r"glitch: (\d+) frames checked", log)
    out["glitches"] = {"frames_checked": int(checked[-1]) if checked else 0,
                       "temporal_hits": log.count("glitch: temporal"),
                       "replay_hits": log.count("glitch: replay")}
    draws = wrong = 0
    for m in re.finditer(r"verify: (\d+) draws checked, (\d+) with wrong vertex data", log):
        draws += int(m.group(1))
        wrong += int(m.group(2))
    out["verify"] = {"draws_checked": draws, "wrong_vertex_draws": wrong,
                     "no_data": log.count("NO DATA")}
    out["crashed"] = "!!!" in log or "EXCEPTION" in log
    return out


def run_performance():
    log, _, secs = run_game("gameplay_measure.txt", [], 150)
    medians, p99s, worst, missed, game, submit = [], [], [], 0, [], []
    for m in re.finditer(r"frametime: \d+ frames: median ([\d.]+) ms, p99 ([\d.]+), worst ([\d.]+)"
                         r".*?headset frames missed (\d+).*?avg game ([\d.]+).*?submit ([\d.]+)",
                         after_gameplay(log)):
        medians.append(float(m.group(1)))
        p99s.append(float(m.group(2)))
        worst.append(float(m.group(3)))
        missed += int(m.group(4))
        game.append(float(m.group(5)))
        submit.append(float(m.group(6)))
    n = len(medians)
    return {"seconds": round(secs), "gameplay_reached": "wait done" in log, "windows": n,
            "median_ms": max(medians) if n else None, "p99_ms": max(p99s) if n else None,
            "worst_ms": max(worst) if n else None, "missed_headset_frames": missed,
            "avg_game_ms": sum(game) / n if n else None,
            "avg_submit_ms": sum(submit) / n if n else None}


RUNS = {"correctness": run_correctness, "performance": run_performance}


def check_result(result):
    """Pass/fail lines. Invariants: no baseline needed."""
    lines, ok = [], True

    def verdict(name, passed, detail):
        nonlocal ok
        ok &= passed
        lines.append(f"{'PASS' if passed else 'FAIL'}  {name}: {detail}")

    c = result.get("correctness")
    if c:
        verdict("run", c["gameplay_reached"] and not c["crashed"],
                f"gameplay reached {c['gameplay_reached']}, crashed {c['crashed']}")
        for label, cap in c.get("captures", {}).items():
            if "error" in cap or cap.get("mean") is None:
                verdict(f"geometry {label}", False, cap.get("error", "no result"))
                continue
            aligned = cap.get("best_shift") == [0, 0] and cap.get("best_scale") == 1.0
            verdict(f"geometry {label}", aligned and cap["mean"] < 4.0,
                    f"mean {cap['mean']:.2f}, shift {cap.get('best_shift')}, "
                    f"scale {cap.get('best_scale')}")
        g = c["glitches"]
        verdict("glitches", g["frames_checked"] > 0 and g["temporal_hits"] == 0
                and g["replay_hits"] == 0, json.dumps(g))
        v = c["verify"]
        verdict("verify", v["draws_checked"] > 0 and v["wrong_vertex_draws"] == 0
                and v["no_data"] == 0, json.dumps(v))
    p = result.get("performance")
    if p:
        verdict("performance", p["gameplay_reached"] and p["median_ms"] is not None
                and p["median_ms"] <= 8.4 and p["missed_headset_frames"] == 0, json.dumps(p))
    return ok, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name")
    ap.add_argument("--only", default="correctness")
    ap.add_argument("--check", metavar="RESULT")
    ap.add_argument("--ini", default="", help="extra section.key=value overrides, comma-separated")
    args = ap.parse_args()
    if args.ini:
        BASE_INI.extend(args.ini.split(","))
    if args.check:
        ok, lines = check_result(json.load(open(args.check)))
        print("\n".join(lines))
        print("SUITE PASSED" if ok else "SUITE FAILED")
        sys.exit(0 if ok else 1)
    if not args.name:
        ap.error("--name is required to run the suite")
    out_dir = os.path.join(ROOT, "tests", "results")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, args.name + ".json")
    result = json.load(open(path)) if os.path.exists(path) else {}
    for name in args.only.split(","):
        print(f"running {name} ...", flush=True)
        result[name] = RUNS[name]()
        json.dump(result, open(path, "w"), indent=2)
        print(json.dumps(result[name], indent=2), flush=True)
    print(f"written {path}")
    ok, lines = check_result(result)
    print("\n".join(lines))
    print("SUITE PASSED" if ok else "SUITE FAILED")


if __name__ == "__main__":
    main()
