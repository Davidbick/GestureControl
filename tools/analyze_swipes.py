"""Train/evaluate the simplified 80 ms directional swipe detector.

Uses training/validation recordings from collect_data.py. The IMU gate
remains a diagnostic sensor field and does not block detection.
The learned centers and thresholds match wearable/src/swipe.c.
"""
import argparse
from collections import defaultdict
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "firmware/wearable/src/swipe_config.h"
AXES = ("gx_mdps", "gy_mdps", "gz_mdps")
DIRECTIONS = ("UP", "DOWN", "LEFT", "RIGHT")

ONSET_MDPS = 60000.0
CAPTURE_MS = 80
MIN_CAPTURE_MDPS = 35000.0
MIN_SIMILARITY = 0.65
MIN_SCORE_MARGIN = 0.12
REARM_MDPS = 20000.0
REARM_HOLD_MS = 150
LOCKOUT_MS = 500
MAX_SAMPLE_GAP_MS = 40


def read_trials(path):
    data = pd.read_csv(path)
    required = {
        "session", "label", "repetition", "timestamp_ms",
        "sequence", *AXES,
    }
    missing = required.difference(data.columns)
    if missing:
        raise ValueError(f"{path}: missing {sorted(missing)}")

    trials = []
    for key, group in data.groupby(
        ["session", "label", "repetition"], sort=False
    ):
        group = group.drop_duplicates("sequence", keep="first")
        if len(group) < 40:
            continue
        labels = str(key[1]).upper()
        if labels not in {"NO_GESTURE", *("SWIPE_" + d for d in DIRECTIONS)}:
            continue
        times = group["timestamp_ms"].to_numpy(dtype=np.int64)
        seq = group["sequence"].to_numpy(dtype=np.int64)
        gyro = group[list(AXES)].to_numpy(dtype=np.float64)
        if not np.isfinite(gyro).all():
            raise ValueError(f"{path}: nonfinite gyro sample in {key}")
        trials.append((key, times, seq, gyro))
    return trials


def captures(trial):
    """Replay the smoothed-gyro onset, capture and neutral rearm state."""
    _, times, seq, samples = trial
    smooth = np.zeros(3)
    vector_sum = np.zeros(3)
    vector_count = 0
    collecting = False
    armed = True
    onset_time = 0
    last_capture_time = 0
    neutral_since = None
    last_time = None
    last_seq = None

    for timestamp, sequence, raw in zip(times, seq, samples):
        t = int(timestamp)
        n = int(sequence)
        if last_time is not None and (
            not (0 < t - last_time <= MAX_SAMPLE_GAP_MS) or
            n != last_seq + 1
        ):
            # Same reset rule as the wearable, treating sequence
            # discontinuities as a reset as well.
            smooth[:] = 0
            vector_sum[:] = 0
            collecting = False
            armed = True
            neutral_since = None
            vector_count = 0
        last_time, last_seq = t, n

        smooth += (raw - smooth) / 3.0
        energy = float(np.linalg.norm(smooth))

        if collecting:
            vector_sum += smooth
            vector_count += 1
            if t - onset_time < CAPTURE_MS:
                continue

            yield vector_sum / vector_count
            collecting = False
            armed = False
            last_capture_time = t
            neutral_since = None
            continue

        if not armed:
            if (t - last_capture_time >= LOCKOUT_MS and
                    energy < REARM_MDPS):
                if neutral_since is None:
                    neutral_since = t
                elif t - neutral_since >= REARM_HOLD_MS:
                    armed = True
                    neutral_since = None
            else:
                neutral_since = None

        if armed and energy >= ONSET_MDPS:
            collecting = True
            onset_time = t
            vector_sum = smooth.copy()
            vector_count = 1


def normalized(vector):
    length = np.linalg.norm(vector)
    return vector / length if length else np.zeros(3)


def learn_centers(trials):
    vectors = defaultdict(list)
    for trial in trials:
        label = trial[0][1].upper()
        if not label.startswith("SWIPE_"):
            continue

        direction = label.removeprefix("SWIPE_")
        events = list(captures(trial))
        if not events:
            continue

        candidate = events[0]
        if np.linalg.norm(candidate) >= MIN_CAPTURE_MDPS:
            vectors[direction].append(normalized(candidate))

    if any(not vectors[d] for d in DIRECTIONS):
        counts = {d: len(vectors[d]) for d in DIRECTIONS}
        raise RuntimeError(f"Missing valid training captures: {counts}")
    centers = np.asarray([
        normalized(np.median(vectors[d], axis=0))
        for d in DIRECTIONS
    ])
    return centers, {d: len(vectors[d]) for d in DIRECTIONS}


def classify(vector, centers):
    energy = np.linalg.norm(vector)
    if energy < MIN_CAPTURE_MDPS:
        return None
    scores = centers @ (vector / energy)
    order = np.argsort(scores)[::-1]
    best, runner = int(order[0]), int(order[1])
    if (scores[best] < MIN_SIMILARITY or
            scores[best] - scores[runner] < MIN_SCORE_MARGIN):
        return None
    return DIRECTIONS[best]


def evaluate(trials, centers):
    passed = 0
    totals = defaultdict(lambda: [0, 0])

    for trial in trials:
        key = trial[0]
        expected = key[1].replace("SWIPE_", "").upper()
        predictions = [
            detected for v in captures(trial)
            if (detected := classify(v, centers)) is not None
        ]
        success = (
            predictions == [expected] if expected != "NO_GESTURE"
            else predictions == []
        )
        passed += int(success)
        totals[expected][0] += int(success)
        totals[expected][1] += 1
        print(
            f"{'PASS' if success else 'FAIL'} {key}: "
            f"detected={predictions}"
        )
    print("Held-out accuracy:", f"{passed}/{len(trials)}")
    for direction, (correct, total) in totals.items():
        print(f"{direction}: {correct}/{total}")
    return passed


def export_centers(centers):
    lines = [
        "#ifndef SWIPE_CONFIG_H",
        "#define SWIPE_CONFIG_H",
        "",
        "// Direction centers from 80ms gyro captures; no gate.",
        "static const float swipe_centers[4][3] = {",
    ]
    for direction, center in zip(DIRECTIONS, centers):
        xyz = ", ".join(f"{v:.8f}f" for v in center)
        lines.append(f"    {{{xyz}}}, // {direction}")
    lines += ["};", "", "#endif", ""]
    OUTPUT.write_text("\n".join(lines), encoding="utf-8")
    print("Exported:", OUTPUT)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--train", default="data/swipe_phase4_train.csv"
    )
    parser.add_argument(
        "--validation", default="data/swipe_phase4_validation.csv"
    )
    parser.add_argument("--no-export", action="store_true")
    args = parser.parse_args()

    def resolve(filename):
        file = Path(filename)
        return file if file.is_absolute() else ROOT / file

    train = read_trials(resolve(args.train))
    valid = read_trials(resolve(args.validation))
    if not train or not valid:
        raise RuntimeError("Need training and validation trials")

    centers, counts = learn_centers(train)
    print("Training captures:", counts)
    print("Direction centers:\n", centers)
    evaluate(valid, centers)
    if args.no_export:
        print("No firmware files modified.")
    else:
        export_centers(centers)


if __name__ == "__main__":
    main()
