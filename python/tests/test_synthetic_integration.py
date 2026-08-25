from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pyarrow.parquet as pq

from lobforge_research.benchmark import benchmark_ndjson_pipeline, benchmark_vectorized_features
from lobforge_research.canonical import LogicalDigest, canonical_typed_bytes
from lobforge_research.reporting import write_synthetic_report
from lobforge_research.synthetic import (
    feature_logical_digest,
    generate_null_control,
    run_synthetic_controls,
    shuffled_labels,
)


def test_canonical_typed_digest_is_order_independent_for_mapping_only():
    left = canonical_typed_bytes({"b": 1, "a": [True, None, 1.5]})
    right = canonical_typed_bytes({"a": [True, None, 1.5], "b": 1})
    assert left == right
    digest = LogicalDigest()
    digest.update({"x": 1})
    digest.update({"x": 2})
    assert digest.rows == 2 and len(digest.hexdigest()) == 64
    try:
        canonical_typed_bytes(float("nan"))
    except ValueError:
        pass
    else:
        raise AssertionError("non-finite float accepted")
    try:
        canonical_typed_bytes(b"bytes")
    except TypeError:
        pass
    else:
        raise AssertionError("unsupported type accepted")


def test_packaged_protocol_matches_versioned_repository_protocol():
    packaged = Path(__file__).parents[1] / "src/lobforge_research/round3_protocol.toml"
    repository = Path(__file__).parents[2] / "configs/round3_protocol.toml"
    assert packaged.read_bytes() == repository.read_bytes()


def test_positive_null_and_shuffle_controls_meet_registered_oracles():
    controls = run_synthetic_controls(rows_per_symbol_day=300)
    assert all(controls["checks"].values()), controls
    assert controls["real_market_signal_status"] == "BLOCKED"
    null = generate_null_control(days=3, symbols=2, rows_per_symbol_day=100)
    shuffled = shuffled_labels(null)
    assert feature_logical_digest(null) == feature_logical_digest(shuffled)
    assert any(
        left["target_mid2_delta_clock_100ms"] != right["target_mid2_delta_clock_100ms"]
        for left, right in zip(null, shuffled, strict=True)
    )


def test_synthetic_report_artifacts_are_logically_reproducible(tmp_path):
    first = write_synthetic_report(tmp_path / "first", rows_per_symbol_day=80)
    second = write_synthetic_report(tmp_path / "second", rows_per_symbol_day=80)
    assert first == second
    assert first["metrics_logical_sha256"] == (
        "83fa5ad778d39c52618d2345d9561b84057d2808c1ef12bf5ad5f98434f2d3a5"
    )
    assert first["model_logical_sha256"] == (
        "8cbeecc29c15dfed32456531d9ab33a6b5e54a93e9d6a8e6252de37865de33c6"
    )
    assert first["prediction_logical_sha256"] == (
        "6bff7b3bceb0693185c8e9d4053e53959688d26800f30d3d481499f97335ea04"
    )
    assert first["protocol_sha256"] == (
        "0315c2c762380ceb5ac30816f5112455fe6aec2f87b089634df78c379eecb2e4"
    )
    metrics = json.loads((tmp_path / "first" / "metrics.json").read_text(encoding="utf-8"))
    assert metrics["real_data"] == {
        "D1": "BLOCKED: DATASET_NOT_PROVIDED",
        "D2": "BLOCKED",
        "D3": "BLOCKED",
        "H1": "BLOCKED",
    }
    assert pq.read_table(tmp_path / "first" / "predictions.parquet").num_rows > 0
    suite = metrics["model_suite"]
    assert set(suite["regression"]) == {
        "zero_change_baseline",
        "lagged_ofi_univariate",
        "imbalance_only",
        "weighted_mid_displacement",
        "stoikov_microprice_displacement",
        "regularized_imbalance_plus_ofi",
    }
    assert suite["final_test_evaluations"] == 1
    assert suite["primary_whole_symbol_day_block_bootstrap"]["replicates"] == 1000
    assert (
        suite["per_symbol_day_metrics"]["regression"]["lagged_ofi_univariate"]["symbol_days"] == 15
    )
    assert (tmp_path / "first" / "decile_response.png").stat().st_size > 1000
    assert (
        len(hashlib.sha256((tmp_path / "first" / "synthetic_report.md").read_bytes()).hexdigest())
        == 64
    )


def test_vectorized_benchmark_smoke():
    result = benchmark_vectorized_features(rows=100_000, runs=3)
    assert result["rows_per_second"] > 1_000_000
    assert result["passes_threshold"] is True
    pipeline = benchmark_ndjson_pipeline(rows=100_000, runs=3)
    assert pipeline["rows_per_second"] > 100_000
    assert pipeline["passes_throughput_threshold"] is True
    assert pipeline["passes_rss_threshold"] is True
