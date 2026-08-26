from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run_once(
    program: Path, config: Path, output: Path, event_chunk: int = 65_536
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(program),
            "--synthetic-fixture",
            "primary",
            "--config",
            str(config),
            "--output-dir",
            str(output),
            "--event-chunk",
            str(event_chunk),
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    if len(sys.argv) != 3:
        return 2
    program, config = map(Path, sys.argv[1:])
    version = subprocess.run(
        [str(program), "--version"], capture_output=True, text=True
    )
    assert version.returncode == 0 and version.stdout == "lobforge_mm_sim_v1\n"
    usage = subprocess.run([str(program)], capture_output=True, text=True)
    assert (
        usage.returncode == 2
        and usage.stdout == ""
        and usage.stderr.startswith("usage:")
    )
    with tempfile.TemporaryDirectory(prefix="lobforge-mm-cli-") as temporary:
        root = Path(temporary)
        runs = [
            run_once(program, config, root / f"run-{index}", 1 if index % 2 == 0 else 1_024)
            for index in range(10)
        ]
        assert all(run.returncode == 0 and run.stderr == "" for run in runs)
        assert len({run.stdout for run in runs}) == 1
        summaries = [
            json.loads((root / f"run-{index}/summary.json").read_text(encoding="utf-8"))
            for index in range(10)
        ]
        assert len({summary["semantic_digest"] for summary in summaries}) == 1
        assert all(
            summary["protocol_sha256"] == summaries[0]["protocol_sha256"]
            for summary in summaries
        )
        assert summaries[0]["fill_events"] == 1
        expected_metrics = {
            "factual_events": 17,
            "order_event_rows": 15,
            "inventory_event_rows": 1,
            "submitted_orders": 2,
            "submitted_quantity": 20,
            "acknowledged_orders": 2,
            "rejected_orders": 0,
            "cancelled_orders": 2,
            "replaced_orders": 3,
            "filled_orders": 0,
            "filled_quantity": 5,
            "turnover_quantity": 5,
            "final_inventory": 5,
            "maximum_absolute_inventory": 5,
            "trade_cash_nanos": -5_000_000_000,
            "fees_nanos": 0,
            "rebates_nanos": 1_000_000,
            "gross_pnl_nanos": 125_000_000,
            "net_pnl_nanos": 126_000_000,
            "conservative_liquidation_pnl_nanos": 49_500_000,
            "realized_gross_pnl_nanos": 0,
            "unrealized_gross_pnl_nanos": 125_000_000,
            "spread_capture_nanos": 50_000_000,
            "stop_switch_triggers": 0,
        }
        assert all(summaries[0][key] == value for key, value in expected_metrics.items())
        assert summaries[0]["markouts"] == [
            {
                "horizon_ns": 10_000_000,
                "eligible_fills": 1,
                "eligible_quantity": 5,
                "missing_fills": 0,
                "directional_value_nanos": 75_000_000,
                "adverse_selection_cost_nanos": -75_000_000,
                "realized_spread_nanos_per_share": 30_000_000.0,
            },
            {
                "horizon_ns": 100_000_000,
                "eligible_fills": 1,
                "eligible_quantity": 5,
                "missing_fills": 0,
                "directional_value_nanos": 100_000_000,
                "adverse_selection_cost_nanos": -100_000_000,
                "realized_spread_nanos_per_share": 40_000_000.0,
            },
            {
                "horizon_ns": 1_000_000_000,
                "eligible_fills": 1,
                "eligible_quantity": 5,
                "missing_fills": 0,
                "directional_value_nanos": 125_000_000,
                "adverse_selection_cost_nanos": -125_000_000,
                "realized_spread_nanos_per_share": 50_000_000.0,
            },
        ]
        for index in range(10):
            directory = root / f"run-{index}"
            manifest = json.loads(
                (directory / "manifest.json").read_text(encoding="utf-8")
            )
            assert manifest["schema"] == "lobforge.mm_manifest"
            assert manifest["real_data_evidence"] == "BLOCKED: DATASET_NOT_PROVIDED"
            assert manifest["order_rows"] == 15
            assert manifest["fill_rows"] == 1
            assert manifest["inventory_rows"] == 1
            for filename in (
                "protocol.json",
                "shadow_orders.ndjson",
                "shadow_fills.ndjson",
                "inventory_events.ndjson",
                "summary.json",
                "metrics.json",
            ):
                assert (directory / filename).is_file()
        repeated = run_once(program, config, root / "run-0")
        assert repeated.returncode == 3
        assert repeated.stdout == ""
        assert "already exists" in repeated.stderr
    print(f"PASS mm_cli digest={summaries[0]['semantic_digest']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
