import argparse
import csv
import time
import uuid

from datetime import datetime, timezone
from pathlib import Path

import serial


# Must match the firmware's MC1 output format.
SENSOR_FIELDS = [
    "sequence",
    "timestamp_ms",
    "emg0_raw",
    "emg1_raw",
    "emg0_mv",
    "emg1_mv",
    "ax_mg",
    "ay_mg",
    "az_mg",
    "gx_mdps",
    "gy_mdps",
    "gz_mdps",
    "dropped_total",
    "errors_total",
]

CSV_FIELDS = [
    "session",
    "label",
    "repetition",
    "host_timestamp",
    *SENSOR_FIELDS,
]


def parse_sensor_line(raw):
    text = raw.decode(
        "utf-8", errors="replace"
    ).strip()

    # Ignore ordinary Zephyr log messages.
    if "MC1," not in text:
        return None

    text = text[text.index("MC1,"):]

    parts = text.split(",")

    if len(parts) != len(SENSOR_FIELDS) + 1:
        return None

    try:
        values = [int(value) for value in parts[1:]]
    except ValueError:
        return None

    return dict(zip(SENSOR_FIELDS, values))



def countdown(seconds, connection):
    print(f"\nStarting in {seconds} seconds...")

    start = time.monotonic()
    last_remaining = seconds + 1

    while True:
        elapsed = time.monotonic() - start
        remaining = max(0, seconds - int(elapsed))

        if remaining != last_remaining and remaining > 0:
            print(f"{remaining}...")
            last_remaining = remaining

        if elapsed >= seconds:
            break

        # Consume old samples during preparation.
        connection.readline()

    # Discard any partial or remaining buffered data.
    connection.reset_input_buffer()


def record_trial(
    connection,
    writer,
    session,
    label,
    repetition,
    duration,
):

    print(f"\nGO! {label} — repetition {repetition}")

    start = time.monotonic()
    deadline = start + duration

    count = 0
    missing = 0
    first = None
    last = None
    previous_sequence = None

    while time.monotonic() < deadline:
        raw = connection.readline()

        if not raw:
            continue

        sample = parse_sensor_line(raw)

        if sample is None:
            continue

        sequence = sample["sequence"]

        if previous_sequence is not None:
            expected = previous_sequence + 1

            if sequence > expected:
                missing += sequence - expected

                if sequence - expected > 10:
                    print(
                        f"WARNING: Large sequence gap "
                        f"({sequence - expected} samples). "
                        "Consider repeating this trial."
                    )
        previous_sequence = sequence

        if first is None:
            first = sample

        last = sample

        writer.writerow({
            "session": session,
            "label": label,
            "repetition": repetition,
            "host_timestamp": datetime.now(
                timezone.utc
            ).isoformat(),
            **sample,
        })

        count += 1

    print("STOP")

    if first is None or last is None:
        print("No sensor samples received.")
        return

    elapsed_ms = (
        last["timestamp_ms"] -
        first["timestamp_ms"]
    ) & 0xFFFFFFFF

    rate = 0.0

    if count > 1 and elapsed_ms > 0:
        rate = (count - 1) * 1000 / elapsed_ms

    dropped = (
        last["dropped_total"] -
        first["dropped_total"]
    )

    errors = (
        last["errors_total"] -
        first["errors_total"]
    )

    print(
        f"Samples: {count} | "
        f"Rate: {rate:.1f} Hz | "
        f"Sequence gaps: {missing}"
    )

    print(
        f"Queue drops: {dropped} | "
        f"Sensor errors: {errors}"
    )

    if (
        missing > 0
        or dropped > 0
        or errors > 0
        or rate < 90
        or rate > 110
    ):
        print("WARNING: Review this recording.")


def main():
    parser = argparse.ArgumentParser(
        description="GestureControl EMG + IMU recorder"
    )

    parser.add_argument(
        "--port",
        required=True,
        help="Serial port, e.g. COM7"
    )

    parser.add_argument(
        "--label",
        required=True,
        help="Gesture label, e.g. FLEX"
    )

    parser.add_argument(
        "--reps",
        type=int,
        default=10
    )

    parser.add_argument(
        "--duration",
        type=float,
        default=2.0,
        help="Recording seconds per repetition"
    )

    parser.add_argument(
        "--rest",
        type=float,
        default=2.0,
        help="Rest seconds between repetitions"
    )

    parser.add_argument(
        "--output",
        default="data/sensor_trials.csv"
    )

    args = parser.parse_args()

    if (
        args.reps < 1
        or args.duration <= 0
        or args.rest < 0
    ):
        parser.error("Invalid duration or repetition count")

    label = args.label.strip().upper().replace(" ", "_")
    session = uuid.uuid4().hex[:12]

    output = Path(args.output)
    output.parent.mkdir(
        parents=True, exist_ok=True
    )

    needs_header = (
        not output.exists()
        or output.stat().st_size == 0
    )

    print(f"\nGestureControl recording session: {session}")
    print(f"Label: {label}")
    print(f"Repetitions: {args.reps}")
    print(f"Duration: {args.duration} seconds")
    print(f"Output: {output}")

    with (
        serial.Serial(
            args.port,
            baudrate=115200,
            timeout=0.2,
        ) as connection,
        output.open(
            "a", newline="", encoding="utf-8"
        ) as file,
    ):
        connection.dtr = True

        writer = csv.DictWriter(
            file,
            fieldnames=CSV_FIELDS
        )

        if needs_header:
            writer.writeheader()

        # Let the CDC connection settle.
        time.sleep(0.5)

        try:
            for repetition in range(1, args.reps + 1):
                print(
                    f"\nPrepare: {label} "
                    f"({repetition}/{args.reps})"
                )

                countdown(3, connection)

                record_trial(
                    connection,
                    writer,
                    session,
                    label,
                    repetition,
                    args.duration,
                )

                file.flush()

                if repetition < args.reps:
                    time.sleep(args.rest)

        except KeyboardInterrupt:
            print("\nRecording interrupted.")

    print(f"\nSaved to {output}")


if __name__ == "__main__":
    main()
