"""Train and evaluate the deployed Phase 4 swipe state machine.

Input must be gate-aware MC1 recordings from collect_data.py.
Record at least 350 ms in the calibrated neutral pose at the start of
EACH trial so the detector can arm, then perform one deliberate gesture.
Older CSVs lack gate_valid and cannot be faithfully replayed.
"""
import argparse
import re
from collections import defaultdict
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware/wearable/src"
DIRECTIONS = ("UP", "DOWN", "LEFT", "RIGHT")
AXES = ("gx_mdps", "gy_mdps", "gz_mdps")
FIXED_NAMES = (
    "SWIPE_PREMOTION_MDPS",
    "SWIPE_NEUTRAL_MDPS",
    "SWIPE_MIN_CAPTURE_MDPS",
    "SWIPE_CAPTURE_MS",
    "SWIPE_GATE_GRACE_MS",
    "SWIPE_NEUTRAL_HOLD_MS",
    "SWIPE_LOCKOUT_MS",
    "SWIPE_SAMPLE_GAP_MS",
)


def direction_of(label):
    label = str(label).strip().upper()
    for direction in DIRECTIONS:
        if label == direction or label.endswith("_" + direction):
            return direction
    return None


def normalized(v):
    length = np.linalg.norm(v)
    return v / length if length > 0 else np.zeros(3)


def read_settings(path):
    source = path.read_text(encoding="utf-8")
    result = {}
    for name in FIXED_NAMES:
        match = re.search(
            rf"(?m)^#define\s+{name}\s+([0-9.]+)",
            source,
        )
        if not match:
            raise ValueError(f"Missing firmware define {name}")
        result[name] = float(match.group(1))
    return result


def read_trials(path):
    data = pd.read_csv(path)
    required = {
        "session", "label", "repetition",
        "sequence", "timestamp_ms", "gate_valid", *AXES,
    }
    missing = required - set(data.columns)
    if missing:
        raise ValueError(
            f"{path}: missing fields {sorted(missing)}. "
            "Use a new gate-aware MC1 recording; older swipe CSVs "
            "cannot reconstruct orientation gating."
        )

    trials = []
    grouped = data.groupby(
        ["session", "label", "repetition"], sort=False
    )
    for key, trial in grouped:
        # Preserve acquisition order (and sequence continuity); do not
        # reorder by timestamps when a device clock wraps.
        trial = trial.drop_duplicates("sequence", keep="first")
        if len(trial) < 40:
            continue
        t = trial["timestamp_ms"].to_numpy(dtype=np.int64)
        seq = trial["sequence"].to_numpy(dtype=np.int64)
        gyro = trial[list(AXES)].to_numpy(dtype=np.float64)
        raw_gate = trial["gate_valid"].to_numpy()

        if not np.all(np.isin(raw_gate, (0, 1))):
            raise ValueError(f"Invalid gate_valid in trial {key}")
        if not np.isfinite(gyro).all():
            raise ValueError(f"Invalid gyro readings in trial {key}")
        trials.append((key, t, seq, gyro, raw_gate.astype(bool)))
    return trials


class Phase4Detector:
    """Python equivalent of wearable/src/swipe.c motion state machine.

    A completed 80 ms gesture capture produces the raw 3-axis mean.
    The classifier is intentionally separate so centers/thresholds can
    be optimized without changing the detector's rearming behavior.
    """

    def __init__(self, settings, onset):
        self.s = settings
        self.onset = float(onset)
        self.reset()

    def reset(self):
        self.smooth = np.zeros(3)
        self.vector_sum = np.zeros(3)
        self.vector_count = 0
        self.last_t = None
        self.last_gate_t = None
        self.onset_t = 0
        self.attempt_t = 0
        self.neutral_t = None
        self.armed = False
        self.collecting = False
        self.ever_attempted = False
        self.previous_above = False
        self.weak_candidate = False

    def update(self, now, sample, in_gate):
        s = self.s

        if self.last_t is not None:
            dt = now - self.last_t
            if dt == 0:
                return None
            if dt < 0 or dt > s["SWIPE_SAMPLE_GAP_MS"]:
                self.reset()
        self.last_t = now

        if in_gate:
            self.last_gate_t = now

        # C: smooth += (raw - smooth) / 3
        self.smooth += (sample - self.smooth) / 3.0
        energy = float(np.linalg.norm(self.smooth))

        if self.collecting:
            self.vector_sum += self.smooth
            self.vector_count += 1
            if now - self.onset_t < s["SWIPE_CAPTURE_MS"]:
                return None

            mean = self.vector_sum / self.vector_count
            self.collecting = False
            self.armed = False
            self.ever_attempted = True
            self.attempt_t = now
            self.neutral_t = None
            return mean.copy()

        if not self.armed:
            cooled = (not self.ever_attempted or
                      now - self.attempt_t >= s["SWIPE_LOCKOUT_MS"])
            if in_gate and energy < s["SWIPE_NEUTRAL_MDPS"]:
                if self.neutral_t is None:
                    self.neutral_t = now
                elif (cooled and now - self.neutral_t >=
                      s["SWIPE_NEUTRAL_HOLD_MS"]):
                    self.armed = True
                    self.neutral_t = None
                    self.weak_candidate = False
            else:
                self.neutral_t = None
            self.previous_above = energy >= self.onset
            return None

        gate_fresh = (in_gate or
                      (self.last_gate_t is not None and
                       now - self.last_gate_t <=
                       s["SWIPE_GATE_GRACE_MS"]))
        above = energy >= self.onset

        if gate_fresh and energy >= s["SWIPE_PREMOTION_MDPS"]:
            self.weak_candidate = True
        if (not above and energy < s["SWIPE_NEUTRAL_MDPS"]
                and self.weak_candidate):
            self.weak_candidate = False

        if above and gate_fresh:
            self.collecting = True
            self.onset_t = now
            self.vector_count = 1
            self.vector_sum = self.smooth.copy()
            self.weak_candidate = False
        elif above and not gate_fresh:
            # Returning to the valid gate must not replay a rejected
            # movement that began entirely outside it.
            self.armed = False
            self.neutral_t = None
            self.weak_candidate = False

        self.previous_above = above
        return None


def captures(trial, settings, onset):
    _, t, seq, gyro, gate = trial
    detector = Phase4Detector(settings, onset)
    previous_seq = None
    for timestamp, sequence, raw, valid in zip(t, seq, gyro, gate):
        timestamp = int(timestamp)
        sequence = int(sequence)
        if previous_seq is not None and sequence != previous_seq + 1:
            detector.reset()
        previous_seq = sequence
        vector = detector.update(timestamp, raw, bool(valid))
        if vector is not None:
            yield vector


def learn_centers(trials, settings, onset):
    examples = defaultdict(list)
    for trial in trials:
        expected = direction_of(trial[0][1])
        if expected is None:
            continue
        strokes = list(captures(trial, settings, onset))
        if not strokes:
            continue
        candidate = strokes[0]
        if (np.linalg.norm(candidate) >=
                settings["SWIPE_MIN_CAPTURE_MDPS"]):
            examples[expected].append(normalized(candidate))

    if any(not examples[d] for d in DIRECTIONS):
        return None
    return np.asarray([
        normalized(np.median(examples[d], axis=0))
        for d in DIRECTIONS
    ])


def recognize(mean, centers, settings, similarity, margin):
    energy = float(np.linalg.norm(mean))
    if energy < settings["SWIPE_MIN_CAPTURE_MDPS"]:
        return None
    scores = centers @ (mean / energy)
    order = np.argsort(scores)[::-1]
    best, runner_up = int(order[0]), int(order[1])
    if scores[best] < similarity or (
        scores[best] - scores[runner_up] < margin
    ):
        return None
    return DIRECTIONS[best]


def evaluate(trials, centers, settings, onset, similarity, margin,
             verbose=False):
    results = []
    for trial in trials:
        key = trial[0]
        expected = direction_of(key[1])
        events = [
            label
            for mean in captures(trial, settings, onset)
            if (label := recognize(
                mean, centers, settings, similarity, margin
            )) is not None
        ]
        passed = (
            events == [expected] if expected is not None
            else len(events) == 0
        )
        results.append((key, expected, events, passed))
        if verbose:
            print(
                f"{'PASS' if passed else 'FAIL'} {key}: "
                f"expected={expected or 'NONE'} detected={events}"
            )
    return results


def accuracy(results):
    if not results:
        return 0.0
    return sum(row[3] for row in results) / len(results)


def export_centers(path, centers):
    lines = [
        "#ifndef SWIPE_CONFIG_H",
        "#define SWIPE_CONFIG_H",
        "",
        "// Direction vectors learned for the Phase 4 swipe detector.",
        "static const float swipe_centers[4][3] = {",
    ]
    for label, center in zip(DIRECTIONS, centers):
        values = ", ".join(f"{x:.8f}f" for x in center)
        lines.append(f"    {{{values}}}, // {label}")
    lines += ["};", "", "#endif", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def export_tuning(path, onset, similarity, margin):
    source = path.read_text(encoding="utf-8")
    replacements = {
        "SWIPE_ONSET_MDPS": f"{onset:.1f}f",
        "SWIPE_MIN_SIMILARITY": f"{similarity:.5f}f",
        "SWIPE_MIN_SCORE_MARGIN": f"{margin:.5f}f",
    }
    for name, new_value in replacements.items():
        source, count = re.subn(
            rf"(?m)^(#define\s+{name}\s+)[^\r\n]+",
            lambda m: m.group(1) + new_value,
            source,
        )
        if count != 1:
            raise ValueError(f"Missing or duplicated {name} in {path}")
    path.write_text(source, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description="Tune/replay the actual Phase 4 gate-aware swipe detector"
    )
    parser.add_argument(
        "--train", default="data/swipe_phase4_train.csv"
    )
    parser.add_argument(
        "--validation", default="data/swipe_phase4_validation.csv"
    )
    parser.add_argument("--no-export", action="store_true")
    args = parser.parse_args()

    settings = read_settings(FIRMWARE / "swipe_tuning.h")
    train = read_trials(ROOT / args.train)
    valid = read_trials(ROOT / args.validation)
    if not train or not valid:
        raise RuntimeError("Need labeled training and validation trials")

    best = None
    for onset in (45000, 60000, 80000, 100000):
        centers = learn_centers(train, settings, onset)
        if centers is None:
            continue
        for similarity in (0.55, 0.65, 0.75):
            for margin in (0.08, 0.12, 0.18):
                result = evaluate(
                    train, centers, settings, onset, similarity, margin
                )
                score = accuracy(result)
                rank = (score, similarity, margin)
                if best is None or rank > best[0]:
                    best = (rank, centers, onset, similarity, margin)

    if best is None:
        raise RuntimeError(
            "Cannot learn all directions. Record a stable in-gate "
            "neutral period followed by one swipe per trial."
        )

    rank, centers, onset, similarity, margin = best
    print("\nSelected settings:",
          f"onset={onset} similarity={similarity} margin={margin}")
    print("Training accuracy:", f"{rank[0]:.1%}")
    print("\nHELD-OUT VALIDATION")
    results = evaluate(
        valid, centers, settings, onset, similarity, margin, verbose=True
    )
    summary = defaultdict(lambda: [0, 0])
    for _, expected, _, passed in results:
        row = summary[expected or "NEGATIVE"]
        row[0] += int(passed)
        row[1] += 1
    for label, (passed, total) in sorted(summary.items()):
        print(f"{label}: {passed}/{total}")
    print("Total held-out accuracy:", f"{accuracy(results):.1%}")

    if args.no_export:
        print("No firmware files modified.")
        return

    export_centers(FIRMWARE / "swipe_config.h", centers)
    export_tuning(
        FIRMWARE / "swipe_tuning.h", onset, similarity, margin
    )
    print("Exported swipe_config.h and swipe_tuning.h")


if __name__ == "__main__":
    main()
