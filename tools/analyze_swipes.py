# tools/analyze_swipes.py

import argparse
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import pandas as pd

DIRECTIONS = ("UP", "DOWN", "LEFT", "RIGHT")
AXES = ("gx_mdps", "gy_mdps", "gz_mdps")

CAPTURE_MS = 80
NEUTRAL_MS = 300
LOCKOUT_MS = 1200


def get_direction(label):
    label = str(label).strip().upper()
    for direction in DIRECTIONS:
        if label == direction or label.endswith("_" + direction):
            return direction
    return None


def get_trials(df):
    required = {
        "session", "label", "repetition",
        "timestamp_ms", *AXES
    }
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing columns: {sorted(missing)}")

    for key, trial in df.groupby(
        ["session", "label", "repetition"], sort=False
    ):
        trial = trial.sort_values("timestamp_ms")
        trial = trial.drop_duplicates("timestamp_ms")
        if len(trial) < 30:
            continue

        t = trial.timestamp_ms.to_numpy(dtype=np.int64)
        if np.any(np.diff(t) > 40):
            print("Skipping trial with timing gap:", key)
            continue

        raw = trial[list(AXES)].to_numpy(dtype=float)

        # Match the firmware's causal smoothing.
        filtered = np.zeros_like(raw)
        state = np.zeros(3)
        for i, sample in enumerate(raw):
            state += (sample - state) / 3.0
            filtered[i] = state

        yield key, t, filtered


def normalized(v):
    length = np.linalg.norm(v)
    return v / length if length > 0 else np.zeros(3)


def learn_signatures(trials, threshold):
    samples = defaultdict(list)

    for key, t, gyro in trials:
        label = get_direction(key[1])
        if label is None:
            continue

        magnitude = np.linalg.norm(gyro, axis=1)

        # Exclude the first 120 ms of each recording.
        indices = np.flatnonzero(
            (t >= t[0] + 120) & (magnitude >= threshold)
        )
        if len(indices) == 0:
            continue

        start = indices[0]
        end = np.searchsorted(t, t[start] + CAPTURE_MS)

        if end - start < 3:
            continue

        vector = normalized(gyro[start:end].mean(axis=0))
        samples[label].append(vector)

    if any(not samples[label] for label in DIRECTIONS):
        return None

    centers = np.array([
        normalized(np.median(samples[label], axis=0))
        for label in DIRECTIONS
    ])

    return centers


def replay(t, gyro, centers, threshold, similarity):
    """Replicate the firmware state machine on a recorded trial."""

    events = []
    armed = True
    collecting = False
    neutral_since = None
    captured_at = None
    emitted_at = None
    vectors = []
    magnitude = np.linalg.norm(gyro, axis=1)

    for i, now in enumerate(t):
        now = int(now)

        if collecting:
            vectors.append(gyro[i])

            if now - captured_at < CAPTURE_MS:
                continue

            vector = normalized(np.mean(vectors, axis=0))
            scores = centers @ vector
            best = int(np.argmax(scores))

            if scores[best] >= similarity:
                events.append(DIRECTIONS[best])

            # Lock out return motion, even on rejected attempts.
            collecting = False
            armed = False
            emitted_at = now
            neutral_since = None
            continue

        if armed:
            if now >= int(t[0]) + 120 and magnitude[i] >= threshold:
                collecting = True
                captured_at = now
                vectors = [gyro[i]]
            continue

        # Wait for stable neutral and minimum lockout time.
        if magnitude[i] < threshold * 0.45:
            if neutral_since is None:
                neutral_since = now

            if (
                now - neutral_since >= NEUTRAL_MS
                and now - emitted_at >= LOCKOUT_MS
            ):
                armed = True
                neutral_since = None
        else:
            neutral_since = None

    return events


def evaluate(trials, centers, threshold, similarity, verbose=False):
    results = []

    for key, t, gyro in trials:
        expected = get_direction(key[1])
        detected = replay(t, gyro, centers, threshold, similarity)

        if expected is None:
            correct = len(detected) == 0
        else:
            correct = detected == [expected]

        results.append((key, expected or "NEGATIVE", detected, correct))

        if verbose:
            status = "PASS" if correct else "FAIL"
            print(
                f"{status} {key} "
                f"expected={expected or 'NONE'} detected={detected}"
            )

    return results


def accuracy(results):
    if not results:
        return 0.0
    return sum(row[3] for row in results) / len(results)


def export_config(path, centers, threshold, similarity):
    lines = [
        "#ifndef SWIPE_CONFIG_H",
        "#define SWIPE_CONFIG_H",
        "",
        "// Generated from training.csv by analyze_swipes.py",
        f"#define SWIPE_THRESHOLD_MDPS {threshold:.2f}f",
        f"#define SWIPE_SIMILARITY {similarity:.5f}f",
        "",
        "static const float swipe_centers[4][3] = {",
    ]

    for label, center in zip(DIRECTIONS, centers):
        vector = ", ".join(f"{x:.8f}f" for x in center)
        lines.append(f"    {{{vector}}}, // {label}")

    lines.extend(["};", "", "#endif"])

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--train", default="data/training.csv")
    parser.add_argument("--validation", default="data/validation.csv")
    parser.add_argument(
        "--output",
        default="firmware/wearable/src/swipe_config.h"
    )
    args = parser.parse_args()

    training = pd.read_csv(args.train)
    validation = pd.read_csv(args.validation)

    print("Training labels:", Counter(training.label))
    print("Validation labels:", Counter(validation.label))

    train_trials = list(get_trials(training))
    valid_trials = list(get_trials(validation))

    best = None

    for threshold in (
        30000, 45000, 60000, 80000,
        100000, 130000, 180000
    ):
        centers = learn_signatures(train_trials, threshold)
        if centers is None:
            continue

        for similarity in (0.55, 0.65, 0.75, 0.82):
            results = evaluate(
                train_trials, centers, threshold, similarity
            )

            score = accuracy(results)
            candidate = (score, similarity, threshold)

            if best is None or candidate > best[0]:
                best = (candidate, centers)

    if best is None:
        raise RuntimeError(
            "Could not learn all four directions. "
            "Check labels and gyro values."
        )

    (train_score, similarity, threshold), centers = best

    print("\nSELECTED MODEL")
    print("Training accuracy:", f"{train_score:.1%}")
    print("Threshold:", threshold, "mdps")
    print("Similarity:", similarity)

    for direction, center in zip(DIRECTIONS, centers):
        print(direction, np.round(center, 4))

    print("\nHELD-OUT VALIDATION")
    results = evaluate(
        valid_trials, centers, threshold, similarity, verbose=True
    )

    summary = defaultdict(lambda: [0, 0])
    for _, label, _, passed in results:
        summary[label][1] += 1
        summary[label][0] += int(passed)

    print("\nRESULTS")
    for label, (passed, total) in sorted(summary.items()):
        print(f"{label}: {passed}/{total}")

    print(f"TOTAL ACCURACY: {accuracy(results):.1%}")

    export_config(
        Path(args.output), centers, threshold, similarity
    )
    print("\nGenerated:", args.output)


if __name__ == "__main__":
    main()
