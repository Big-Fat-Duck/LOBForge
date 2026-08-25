"""Frozen-protocol baselines, interpretable ablations, and block inference."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

import numpy as np
import numpy.typing as npt

from .evaluation import (
    CLASSES,
    block_bootstrap_mean,
    classification_metrics,
    regression_metrics,
    symbol_day_spearman,
)
from .microprice import StoikovMicroprice
from .models import (
    TrainOnlyLogisticClassifier,
    TrainOnlyRegressor,
    empirical_class_prior,
    hard_prediction_probabilities,
    majority_class,
    sign_baseline,
    zero_change_regression,
)

REGRESSION_TARGET = "target_mid2_delta_clock_100ms"
DIRECTION_TARGET = "target_direction_clock_100ms"


def _target_values(rows: Sequence[Mapping[str, Any]], target: str) -> npt.NDArray[np.float64]:
    return np.asarray([float(row[target]) for row in rows if row.get(target) is not None])


def _target_indices(rows: Sequence[Mapping[str, Any]], target: str) -> npt.NDArray[np.int64]:
    return np.asarray(
        [index for index, row in enumerate(rows) if row.get(target) is not None],
        dtype=np.int64,
    )


def _truth_at(
    rows: Sequence[Mapping[str, Any]],
    target: str,
    indices: npt.NDArray[np.int64],
) -> npt.NDArray[np.float64]:
    return np.asarray([float(rows[int(index)][target]) for index in indices], dtype=np.float64)


def _baseline_rows(
    rows: Sequence[Mapping[str, Any]], feature: str
) -> tuple[npt.NDArray[np.int8], npt.NDArray[np.float64], npt.NDArray[np.int64]]:
    truth: list[int] = []
    values: list[float] = []
    indices: list[int] = []
    for index, row in enumerate(rows):
        if row.get(DIRECTION_TARGET) is None or row.get(feature) is None:
            continue
        truth.append(int(row[DIRECTION_TARGET]))
        values.append(float(row[feature]))
        indices.append(index)
    return (
        np.asarray(truth, dtype=np.int8),
        np.asarray(values, dtype=np.float64),
        np.asarray(indices, dtype=np.int64),
    )


def _classification_result(
    truth: npt.NDArray[np.int8],
    prediction: npt.NDArray[np.int8],
    probability: npt.NDArray[np.float64],
) -> dict[str, Any]:
    return classification_metrics(truth, prediction, probability)


def _groups(
    rows: Sequence[Mapping[str, Any]], indices: npt.NDArray[np.int64]
) -> dict[str, list[int]]:
    grouped: dict[str, list[int]] = {}
    for position, index in enumerate(indices):
        row = rows[int(index)]
        key = f"{row['symbol']}|{row['session_date']}"
        grouped.setdefault(key, []).append(position)
    return grouped


def _mean_optional(values: Sequence[float | int | None]) -> float | None:
    finite = [float(value) for value in values if value is not None]
    return None if not finite else float(np.mean(finite))


def _regression_by_symbol_day(
    rows: Sequence[Mapping[str, Any]],
    indices: npt.NDArray[np.int64],
    truth: npt.NDArray[np.float64],
    prediction: npt.NDArray[np.float64],
) -> dict[str, Any]:
    per_group: dict[str, dict[str, float | int | None]] = {}
    for key, positions in sorted(_groups(rows, indices).items()):
        selected = np.asarray(positions, dtype=np.int64)
        per_group[key] = regression_metrics(truth[selected], prediction[selected])
    metric_names = (
        "mae",
        "rmse",
        "oos_r2_vs_zero",
        "pearson",
        "spearman",
        "zero_return_prevalence",
    )
    return {
        "per_symbol_day": per_group,
        "equal_weight": {
            metric: _mean_optional([group[metric] for group in per_group.values()])
            for metric in metric_names
        },
        "symbol_days": len(per_group),
    }


def _classification_by_symbol_day(
    rows: Sequence[Mapping[str, Any]],
    indices: npt.NDArray[np.int64],
    truth: npt.NDArray[np.int8],
    prediction: npt.NDArray[np.int8],
    probability: npt.NDArray[np.float64],
) -> dict[str, Any]:
    per_group: dict[str, dict[str, Any]] = {}
    for key, positions in sorted(_groups(rows, indices).items()):
        selected = np.asarray(positions, dtype=np.int64)
        per_group[key] = classification_metrics(
            truth[selected], prediction[selected], probability[selected]
        )
    scalar_names = (
        "macro_f1",
        "balanced_accuracy",
        "mcc",
        "log_loss",
        "brier",
        "roc_auc_ovr_macro",
        "raw_accuracy_secondary",
        "zero_return_prevalence",
    )
    equal_weight: dict[str, Any] = {
        metric: _mean_optional([group[metric] for group in per_group.values()])
        for metric in scalar_names
    }
    equal_weight["class_balance"] = {
        str(int(label)): _mean_optional(
            [group["class_balance"][str(int(label))] for group in per_group.values()]
        )
        for label in CLASSES
    }
    equal_weight["roc_auc_ovr"] = {
        str(int(label)): _mean_optional(
            [group["roc_auc_ovr"][str(int(label))] for group in per_group.values()]
        )
        for label in CLASSES
    }
    return {
        "per_symbol_day": per_group,
        "equal_weight": equal_weight,
        "symbol_days": len(per_group),
    }


def _fit_selected_classifier(
    train: Sequence[Mapping[str, Any]],
    validation: Sequence[Mapping[str, Any]],
    features: Sequence[str],
    *,
    seed: int,
) -> tuple[TrainOnlyLogisticClassifier, dict[str, Any]]:
    candidates: list[dict[str, float]] = []
    best_score = -np.inf
    selected: TrainOnlyLogisticClassifier | None = None
    selected_c = 0.0
    for c_value in (0.1, 1.0, 10.0):
        candidate = TrainOnlyLogisticClassifier(features, c_value=c_value, random_seed=seed).fit(
            train, DIRECTION_TARGET
        )
        probability, indices = candidate.predict_proba(validation, DIRECTION_TARGET)
        truth = _truth_at(validation, DIRECTION_TARGET, indices).astype(np.int8)
        prediction = CLASSES[np.argmax(probability, axis=1)]
        score = float(classification_metrics(truth, prediction, probability)["balanced_accuracy"])
        candidates.append({"c_value": c_value, "validation_balanced_accuracy": score})
        if score > best_score:
            best_score = score
            selected = candidate
            selected_c = c_value
    if selected is None:
        raise RuntimeError("no logistic candidate was selected")
    return selected, {
        "criterion": "validation_balanced_accuracy",
        "candidates": candidates,
        "selected_c": selected_c,
        "final_test_evaluations": 1,
    }


def evaluate_interpretable_suite(
    train: Sequence[Mapping[str, Any]],
    validation: Sequence[Mapping[str, Any]],
    test: Sequence[Mapping[str, Any]],
    *,
    seed: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Evaluate the frozen ablations; fitting and selection never inspect test labels."""

    microprice = StoikovMicroprice(minimum_state_samples=5).fit(train)
    train_rows = microprice.transform(train)
    validation_rows = microprice.transform(validation)
    test_rows = microprice.transform(test)

    regression_truth = _target_values(test_rows, REGRESSION_TARGET)
    regression_test_indices = _target_indices(test_rows, REGRESSION_TARGET)
    zero_prediction = zero_change_regression(regression_truth.size)
    regression: dict[str, Any] = {
        "zero_change_baseline": regression_metrics(regression_truth, zero_prediction)
    }
    regression_symbol_day: dict[str, Any] = {
        "zero_change_baseline": _regression_by_symbol_day(
            test_rows,
            regression_test_indices,
            regression_truth,
            zero_prediction,
        )
    }
    train_direction = _target_values(train_rows, DIRECTION_TARGET).astype(np.int8)
    test_direction = _target_values(test_rows, DIRECTION_TARGET).astype(np.int8)
    direction_test_indices = _target_indices(test_rows, DIRECTION_TARGET)
    prior_probability = empirical_class_prior(train_direction, test_direction.size)
    prior_prediction = CLASSES[np.argmax(prior_probability, axis=1)]
    majority_prediction = majority_class(train_direction, test_direction.size)
    majority_probability = hard_prediction_probabilities(majority_prediction)
    classification: dict[str, Any] = {
        "empirical_class_prior": _classification_result(
            test_direction, prior_prediction, prior_probability
        ),
        "majority_class": _classification_result(
            test_direction,
            majority_prediction,
            majority_probability,
        ),
    }
    classification_symbol_day: dict[str, Any] = {
        "empirical_class_prior": _classification_by_symbol_day(
            test_rows,
            direction_test_indices,
            test_direction,
            prior_prediction,
            prior_probability,
        ),
        "majority_class": _classification_by_symbol_day(
            test_rows,
            direction_test_indices,
            test_direction,
            majority_prediction,
            majority_probability,
        ),
    }
    for name, feature in (
        ("sign_imbalance_l1", "imbalance_l1"),
        ("sign_weighted_mid_displacement", "weighted_midprice_displacement"),
        ("sign_lagged_ofi", "ofi_l1_100ms"),
    ):
        truth, values, baseline_indices = _baseline_rows(test_rows, feature)
        prediction = sign_baseline(values)
        probability = hard_prediction_probabilities(prediction)
        classification[name] = _classification_result(truth, prediction, probability)
        classification_symbol_day[name] = _classification_by_symbol_day(
            test_rows, baseline_indices, truth, prediction, probability
        )

    ablations: dict[str, tuple[list[str], float]] = {
        "lagged_ofi_univariate": (["ofi_l1_100ms"], 0.0),
        "imbalance_only": (["imbalance_l1"], 0.0),
        "weighted_mid_displacement": (["weighted_midprice_displacement"], 0.0),
        "stoikov_microprice_displacement": (["stoikov_microprice_displacement"], 0.0),
        "regularized_imbalance_plus_ofi": (["imbalance_l1", "ofi_l1_100ms"], 1.0),
    }
    model_parameters: dict[str, Any] = {"stoikov": microprice.to_dict()}
    primary_prediction_rows: list[dict[str, Any]] = []
    for name, (features, alpha) in ablations.items():
        regressor = TrainOnlyRegressor(features, alpha=alpha).fit(train_rows, REGRESSION_TARGET)
        regression_prediction, regression_indices = regressor.predict(test_rows, REGRESSION_TARGET)
        regression_truth_values = _truth_at(test_rows, REGRESSION_TARGET, regression_indices)
        regression[name] = regression_metrics(regression_truth_values, regression_prediction)
        regression_symbol_day[name] = _regression_by_symbol_day(
            test_rows,
            regression_indices,
            regression_truth_values,
            regression_prediction,
        )
        if name == "lagged_ofi_univariate":
            for index, value in zip(regression_indices, regression_prediction, strict=True):
                row = dict(test_rows[int(index)])
                row["model_prediction"] = float(value)
                primary_prediction_rows.append(row)

        classifier, selection = _fit_selected_classifier(
            train_rows, validation_rows, features, seed=seed
        )
        probability, class_indices = classifier.predict_proba(test_rows, DIRECTION_TARGET)
        class_truth = _truth_at(test_rows, DIRECTION_TARGET, class_indices).astype(np.int8)
        class_prediction = CLASSES[np.argmax(probability, axis=1)]
        classification[name] = _classification_result(class_truth, class_prediction, probability)
        classification_symbol_day[name] = _classification_by_symbol_day(
            test_rows,
            class_indices,
            class_truth,
            class_prediction,
            probability,
        )
        model_parameters[name] = {
            "regression": regressor.parameters(),
            "classification": classifier.parameters(),
            "selection": selection,
        }

    primary_ic = symbol_day_spearman(primary_prediction_rows, "model_prediction", REGRESSION_TARGET)
    bootstrap = block_bootstrap_mean(primary_ic["per_symbol_day"], replicates=1000, seed=seed)
    suite = {
        "regression": regression,
        "classification": classification,
        "per_symbol_day_metrics": {
            "regression": regression_symbol_day,
            "classification": classification_symbol_day,
        },
        "primary_per_symbol_day_ic": primary_ic,
        "primary_whole_symbol_day_block_bootstrap": bootstrap,
        "aggregation": (
            "per_symbol_day_metrics.equal_weight is authoritative; row-level model metrics are "
            "retained as diagnostics"
        ),
        "selection_partition": "validation",
        "final_test_evaluations": 1,
        "collinearity_policy": (
            "imbalance and weighted-mid displacement are separate ablations; "
            "the regularized model uses imbalance plus lagged OFI only"
        ),
    }
    return suite, model_parameters
