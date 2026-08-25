"""Interpretable metrics, calibration, block inference, and FDR control."""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Callable, Mapping, Sequence
from typing import Any

import numpy as np
import numpy.typing as npt
from scipy.stats import pearsonr, spearmanr
from sklearn.metrics import (
    balanced_accuracy_score,
    confusion_matrix,
    f1_score,
    log_loss,
    matthews_corrcoef,
    roc_auc_score,
)

CLASSES = np.asarray([-1, 0, 1], dtype=np.int8)


def _finite_pair(
    truth: npt.ArrayLike, prediction: npt.ArrayLike
) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
    y = np.asarray(truth, dtype=np.float64)
    p = np.asarray(prediction, dtype=np.float64)
    if y.shape != p.shape:
        raise ValueError("truth and prediction shapes differ")
    mask = np.isfinite(y) & np.isfinite(p)
    return y[mask], p[mask]


def _correlation(
    function: Callable[[npt.ArrayLike, npt.ArrayLike], Any],
    truth: npt.NDArray[np.float64],
    prediction: npt.NDArray[np.float64],
) -> float | None:
    if truth.size < 2 or np.ptp(truth) == 0 or np.ptp(prediction) == 0:
        return None
    value = float(function(truth, prediction).statistic)
    return value if np.isfinite(value) else None


def regression_metrics(
    truth: npt.ArrayLike, prediction: npt.ArrayLike
) -> dict[str, float | int | None]:
    y, p = _finite_pair(truth, prediction)
    if not y.size:
        raise ValueError("no finite regression observations")
    residual = y - p
    baseline_sse = float(np.dot(y, y))
    sse = float(np.dot(residual, residual))
    return {
        "observations": int(y.size),
        "mae": float(np.mean(np.abs(residual))),
        "rmse": float(np.sqrt(np.mean(residual**2))),
        "oos_r2_vs_zero": None if baseline_sse == 0 else 1.0 - sse / baseline_sse,
        "pearson": _correlation(pearsonr, y, p),
        "spearman": _correlation(spearmanr, y, p),
        "zero_return_prevalence": float(np.mean(y == 0)),
    }


def multiclass_brier(truth: npt.ArrayLike, probabilities: npt.ArrayLike) -> float:
    y = np.asarray(truth, dtype=np.int8)
    probability = np.asarray(probabilities, dtype=np.float64)
    if probability.shape != (y.size, CLASSES.size):
        raise ValueError("probability matrix must have one column per {-1,0,1} class")
    expected = (y[:, None] == CLASSES[None, :]).astype(np.float64)
    return float(np.mean(np.sum((probability - expected) ** 2, axis=1)))


def classification_metrics(
    truth: npt.ArrayLike,
    prediction: npt.ArrayLike,
    probabilities: npt.ArrayLike,
) -> dict[str, Any]:
    y = np.asarray(truth, dtype=np.int8)
    predicted = np.asarray(prediction, dtype=np.int8)
    probability = np.asarray(probabilities, dtype=np.float64)
    if y.shape != predicted.shape or probability.shape != (y.size, 3):
        raise ValueError("classification inputs have inconsistent shapes")
    if not np.allclose(probability.sum(axis=1), 1.0, atol=1e-12):
        raise ValueError("probability rows must sum to one")
    class_balance = {str(int(label)): float(np.mean(y == label)) for label in CLASSES}
    auc: dict[str, float | None] = {}
    auc_values: list[float] = []
    for index, label in enumerate(CLASSES):
        binary = y == label
        if binary.all() or not binary.any():
            auc[str(int(label))] = None
        else:
            value = float(roc_auc_score(binary, probability[:, index]))
            auc[str(int(label))] = value
            auc_values.append(value)
    return {
        "observations": int(y.size),
        "class_balance": class_balance,
        "confusion_matrix": confusion_matrix(y, predicted, labels=CLASSES).tolist(),
        "macro_f1": float(f1_score(y, predicted, labels=CLASSES, average="macro", zero_division=0)),
        "balanced_accuracy": float(balanced_accuracy_score(y, predicted)),
        "mcc": float(matthews_corrcoef(y, predicted)),
        "log_loss": float(log_loss(y, probability, labels=CLASSES)),
        "brier": multiclass_brier(y, probability),
        "roc_auc_ovr": auc,
        "roc_auc_ovr_macro": None if not auc_values else float(np.mean(auc_values)),
        "raw_accuracy_secondary": float(np.mean(y == predicted)),
        "zero_return_prevalence": float(np.mean(y == 0)),
    }


def reliability_table(
    truth: npt.ArrayLike, probabilities: npt.ArrayLike, bins: int = 10
) -> dict[str, Any]:
    """Equal-frequency confidence calibration for multiclass probabilities."""

    y = np.asarray(truth, dtype=np.int8)
    probability = np.asarray(probabilities, dtype=np.float64)
    if bins < 2 or probability.shape != (y.size, 3):
        raise ValueError("invalid reliability inputs")
    prediction_index = np.argmax(probability, axis=1)
    prediction = CLASSES[prediction_index]
    confidence = probability[np.arange(y.size), prediction_index]
    order = np.argsort(confidence, kind="stable")
    groups = [group for group in np.array_split(order, bins) if group.size]
    table: list[dict[str, float | int]] = []
    ece = 0.0
    maximum = 0.0
    for index, group in enumerate(groups):
        mean_confidence = float(np.mean(confidence[group]))
        accuracy = float(np.mean(prediction[group] == y[group]))
        error = abs(mean_confidence - accuracy)
        ece += group.size / y.size * error
        maximum = max(maximum, error)
        table.append(
            {
                "bin": index + 1,
                "count": int(group.size),
                "mean_confidence": mean_confidence,
                "empirical_accuracy": accuracy,
            }
        )
    return {
        "brier": multiclass_brier(y, probability),
        "ece_equal_frequency_10": ece,
        "maximum_calibration_error": maximum,
        "table": table,
    }


def symbol_day_spearman(
    rows: Sequence[Mapping[str, Any]], feature: str, target: str
) -> dict[str, Any]:
    grouped: dict[tuple[str, str], list[tuple[float, float]]] = defaultdict(list)
    for row in rows:
        if row.get(feature) is not None and row.get(target) is not None:
            grouped[(str(row["symbol"]), str(row["session_date"]))].append(
                (float(row[feature]), float(row[target]))
            )
    values: dict[str, float | None] = {}
    finite: list[float] = []
    for key in sorted(grouped):
        pairs = np.asarray(grouped[key], dtype=np.float64)
        value = _correlation(spearmanr, pairs[:, 1], pairs[:, 0])
        values[f"{key[0]}|{key[1]}"] = value
        if value is not None:
            finite.append(value)
    return {
        "per_symbol_day": values,
        "equal_weight_mean": None if not finite else float(np.mean(finite)),
        "positive_fraction": None if not finite else float(np.mean(np.asarray(finite) > 0)),
        "symbol_days": len(finite),
    }


def block_bootstrap_mean(
    symbol_day_values: Mapping[str, float | None],
    *,
    replicates: int = 1000,
    confidence: float = 0.95,
    seed: int = 0,
) -> dict[str, float | int]:
    values = np.asarray(
        [value for value in symbol_day_values.values() if value is not None], dtype=np.float64
    )
    if not values.size or replicates < 1 or not 0 < confidence < 1:
        raise ValueError("invalid block bootstrap inputs")
    generator = np.random.default_rng(seed)
    samples = generator.choice(values, size=(replicates, values.size), replace=True).mean(axis=1)
    alpha = (1.0 - confidence) / 2.0
    return {
        "blocks": int(values.size),
        "replicates": replicates,
        "mean": float(np.mean(values)),
        "ci_lower": float(np.quantile(samples, alpha)),
        "ci_upper": float(np.quantile(samples, 1.0 - alpha)),
    }


def benjamini_hochberg(
    p_values: Mapping[str, float], alpha: float = 0.05
) -> dict[str, dict[str, float | bool]]:
    if not 0 < alpha < 1:
        raise ValueError("alpha must be in (0, 1)")
    ordered = sorted(p_values.items(), key=lambda item: (item[1], item[0]))
    count = len(ordered)
    adjusted = [0.0] * count
    running = 1.0
    for reverse_index in range(count - 1, -1, -1):
        rank = reverse_index + 1
        name, value = ordered[reverse_index]
        if not 0 <= value <= 1:
            raise ValueError(f"invalid p-value for {name}")
        running = min(running, value * count / rank)
        adjusted[reverse_index] = running
    return {
        name: {
            "p_value": value,
            "adjusted_p_value": adjusted[index],
            "reject": adjusted[index] <= alpha,
        }
        for index, (name, value) in enumerate(ordered)
    }


def decile_response(
    feature: npt.ArrayLike, target: npt.ArrayLike, bins: int = 10
) -> list[dict[str, float | int]]:
    x, y = _finite_pair(feature, target)
    order = np.argsort(x, kind="stable")
    output: list[dict[str, float | int]] = []
    for index, group in enumerate(np.array_split(order, bins)):
        if group.size:
            output.append(
                {
                    "bin": index + 1,
                    "count": int(group.size),
                    "mean_feature": float(np.mean(x[group])),
                    "mean_target": float(np.mean(y[group])),
                }
            )
    return output
