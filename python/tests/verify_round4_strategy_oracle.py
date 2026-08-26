from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from pathlib import Path

from lobforge_research.round4_oracle import strategy_quote_scalar


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("--rows", type=int, default=10_000)
    arguments = parser.parse_args()
    completed = subprocess.run(
        [str(arguments.executable), "--rows", str(arguments.rows)],
        check=True,
        capture_output=True,
        text=True,
    )
    observed = 0
    for row in csv.DictReader(completed.stdout.splitlines()):
        observed += 1
        expected = strategy_quote_scalar(
            kind=row["kind"],  # type: ignore[arg-type]
            timestamp_ns=int(row["timestamp_ns"]),
            session_end_ns=int(row["session_end_ns"]),
            inventory=int(row["inventory"]),
            bids=list(range(10_000, 9_899, -1)),
            asks=list(range(10_002, 10_103)),
            tick_size_price4=1,
            maximum_distance_ticks=100,
            symmetric_half_spread_price4=1.0 + (int(row["index"]) % 5) * 0.1,
            gamma=float.fromhex(row["gamma"]),
            sigma_squared=float.fromhex(row["sigma_squared"]),
            arrival_intensity_k=float.fromhex(row["k"]),
            causal_signal=float.fromhex(row["signal"]),
            signal_coefficient_price4=float.fromhex(row["signal_coefficient"]),
        )
        actual_bid = None if int(row["bid"]) < 0 else int(row["bid"])
        actual_ask = None if int(row["ask"]) < 0 else int(row["ask"])
        if expected.bid_price4 != actual_bid or expected.ask_price4 != actual_ask:
            raise AssertionError(f"quote mismatch at vector {row['index']}")
        reservation = float.fromhex(row["reservation"])
        spread = float.fromhex(row["total_spread"])
        if expected.reservation_price4 is None or expected.total_spread_price4 is None:
            raise AssertionError(f"unexpected null formula at vector {row['index']}")
        if abs(expected.reservation_price4 - reservation) > 1e-12:
            raise AssertionError(f"reservation mismatch at vector {row['index']}")
        if abs(expected.total_spread_price4 - spread) > 1e-12:
            raise AssertionError(f"spread mismatch at vector {row['index']}")
    if observed != arguments.rows:
        raise AssertionError(f"expected {arguments.rows} vectors, observed {observed}")
    print(f"PASS strategy_oracle_vectors rows={observed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
