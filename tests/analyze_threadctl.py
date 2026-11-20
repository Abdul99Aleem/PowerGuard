"""Validate captured critical jobs and report distributions without hiding gaps."""
import argparse
import csv
import json
import math
from pathlib import Path


def distribution(values):
    values = sorted(values)
    return {
        "n": len(values),
        **{key: values[max(0, math.ceil(len(values) * q) - 1)]
           for key, q in (("p50_ns", .5), ("p95_ns", .95), ("p99_ns", .99), ("max_ns", 1))},
    }


def analyze(path):
    with path.open(newline="") as stream:
        rows = [{key: int(value) for key, value in row.items()} for row in csv.DictReader(stream)]
    if not rows:
        raise ValueError("No critical jobs captured")
    gaps = []
    bad_timestamps = []
    missed = []
    for index, row in enumerate(rows):
        if index and row["id"] != rows[index - 1]["id"] + 1:
            gaps.append([rows[index - 1]["id"], row["id"]])
        if not (row["release_ns"] <= row["start_ns"] <= row["finish_ns"]
                and row["deadline_ns"] - row["release_ns"] == 5_000_000):
            bad_timestamps.append(row["id"])
        if row["finish_ns"] > row["deadline_ns"]:
            missed.append(row["id"])
    period_errors = sum(b["release_ns"] - a["release_ns"] != 10_000_000
                        for a, b in zip(rows, rows[1:]))
    return {
        "source": str(path), "first_id": rows[0]["id"], "last_id": rows[-1]["id"],
        "jobs": len(rows), "gaps": gaps, "invalid_timestamps": bad_timestamps,
        "period_errors": period_errors, "deadline_misses": len(missed), "missed_ids": missed,
        "response": distribution([r["finish_ns"] - r["release_ns"] for r in rows]),
        "release_lateness": distribution([r["start_ns"] - r["release_ns"] for r in rows]),
        "execution_wall_time": distribution([r["finish_ns"] - r["start_ns"] for r in rows]),
        "pass": not (gaps or bad_timestamps or missed or period_errors),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jobs", type=Path)
    args = parser.parse_args()
    result = analyze(args.jobs)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["pass"] else 1)
