from __future__ import annotations

import numpy as np
import pytest

from lobforge_research.evaluation import (
    benjamini_hochberg,
    block_bootstrap_mean,
    classification_metrics,
    decile_response,
    multiclass_brier,
    regression_metrics,
    reliability_table,
    symbol_day_spearman,
)


def test_regression_metrics_keep_negative_results_and_zero_returns():
    metrics = regression_metrics([0, 1, -1, 0], [2, -1, 1, 2])
    assert metrics["oos_r2_vs_zero"] < 0
    assert metrics["zero_return_prevalence"] == 0.5
    assert metrics["spearman"] < 0
    constant = regression_metrics([0, 0], [0, 0])
    assert constant["oos_r2_vs_zero"] is None
    assert constant["pearson"] is None
    with pytest.raises(ValueError):
        regression_metrics([np.nan], [1])
    with pytest.raises(ValueError):
        regression_metrics([1], [1, 2])


def test_three_class_metrics_and_calibration_tables():
    truth = np.asarray([-1, 0, 1, -1, 0, 1])
    probabilities = np.asarray(
        [
            [0.8, 0.1, 0.1],
            [0.1, 0.8, 0.1],
            [0.1, 0.1, 0.8],
            [0.7, 0.2, 0.1],
            [0.2, 0.7, 0.1],
            [0.1, 0.2, 0.7],
        ]
    )
    prediction = np.asarray([-1, 0, 1, -1, 0, 1])
    metrics = classification_metrics(truth, prediction, probabilities)
    assert metrics["balanced_accuracy"] == 1.0
    assert metrics["macro_f1"] == 1.0
    assert metrics["confusion_matrix"] == [[2, 0, 0], [0, 2, 0], [0, 0, 2]]
    assert multiclass_brier(truth, probabilities) < 0.2
    reliability = reliability_table(truth, probabilities, bins=3)
    assert len(reliability["table"]) == 3
    assert reliability["ece_equal_frequency_10"] >= 0
    with pytest.raises(ValueError):
        classification_metrics(truth, prediction, probabilities[:, :2])
    with pytest.raises(ValueError):
        classification_metrics(truth, prediction, probabilities * 2)
    with pytest.raises(ValueError):
        multiclass_brier(truth, probabilities[:, :2])
    with pytest.raises(ValueError):
        reliability_table(truth, probabilities, bins=1)


def test_symbol_day_equal_weight_bootstrap_bh_and_deciles():
    rows = []
    for symbol, count, sign in (("HEAVY", 100, 1), ("LIGHT", 4, -1)):
        for index in range(count):
            rows.append(
                {
                    "symbol": symbol,
                    "session_date": "2026-01-01",
                    "x": index,
                    "y": sign * index,
                }
            )
    grouped = symbol_day_spearman(rows, "x", "y")
    assert abs(grouped["equal_weight_mean"]) < 1e-12
    assert grouped["symbol_days"] == 2
    first = block_bootstrap_mean(grouped["per_symbol_day"], seed=7, replicates=100)
    second = block_bootstrap_mean(grouped["per_symbol_day"], seed=7, replicates=100)
    assert first == second and first["blocks"] == 2
    adjusted = benjamini_hochberg({"a": 0.001, "b": 0.04, "c": 0.8})
    assert adjusted["a"]["reject"] is True
    assert adjusted["c"]["reject"] is False
    deciles = decile_response(np.arange(100), np.arange(100))
    assert len(deciles) == 10 and deciles[0]["mean_target"] < deciles[-1]["mean_target"]
    with pytest.raises(ValueError):
        block_bootstrap_mean({}, replicates=1)
    with pytest.raises(ValueError):
        benjamini_hochberg({"bad": 2.0})
    with pytest.raises(ValueError):
        benjamini_hochberg({}, alpha=2.0)
