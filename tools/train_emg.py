from pathlib import Path
from collections import Counter
import csv
import numpy as np
from export_emg_c import export_emg_tree

from sklearn.tree import DecisionTreeClassifier
from sklearn.ensemble import RandomForestClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import make_pipeline
from sklearn.model_selection import StratifiedGroupKFold
from sklearn.metrics import (
    f1_score, accuracy_score,
    confusion_matrix, classification_report
)

ROOT = Path(__file__).resolve().parents[1]
LABELS = ("REST", "FLEX", "EXTEND")

# 100 Hz sampling, 400 ms windows, 100 ms hop
WINDOW = 40
HOP = 10


def load(path, allowed):
    features, labels, groups = [], [], []
    rows, previous = [], None
    broken = 0

    def finish():
        if not rows:
            return

        a = np.asarray(
            [(v[0], v[1]) for v in rows],
            dtype=np.float64
        )

        for i in range(0, len(a) - WINDOW + 1, HOP):
            w = a[i:i + WINDOW]

            mean = w.mean(axis=0)
            maximum = w.max(axis=0)
            variance = w.var(axis=0)
            span = np.ptp(w, axis=0)

            features.append([
                mean[0], maximum[0],
                variance[0], span[0],
                mean[1], maximum[1],
                variance[1], span[1]
            ])

            labels.append(rows[0][2])
            groups.append(rows[0][3])

    with path.open(newline="", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            label = r["label"].strip().upper()

            if label not in allowed:
                finish()
                rows, previous = [], None
                continue

            try:
                a = int(r["emg0_mv"])
                b = int(r["emg1_mv"])
                timestamp = int(r["timestamp_ms"])
                sequence = int(r["sequence"])

                if min(a, b) < 0 or max(a, b) > 3300:
                    raise ValueError("Invalid EMG voltage")

            except (ValueError, TypeError):
                finish()
                rows, previous = [], None
                continue

            key = (
                f"{r['session']}/{label}/"
                f"{r['repetition']}"
            )

            if previous is not None:
                old_key, old_t, old_seq = previous

                continuous = (
                    key == old_key
                    and 0 < timestamp - old_t <= 25
                    and sequence == old_seq + 1
                )

                if not continuous:
                    if key == old_key:
                        broken += 1

                    finish()
                    rows = []

            rows.append((a, b, label, key))
            previous = (key, timestamp, sequence)

    finish()

    return (
        np.asarray(features),
        np.asarray(labels),
        np.asarray(groups),
        broken
    )


def main():
    train_path = ROOT / "data" / "training.csv"
    valid_path = ROOT / "data" / "validation.csv"

    x, y, groups, gaps = load(
        train_path, set(LABELS)
    )

    vx, vy, _, vgaps = load(
        valid_path, set(LABELS)
    )

    print("Training windows:", len(y), dict(Counter(y)))
    print("Validation windows:", len(vy), dict(Counter(vy)))
    print("Continuity breaks:", gaps, vgaps)

    if set(y) != set(LABELS) or set(vy) != set(LABELS):
        raise RuntimeError(
            "All three classes must be present in both CSVs"
        )

    candidates = []

    for name, columns in [
        ("EMG0", [0, 1, 2, 3]),
        ("BOTH", list(range(8)))
    ]:
        for depth in (2, 3, 4, 5):
            candidates.append((
                f"TREE-{name}-depth{depth}",
                columns,
                DecisionTreeClassifier(
                    max_depth=depth,
                    min_samples_leaf=20,
                    random_state=42
                )
            ))

        candidates.append((
            f"FOREST-{name}",
            columns,
            RandomForestClassifier(
                n_estimators=60,
                max_depth=5,
                min_samples_leaf=12,
                random_state=42,
                n_jobs=-1
            )
        ))

        candidates.append((
            f"LOGISTIC-{name}",
            columns,
            make_pipeline(
                StandardScaler(),
                LogisticRegression(max_iter=1500)
            )
        ))

    # Keep all windows from a recording trial in
    # the same cross-validation fold.
    folds = list(
        StratifiedGroupKFold(
            n_splits=5,
            shuffle=True,
            random_state=42
        ).split(x, y, groups)
    )

    print("\nMODEL                       CV F1    VALID F1   VALID ACC")

    results = []

    for name, cols, model in candidates:
        fold_scores = []

        for tr, te in folds:
            model.fit(x[tr][:, cols], y[tr])
            pred = model.predict(x[te][:, cols])

            fold_scores.append(
                f1_score(
                    y[te], pred,
                    labels=LABELS,
                    average="macro",
                    zero_division=0
                )
            )

        model.fit(x[:, cols], y)
        pred = model.predict(vx[:, cols])

        cv = np.mean(fold_scores)
        vf1 = f1_score(
            vy, pred,
            labels=LABELS,
            average="macro",
            zero_division=0
        )
        va = accuracy_score(vy, pred)

        results.append((name, cols, model, cv, vf1))

        print(
            f"{name:27s} {cv:7.3f}"
            f"    {vf1:7.3f}     {va:7.3f}"
        )

    # Only decision trees qualify for simple C export.
    trees = [
        r for r in results
        if r[0].startswith("TREE-")
    ]

    winner = max(trees, key=lambda r: (r[4], r[3]))
    name, cols, model, cv, vf1 = winner

    print("\nProvisional best deployable tree:", name)
    print("Validation was used in selection, so this")
    print("score is not an unbiased final test estimate.")

    # Export selected decision tree to C for the wearable
    export_emg_tree(
        model=model,
        feature_names=[f"feature_{i}" for i in cols],
        output_dir=ROOT / "firmware/wearable/src",
    )

    pred = model.predict(vx[:, cols])

    print("\nClass order:", LABELS)
    print("Confusion matrix (actual rows, predicted columns):")
    print(confusion_matrix(vy, pred, labels=LABELS))

    print("\nPer-class performance:")
    print(
        classification_report(
            vy, pred,
            labels=LABELS,
            zero_division=0
        )
    )

    # Check how a 3-class model behaves during
    # swipes, neutral motion, and timed flex recordings.
    diagnostic_labels = {
        "NO_GESTURE",
        "FLEX_SHORT",
        "FLEX_MEDIUM",
        "FLEX_LONG",
        "SWIPE_UP",
        "SWIPE_DOWN",
        "SWIPE_LEFT",
        "SWIPE_RIGHT"
    }

    for path in (train_path, valid_path):
        xx, yy, _, _ = load(path, diagnostic_labels)

        if not len(yy):
            continue

        other_pred = model.predict(xx[:, cols])

        print("\nDiagnostic labels:", path.name)
        print("Fraction of windows classified as FLEX:")

        for label in sorted(set(yy)):
            mask = yy == label
            rate = np.mean(other_pred[mask] == "FLEX")

            print(
                f"  {label:15s} "
                f"{rate:.1%} ({mask.sum()} windows)"
            )


if __name__ == "__main__":
    main()
