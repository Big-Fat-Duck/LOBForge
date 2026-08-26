from __future__ import annotations

import json
from pathlib import Path

import pytest

from lobforge_research.round4_evaluation import (
    artifact_logical_digest,
    equal_day_block_bootstrap,
    prefix_invariant,
    read_strict_ndjson,
    sensitivity_table,
    validate_artifact_directory,
)
from lobforge_research.round4_reporting import (
    analyze_round4_artifacts,
    write_round4_synthetic_report,
)
from lobforge_research.round4_synthetic import run_round4_synthetic_controls


def _write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def _artifact(path: Path) -> None:
    path.mkdir()
    summary = {
        "schema": "lobforge.mm_summary",
        "version": 1,
        "protocol_sha256": "a" * 64,
        "semantic_digest": "digest",
        "net_pnl_nanos": -5,
    }
    manifest = {
        "schema": "lobforge.mm_manifest",
        "version": 1,
        "protocol_sha256": "a" * 64,
        "semantic_digest": "digest",
        "order_rows": 1,
        "fill_rows": 1,
        "inventory_rows": 1,
    }
    _write_json(path / "summary.json", summary)
    _write_json(path / "manifest.json", manifest)
    rows = {
        "shadow_orders.ndjson": {
            "schema": "lobforge.shadow_order",
            "version": 1,
            "timestamp_ns": 1,
            "local_sequence": 1,
            "order_id": 1,
            "symbol": "SYNTH",
            "side": "buy",
            "price4": 100,
            "quantity": 1,
            "remaining_quantity": 1,
            "from_state": "submitted",
            "to_state": "pending_ack",
            "reason": "accepted",
            "queue_ahead_quantity": 0,
        },
        "shadow_fills.ndjson": {
            "schema": "lobforge.shadow_fill",
            "version": 1,
            "timestamp_ns": 2,
            "local_sequence": 2,
            "order_id": 1,
            "factual_sequence": 1,
            "symbol": "SYNTH",
            "side": "buy",
            "quantity": 1,
            "accounting_price4": 100,
            "factual_display_price4": 100,
            "factual_execution_price4": None,
            "anchor_mid2": 202,
            "match_number": 1,
            "reason": "factual_execution",
            "fee_nanos": 0,
            "rebate_nanos": 0,
        },
        "inventory_events.ndjson": {
            "schema": "lobforge.inventory_event",
            "version": 1,
            "timestamp_ns": 2,
            "local_sequence": 3,
            "order_id": 1,
            "inventory": 1,
            "trade_cash_nanos": -10_000_000,
            "fees_nanos": 0,
            "rebates_nanos": 0,
            "realized_gross_pnl_nanos": 0,
            "gross_equity_nanos": 0,
            "net_equity_nanos": 0,
            "conservative_liquidation_equity_nanos": None,
        },
    }
    for name, row in rows.items():
        _write_json(path / name, row)


def test_synthetic_controls_all_eight_and_honest_scope() -> None:
    result = run_round4_synthetic_controls()
    assert result["status"] == "PASS"
    assert len(result["scenarios"]) == 8
    assert all(row["status"] == "PASS" for row in result["scenarios"])
    assert result["P1"] == "BLOCKED: DATASET_NOT_PROVIDED"
    signal = result["scenarios"][-1]["metrics"]
    assert signal["planted_ic"] > 0.5
    assert abs(signal["null_ic"]) < 0.02
    assert abs(signal["shuffled_ic"]) < 0.02


def test_artifact_validation_analysis_and_report(tmp_path: Path) -> None:
    source = tmp_path / "source"
    _artifact(source)
    validated = validate_artifact_directory(source)
    assert validated["summary"]["net_pnl_nanos"] == -5
    assert all(len(value) == 64 for value in validated["logical_digests"].values())
    analysis = analyze_round4_artifacts(source, tmp_path / "analysis")
    assert analysis["semantic_digest"] == "digest"
    assert len(analysis["source_analysis_digest"]) == 64
    report = write_round4_synthetic_report(tmp_path / "controls")
    assert report["status"] == "PASS"
    assert (tmp_path / "controls/synthetic_controls.png").stat().st_size > 1_000
    with pytest.raises(FileExistsError):
        analyze_round4_artifacts(source, tmp_path / "analysis")
    with pytest.raises(FileExistsError):
        write_round4_synthetic_report(tmp_path / "controls")


def test_strict_ndjson_negative_cases(tmp_path: Path) -> None:
    path = tmp_path / "rows.ndjson"
    valid = {
        "schema": "lobforge.shadow_fill",
        "version": 1,
        "timestamp_ns": 1,
        "local_sequence": 1,
        "order_id": 1,
        "factual_sequence": 1,
        "symbol": "SYNTH",
        "side": "buy",
        "quantity": 1,
        "accounting_price4": 100,
        "factual_display_price4": 100,
        "factual_execution_price4": None,
        "anchor_mid2": 202,
        "match_number": 1,
        "reason": "factual_execution",
        "fee_nanos": 0,
        "rebate_nanos": 0,
    }
    _write_json(path, valid)
    assert read_strict_ndjson(path, "lobforge.shadow_fill") == [valid]
    with pytest.raises(ValueError, match="unsupported"):
        read_strict_ndjson(path, "unknown")
    path.write_text("{}", encoding="utf-8")
    with pytest.raises(ValueError, match="invalid NDJSON"):
        read_strict_ndjson(path, "lobforge.shadow_fill")
    path.write_text("not-json\n", encoding="utf-8")
    with pytest.raises(ValueError, match="invalid JSON"):
        read_strict_ndjson(path, "lobforge.shadow_fill")
    path.write_text("[]\n", encoding="utf-8")
    with pytest.raises(ValueError, match="non-object"):
        read_strict_ndjson(path, "lobforge.shadow_fill")
    _write_json(path, {**valid, "schema": "wrong"})
    with pytest.raises(ValueError, match="schema"):
        read_strict_ndjson(path, "lobforge.shadow_fill")
    _write_json(path, {**valid, "version": 2})
    with pytest.raises(ValueError, match="version"):
        read_strict_ndjson(path, "lobforge.shadow_fill")
    _write_json(path, {**valid, "quantity": -1})
    with pytest.raises(ValueError, match="quantity"):
        read_strict_ndjson(path, "lobforge.shadow_fill")
    _write_json(path, {key: value for key, value in valid.items() if key != "anchor_mid2"})
    with pytest.raises(ValueError, match="missing field"):
        read_strict_ndjson(path, "lobforge.shadow_fill")
    path.write_text(
        "\n".join(
            [
                json.dumps({**valid, "timestamp_ns": 2, "local_sequence": 2}),
                json.dumps({**valid, "timestamp_ns": 1, "local_sequence": 3}),
                "",
            ]
        ),
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="order regression"):
        read_strict_ndjson(path, "lobforge.shadow_fill")


def test_manifest_mismatch_rejected(tmp_path: Path) -> None:
    source = tmp_path / "source"
    _artifact(source)
    manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    for field, value, message in (
        ("schema", "wrong", "manifest"),
        ("protocol_sha256", "b" * 64, "protocol"),
        ("semantic_digest", "wrong", "semantic"),
    ):
        changed = {**manifest, field: value}
        _write_json(source / "manifest.json", changed)
        with pytest.raises(ValueError, match=message):
            validate_artifact_directory(source)
        _write_json(source / "manifest.json", manifest)
    summary = json.loads((source / "summary.json").read_text(encoding="utf-8"))
    _write_json(source / "summary.json", {**summary, "version": 2})
    with pytest.raises(ValueError, match="summary"):
        validate_artifact_directory(source)
    _write_json(source / "summary.json", summary)
    _write_json(source / "manifest.json", {**manifest, "fill_rows": 2})
    with pytest.raises(ValueError, match="fill_rows"):
        validate_artifact_directory(source)


def test_prefix_bootstrap_and_sensitivity() -> None:
    baseline = [
        {"timestamp_ns": 1, "quote": 100},
        {"timestamp_ns": 2, "quote": 101},
        {"timestamp_ns": 3, "quote": 102},
    ]
    perturbed = [*baseline[:2], {"timestamp_ns": 3, "quote": 999}]
    assert prefix_invariant(baseline, perturbed, cutoff_timestamp_ns=2)
    assert not prefix_invariant(baseline, perturbed, cutoff_timestamp_ns=3)
    assert artifact_logical_digest(baseline) == artifact_logical_digest(list(baseline))
    low, high = equal_day_block_bootstrap(
        {"2026-01-01": 1.0, "2026-01-02": 2.0, "2026-01-03": 3.0},
        seed=7,
        repetitions=500,
    )
    assert low <= 2.0 <= high
    with pytest.raises(ValueError):
        equal_day_block_bootstrap({}, seed=1, repetitions=1)
    rows = [
        {
            "latency_ns": 10,
            "queue_model": "primary",
            "fee_schedule": "low",
            "net_pnl_nanos": -1,
            "filled_quantity": 2,
        },
        {
            "latency_ns": 0,
            "queue_model": "primary",
            "fee_schedule": "low",
            "net_pnl_nanos": 1,
            "filled_quantity": 3,
        },
    ]
    table = sensitivity_table(rows)
    assert table[0]["latency_ns"] == 0
    with pytest.raises(ValueError, match="missing"):
        sensitivity_table([{"latency_ns": 1}])
