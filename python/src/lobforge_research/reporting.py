"""Deterministic synthetic evidence artifacts and scope-safe reporting."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import matplotlib
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

matplotlib.use("Agg")
from matplotlib import pyplot as plt

from .canonical import LogicalDigest, canonical_typed_bytes
from .evaluation import (
    classification_metrics,
    decile_response,
    regression_metrics,
    reliability_table,
)
from .microprice import StoikovMicroprice
from .models import TrainOnlyLogisticClassifier, TrainOnlyRegressor
from .protocol_evaluation import evaluate_interpretable_suite
from .splits import chronological_date_split
from .synthetic import generate_planted_signal, run_synthetic_controls


def _json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )


def _plot_deciles(deciles: list[dict[str, float | int]], path: Path) -> None:
    figure, axis = plt.subplots(figsize=(7.0, 4.0), constrained_layout=True)
    axis.plot(
        [int(row["bin"]) for row in deciles],
        [float(row["mean_target"]) for row in deciles],
        marker="o",
    )
    axis.axhline(0.0, color="black", linewidth=0.8)
    axis.set(
        xlabel="OFI decile",
        ylabel="Mean future mid2 delta",
        title="Synthetic positive control",
    )
    figure.savefig(path, dpi=120, metadata={"Software": "LOBForge Round 3"})
    plt.close(figure)


def _plot_reliability(table: dict[str, Any], path: Path) -> None:
    rows = table["table"]
    figure, axis = plt.subplots(figsize=(5.0, 5.0), constrained_layout=True)
    axis.plot([0, 1], [0, 1], linestyle="--", color="grey")
    axis.scatter(
        [float(row["mean_confidence"]) for row in rows],
        [float(row["empirical_accuracy"]) for row in rows],
    )
    axis.set(
        xlim=(0, 1),
        ylim=(0, 1),
        xlabel="Mean predicted confidence",
        ylabel="Empirical accuracy",
        title="Synthetic calibration",
    )
    figure.savefig(path, dpi=120, metadata={"Software": "LOBForge Round 3"})
    plt.close(figure)


def write_synthetic_report(
    output: Path,
    *,
    seed: int = 20260825,
    rows_per_symbol_day: int = 500,
    protocol_path: Path | None = None,
) -> dict[str, Any]:
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    source_protocol = Path(__file__).resolve().parents[3] / "configs" / "round3_protocol.toml"
    protocol = (
        (
            source_protocol
            if source_protocol.is_file()
            else Path(__file__).with_name("round3_protocol.toml")
        )
        if protocol_path is None
        else protocol_path
    )
    protocol_sha256 = hashlib.sha256(protocol.read_bytes()).hexdigest()
    controls = run_synthetic_controls(seed=seed, rows_per_symbol_day=rows_per_symbol_day)
    rows = generate_planted_signal(seed=seed, rows_per_symbol_day=rows_per_symbol_day)
    split = chronological_date_split(rows)
    feature = "ofi_l1_100ms"
    regression_target = "target_mid2_delta_clock_100ms"
    direction_target = "target_direction_clock_100ms"
    regressor = TrainOnlyRegressor([feature]).fit(split.train, regression_target)
    regression_prediction, regression_indices = regressor.predict(split.test, regression_target)
    classifier = TrainOnlyLogisticClassifier([feature], random_seed=seed).fit(
        split.train, direction_target
    )
    probabilities_before, classification_indices = classifier.predict_proba(
        split.test, direction_target, calibrated=False
    )
    probabilities_after, after_indices = classifier.predict_proba(
        split.test, direction_target, calibrated=True
    )
    if not np.array_equal(classification_indices, after_indices):
        raise RuntimeError("calibration changed evaluation row alignment")
    direction_truth = np.asarray(
        [split.test[int(index)][direction_target] for index in classification_indices],
        dtype=np.int8,
    )
    regression_truth = np.asarray(
        [split.test[int(index)][regression_target] for index in regression_indices],
        dtype=np.float64,
    )
    class_prediction = np.asarray([-1, 0, 1], dtype=np.int8)[np.argmax(probabilities_after, axis=1)]
    calibration_before = reliability_table(direction_truth, probabilities_before)
    calibration_after = reliability_table(direction_truth, probabilities_after)
    deciles = decile_response(
        [row[feature] for row in split.test],
        [row[regression_target] for row in split.test],
    )
    microprice = StoikovMicroprice(minimum_state_samples=5).fit(split.train)
    microprice.save(output / "stoikov_microprice.json")
    model_suite, suite_parameters = evaluate_interpretable_suite(
        split.train, split.validation, split.test, seed=seed
    )
    model_parameters = {
        "regression": regressor.parameters(),
        "classification": classifier.parameters(),
        "stoikov": microprice.to_dict(),
        "interpretable_suite": suite_parameters,
    }
    _json(output / "model_parameters.json", model_parameters)

    prediction_rows: list[dict[str, Any]] = []
    prediction_digest = LogicalDigest()
    for position, index in enumerate(classification_indices):
        source = split.test[int(index)]
        row = {
            "session_date": source["session_date"],
            "symbol": source["symbol"],
            "anchor_sequence": source["anchor_sequence"],
            "truth": int(direction_truth[position]),
            "prediction": int(class_prediction[position]),
            "probability_down": float(probabilities_after[position, 0]),
            "probability_zero": float(probabilities_after[position, 1]),
            "probability_up": float(probabilities_after[position, 2]),
        }
        prediction_rows.append(row)
        prediction_digest.update(row)
    prediction_schema = pa.schema(
        [
            pa.field("session_date", pa.string(), False),
            pa.field("symbol", pa.string(), False),
            pa.field("anchor_sequence", pa.uint64(), False),
            pa.field("truth", pa.int8(), False),
            pa.field("prediction", pa.int8(), False),
            pa.field("probability_down", pa.float64(), False),
            pa.field("probability_zero", pa.float64(), False),
            pa.field("probability_up", pa.float64(), False),
        ]
    )
    pq.write_table(
        pa.Table.from_pylist(prediction_rows, schema=prediction_schema),
        output / "predictions.parquet",
        compression="zstd",
        use_dictionary=False,
    )
    metrics: dict[str, Any] = {
        "metrics_schema": "lobforge.round3_synthetic_metrics",
        "version": 1,
        "seed": seed,
        "protocol_sha256": protocol_sha256,
        "split": {
            "train_dates": split.train_dates,
            "validation_dates": split.validation_dates,
            "test_dates": split.test_dates,
            "purged_rows": split.purged_rows,
        },
        "regression": regression_metrics(regression_truth, regression_prediction),
        "classification": classification_metrics(
            direction_truth, class_prediction, probabilities_after
        ),
        "calibration": {"before": calibration_before, "after": calibration_after},
        "decile_response": deciles,
        "controls": controls,
        "model_suite": model_suite,
        "prediction_logical_sha256": prediction_digest.hexdigest(),
        "model_logical_sha256": hashlib.sha256(canonical_typed_bytes(model_parameters)).hexdigest(),
        "real_data": {
            "D1": "BLOCKED: DATASET_NOT_PROVIDED",
            "D2": "BLOCKED",
            "D3": "BLOCKED",
            "H1": "BLOCKED",
        },
        "scope": "engineering controls only; not evidence of market alpha or profitability",
    }
    _json(output / "metrics.json", metrics)
    _plot_deciles(deciles, output / "decile_response.png")
    _plot_reliability(calibration_after, output / "reliability.png")
    report = """# LOBForge Round 3 synthetic validation

This report is generated from deterministic synthetic controls only. It validates the
research implementation and rejection mechanism; it is not evidence of a real-market
signal, causality, execution quality, or profitability.

## Evidence status

- D1: BLOCKED: DATASET_NOT_PROVIDED
- D2: BLOCKED
- D3: BLOCKED
- H1: BLOCKED

## Artifacts

- `metrics.json`: regression, three-class, calibration, decile and control metrics
- `model_parameters.json`: train-only fitted parameters and diagnostics
- `predictions.parquet`: final synthetic test predictions
- `decile_response.png` and `reliability.png`: deterministic figures
"""
    (output / "synthetic_report.md").write_text(report, encoding="utf-8", newline="\n")
    manifest = {
        "artifact_schema": "lobforge.synthetic_report",
        "version": 1,
        "seed": seed,
        "protocol_sha256": protocol_sha256,
        "rows_per_symbol_day": rows_per_symbol_day,
        "prediction_logical_sha256": prediction_digest.hexdigest(),
        "model_logical_sha256": metrics["model_logical_sha256"],
        "metrics_logical_sha256": hashlib.sha256(canonical_typed_bytes(metrics)).hexdigest(),
        "report_sha256": hashlib.sha256(report.encode("utf-8")).hexdigest(),
    }
    _json(output / "manifest.json", manifest)
    return manifest
