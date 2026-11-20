# POWERGUARD  /  TEAM SCHEDULIX
# --------------------------------------------------------------------------
# analyze_manager_stalls.py
# Original software (c) 2026 Abdul Aleem. MIT License.
# Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.

"""Audit timestamped manager-stall confirmations, including their actual origin."""
import argparse
import json
from pathlib import Path


def analyze(path, minimum=30):
    rows = []
    errors = []
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        parts = line.split()
        if parts[0] != "PASS":
            errors.append(line)
            continue
        row = {k: int(v) for k, v in (part.split("=", 1) for part in parts[1:])}
        confirmation = row["supervisor_received_ns"] - row["detected_ns"]
        valid = (
            row["manager_stopped"] == 1
            and row["detected_ns"] == row["progress_ns"] + 50_000_000
            and row["stop_ns"] <= row["supervisor_received_ns"] < row["stop_ns"] + 1_000_000_000
            and row["detected_ns"] <= row["effective_ns"] <= row["supervisor_received_ns"]
            and confirmation == row["confirmation_ns"]
            and 0 <= confirmation <= 20_000_000
        )
        if not valid:
            errors.append(line)
        rows.append(row)
    confirmations = [r["confirmation_ns"] for r in rows]
    return {
        "source": str(path), "trials": len(rows), "minimum_required": minimum,
        "manager_deadline_ns": 50_000_000, "confirmation_limit_ns": 20_000_000,
        "worst_confirmation_ns": max(confirmations, default=None),
        "min_confirmation_ns": min(confirmations, default=None),
        "worst_effect_ns": max((r["effective_ns"] - r["detected_ns"] for r in rows), default=None),
        "worst_pause_to_confirmation_ns": max((r["supervisor_received_ns"] - r["stop_ns"] for r in rows), default=None),
        "unique_decisions": len({(r["epoch"], r["decision"]) for r in rows}),
        "errors": errors,
        "pass": not errors and len(rows) >= minimum and len({(r["epoch"], r["decision"]) for r in rows}) == len(rows),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("confirmations", type=Path)
    parser.add_argument("--minimum", type=int, default=30)
    args = parser.parse_args()
    result = analyze(args.confirmations, args.minimum)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["pass"] else 1)
