from __future__ import annotations

import copy

import numpy as np
import pytest

from lobforge_research.models import (
    TrainOnlyLogisticClassifier,
    TrainOnlyRegressor,
    empirical_class_prior,
    hard_prediction_probabilities,
    majority_class,
    model_matrix,
    sign_baseline,
    zero_change_regression,
)
from lobforge_research.splits import (
    FitScope,
    chronological_date_split,
    select_symbols_train_only,
    validate_feature_columns,
)
from lobforge_research.synthetic import generate_planted_signal


def test_global_date_split_purge_and_symbol_cohesion():
    rows = []
    for date_index, date in enumerate(("2026-01-01", "2026-01-02", "2026-01-03", "2026-01-04")):
        for symbol in ("A", "B"):
            rows.append(
                {
                    "session_date": date,
                    "symbol": symbol,
                    "anchor_timestamp_ns": 500_000_000 if date_index else 1,
                }
            )
    split = chronological_date_split(
        rows, train_fraction=0.5, validation_fraction=0.25, purge_ns=1_000_000_000
    )
    assert split.train_dates == ("2026-01-01", "2026-01-02")
    assert split.validation_dates == ("2026-01-03",)
    assert split.test_dates == ("2026-01-04",)
    assert not split.validation
    assert not split.test
    assert split.purged_rows == {"train": 0, "validation": 2, "test": 2}
    with pytest.raises(ValueError):
        chronological_date_split(rows[:2])
    with pytest.raises(ValueError):
        chronological_date_split(rows, train_fraction=0.9, validation_fraction=0.2)


def test_feature_denylist_symbol_selection_and_fit_scope():
    available = {"ofi_l1_100ms", "target_mid2_delta_clock_100ms", "future_x"}
    assert validate_feature_columns(["ofi_l1_100ms"], available) == ("ofi_l1_100ms",)
    for forbidden in ("target_mid2_delta_clock_100ms", "future_x", "next_price", "lead_1"):
        with pytest.raises(ValueError):
            validate_feature_columns([forbidden], available | {forbidden})
    with pytest.raises(ValueError):
        validate_feature_columns([], available)
    with pytest.raises(ValueError):
        validate_feature_columns(["missing"], available)
    rows = [
        {"symbol": "B", "mid2": 1},
        {"symbol": "A", "mid2": 1},
        {"symbol": "A", "mid2": 1},
    ]
    assert select_symbols_train_only(rows, limit=2) == ("A", "B")
    with pytest.raises(ValueError):
        select_symbols_train_only(rows, limit=1, partition="validation")
    scope = FitScope()
    with pytest.raises(ValueError):
        scope.require_fitted()
    scope.require_train("train")
    scope.require_fitted()
    with pytest.raises(ValueError):
        scope.require_train("train")


def test_train_only_models_baselines_and_collinearity():
    rows = generate_planted_signal(days=4, symbols=2, rows_per_symbol_day=100)
    split = chronological_date_split(rows, train_fraction=0.5, validation_fraction=0.25)
    regression = TrainOnlyRegressor(["ofi_l1_100ms"])
    with pytest.raises(ValueError):
        regression.fit(split.train, "target_mid2_delta_clock_100ms", partition="validation")
    regression = TrainOnlyRegressor(["ofi_l1_100ms"]).fit(
        split.train, "target_mid2_delta_clock_100ms"
    )
    prediction, indices = regression.predict(split.test, "target_mid2_delta_clock_100ms")
    assert prediction.size == indices.size > 0
    assert regression.parameters()["coefficient"][0] > 0
    with pytest.raises(ValueError):
        TrainOnlyRegressor(["imbalance_l1", "weighted_midprice_displacement"])

    classifier = TrainOnlyLogisticClassifier(["ofi_l1_100ms"], calibration_fraction=0.2).fit(
        split.train, "target_direction_clock_100ms"
    )
    raw, raw_indices = classifier.predict_proba(
        split.test, "target_direction_clock_100ms", calibrated=False
    )
    calibrated, calibrated_indices = classifier.predict_proba(
        split.test, "target_direction_clock_100ms"
    )
    np.testing.assert_array_equal(raw_indices, calibrated_indices)
    np.testing.assert_allclose(raw.sum(axis=1), 1.0)
    np.testing.assert_allclose(calibrated.sum(axis=1), 1.0)
    assert classifier.parameters()["calibrator"]
    direction, _ = classifier.predict(split.test, "target_direction_clock_100ms")
    assert set(direction).issubset({-1, 0, 1})

    truth = np.asarray([-1, -1, 0, 1])
    np.testing.assert_array_equal(zero_change_regression(3), np.zeros(3))
    np.testing.assert_allclose(empirical_class_prior(truth, 2)[0], [0.5, 0.25, 0.25])
    np.testing.assert_array_equal(majority_class(truth, 2), [-1, -1])
    np.testing.assert_array_equal(sign_baseline([-2.0, 0.0, 3.0]), [-1, 0, 1])
    np.testing.assert_allclose(hard_prediction_probabilities([-1, 0, 1]).sum(axis=1), 1.0)


def test_model_matrix_missing_imputation_and_errors():
    rows = [
        {"x": None, "y": 1},
        {"x": 2, "y": None},
        {"x": 3, "y": 0},
    ]
    matrix, target, indices = model_matrix(rows, ["x"], "y")
    assert np.isnan(matrix[0, 0]) and target.tolist() == [1.0, 0.0]
    assert indices.tolist() == [0, 2]
    with pytest.raises(ValueError):
        model_matrix([], ["x"], "y")
    no_targets = copy.deepcopy(rows)
    for row in no_targets:
        row["y"] = None
    with pytest.raises(ValueError):
        model_matrix(no_targets, ["x"], "y")
    with pytest.raises(ValueError):
        TrainOnlyLogisticClassifier(["x"], calibration_fraction=0.8)
