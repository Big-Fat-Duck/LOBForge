from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

from lobforge_research.benchmark import benchmark_ndjson_pipeline, benchmark_vectorized_features
from lobforge_research.canonical import (
    LogicalDigest,
    canonical_float_policy,
    canonical_typed_bytes,
    canonicalize_floats,
)
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


def test_artifact_float_canonicalization_is_versioned_and_strict():
    assert canonical_float_policy() == {
        "name": "lobforge.significant_decimal/v1",
        "significant_digits": 8,
        "negative_zero": "normalized_to_positive_zero",
        "non_finite": "rejected",
        "probability_sum_absolute_tolerance": "3e-8",
    }
    normalized = canonicalize_floats(
        {"negative_zero": -0.0, "roundoff": [0.24024342147056982, 1.234567849]}
    )
    assert normalized == {"negative_zero": 0.0, "roundoff": [0.24024342, 1.2345678]}
    assert normalized["negative_zero"].hex() == "0x0.0p+0"
    assert canonicalize_floats(0.24024342147056982) == canonicalize_floats(0.24024342147345645)
    for non_finite in (float("nan"), float("inf"), float("-inf")):
        try:
            canonicalize_floats({"value": non_finite})
        except ValueError:
            pass
        else:
            raise AssertionError("non-finite artifact float accepted")


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
        "9de71628d6f011dd26b40d05b7aedef9a4287315991adf7e2088f0ee434b1130"
    )
    assert first["model_logical_sha256"] == (
        "ead97eb95d627d1d73294c21f8d6adf5e54f0038ac473fa137c65c925ab34a30"
    )
    assert first["prediction_logical_sha256"] == (
        "12fd53a41b4c6364636dc303d4c5d739a4ef08aa62a2f13851344b2ec992fc40"
    )
    assert first["protocol_sha256"] == (
        "0315c2c762380ceb5ac30816f5112455fe6aec2f87b089634df78c379eecb2e4"
    )
    metrics = json.loads((tmp_path / "first" / "metrics.json").read_text(encoding="utf-8"))
    model_parameters = json.loads(
        (tmp_path / "first" / "model_parameters.json").read_text(encoding="utf-8")
    )
    stoikov_parameters = json.loads(
        (tmp_path / "first" / "stoikov_microprice.json").read_text(encoding="utf-8")
    )
    assert metrics["canonical_float_policy"] == canonical_float_policy()
    assert model_parameters["canonical_float_policy"] == canonical_float_policy()
    assert stoikov_parameters["canonical_float_policy"] == canonical_float_policy()
    assert canonicalize_floats(metrics) == metrics
    assert canonicalize_floats(model_parameters) == model_parameters
    assert canonicalize_floats(stoikov_parameters) == stoikov_parameters
    assert model_parameters["stoikov"] == stoikov_parameters
    assert metrics["real_data"] == {
        "D1": "BLOCKED: DATASET_NOT_PROVIDED",
        "D2": "BLOCKED",
        "D3": "BLOCKED",
        "H1": "BLOCKED",
    }
    predictions = pq.read_table(tmp_path / "first" / "predictions.parquet")
    assert predictions.num_rows > 0
    assert predictions.schema.metadata == {
        b"lobforge.canonical_float_policy": b"lobforge.significant_decimal/v1",
        b"lobforge.canonical_float_significant_digits": b"8",
        b"lobforge.probability_sum_absolute_tolerance": b"3e-8",
    }
    prediction_rows = predictions.to_pylist()
    assert canonicalize_floats(prediction_rows) == prediction_rows
    probability = np.column_stack(
        [
            predictions["probability_down"].to_numpy(),
            predictions["probability_zero"].to_numpy(),
            predictions["probability_up"].to_numpy(),
        ]
    )
    probability_tolerance = float(canonical_float_policy()["probability_sum_absolute_tolerance"])
    assert np.all(np.isfinite(probability))
    assert np.all(probability >= 0.0)
    assert np.all(np.abs(np.sum(probability, axis=1) - 1.0) <= probability_tolerance)
    expected_prediction = np.asarray([-1, 0, 1], dtype=np.int8)[np.argmax(probability, axis=1)]
    assert np.array_equal(expected_prediction, predictions["prediction"].to_numpy())
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
