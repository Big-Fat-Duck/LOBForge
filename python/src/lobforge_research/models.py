"""Train-only interpretable baselines and regularized linear models."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

import numpy as np
import numpy.typing as npt
from sklearn.impute import SimpleImputer
from sklearn.linear_model import LinearRegression, LogisticRegression, Ridge
from sklearn.preprocessing import StandardScaler

from .evaluation import CLASSES
from .splits import FitScope, validate_feature_columns


def model_matrix(
    rows: Sequence[Mapping[str, Any]], features: Sequence[str], target: str
) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64], npt.NDArray[np.int64]]:
    if not rows:
        raise ValueError("model matrix requires rows")
    selected = validate_feature_columns(features, rows[0].keys())
    matrix: list[list[float]] = []
    targets: list[float] = []
    indices: list[int] = []
    for index, row in enumerate(rows):
        if row.get(target) is None:
            continue
        matrix.append(
            [
                float("nan") if row.get(feature) is None else float(row[feature])
                for feature in selected
            ]
        )
        targets.append(float(row[target]))
        indices.append(index)
    if not matrix:
        raise ValueError("no rows have a target")
    return (
        np.asarray(matrix, dtype=np.float64),
        np.asarray(targets, dtype=np.float64),
        np.asarray(indices, dtype=np.int64),
    )


def _check_collinearity(features: Sequence[str]) -> None:
    selected = set(features)
    if {"imbalance_l1", "weighted_midprice_displacement"} <= selected:
        raise ValueError("imbalance_l1 and weighted-mid displacement require separate ablations")


class TrainOnlyRegressor:
    def __init__(self, features: Sequence[str], *, alpha: float = 0.0) -> None:
        _check_collinearity(features)
        self.features = tuple(features)
        self.alpha = alpha
        self.scope = FitScope()
        self.imputer = SimpleImputer(strategy="median")
        self.scaler = StandardScaler()
        self.estimator: LinearRegression | Ridge
        self.estimator = LinearRegression() if alpha == 0 else Ridge(alpha=alpha)

    def fit(
        self,
        rows: Sequence[Mapping[str, Any]],
        target: str,
        *,
        partition: str = "train",
    ) -> TrainOnlyRegressor:
        self.scope.require_train(partition)
        x, y, _ = model_matrix(rows, self.features, target)
        x_imputed = self.imputer.fit_transform(x)
        x_scaled = self.scaler.fit_transform(x_imputed)
        self.estimator.fit(x_scaled, y)
        return self

    def predict(
        self, rows: Sequence[Mapping[str, Any]], target: str
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.int64]]:
        self.scope.require_fitted()
        x, _, indices = model_matrix(rows, self.features, target)
        return self.estimator.predict(self.scaler.transform(self.imputer.transform(x))), indices

    def parameters(self) -> dict[str, Any]:
        self.scope.require_fitted()
        return {
            "features": list(self.features),
            "alpha": self.alpha,
            "imputer_statistics": self.imputer.statistics_.tolist(),
            "scaler_mean": self.scaler.mean_.tolist(),
            "scaler_scale": self.scaler.scale_.tolist(),
            "coefficient": np.asarray(self.estimator.coef_).tolist(),
            "intercept": np.asarray(self.estimator.intercept_).tolist(),
        }


def _aligned_probabilities(
    probabilities: npt.NDArray[np.float64], classes: npt.NDArray[Any]
) -> npt.NDArray[np.float64]:
    output = np.zeros((probabilities.shape[0], 3), dtype=np.float64)
    for source, label in enumerate(classes):
        destination = int(np.flatnonzero(int(label) == CLASSES)[0])
        output[:, destination] = probabilities[:, source]
    row_sum = output.sum(axis=1)
    aligned = np.zeros_like(output)
    np.divide(output, row_sum[:, None], out=aligned, where=row_sum[:, None] != 0)
    return np.asarray(aligned, dtype=np.float64)


class TrainOnlyLogisticClassifier:
    """Logistic model with a chronological train-tail multinomial calibrator."""

    def __init__(
        self,
        features: Sequence[str],
        *,
        c_value: float = 1.0,
        calibration_fraction: float = 0.20,
        random_seed: int = 0,
    ) -> None:
        _check_collinearity(features)
        if not 0 < calibration_fraction < 0.5:
            raise ValueError("calibration fraction must be in (0, 0.5)")
        self.features = tuple(features)
        self.c_value = c_value
        self.calibration_fraction = calibration_fraction
        self.random_seed = random_seed
        self.scope = FitScope()
        self.imputer = SimpleImputer(strategy="median")
        self.scaler = StandardScaler()
        self.estimator = LogisticRegression(
            C=c_value, max_iter=2000, solver="lbfgs", random_state=random_seed
        )
        self.calibrator: LogisticRegression | None = None
        self.calibration_reason: str | None = None

    def fit(
        self,
        rows: Sequence[Mapping[str, Any]],
        target: str,
        *,
        partition: str = "train",
    ) -> TrainOnlyLogisticClassifier:
        self.scope.require_train(partition)
        x, y_float, _ = model_matrix(rows, self.features, target)
        y = y_float.astype(np.int8)
        if not set(np.unique(y)).issubset(set(CLASSES.tolist())):
            raise ValueError("direction target must use {-1, 0, 1}")
        split = max(1, int(x.shape[0] * (1.0 - self.calibration_fraction)))
        if split >= x.shape[0]:
            raise ValueError("not enough rows for chronological calibration")
        core_x, calibration_x = x[:split], x[split:]
        core_y, calibration_y = y[:split], y[split:]
        if np.unique(core_y).size < 2:
            raise ValueError("classification training core requires at least two classes")
        core_imputed = self.imputer.fit_transform(core_x)
        core_scaled = self.scaler.fit_transform(core_imputed)
        self.estimator.fit(core_scaled, core_y)
        calibration_scaled = self.scaler.transform(self.imputer.transform(calibration_x))
        raw_probability = _aligned_probabilities(
            self.estimator.predict_proba(calibration_scaled), self.estimator.classes_
        )
        if np.unique(calibration_y).size < 2:
            self.calibration_reason = "calibration_tail_has_one_class"
            return self
        self.calibrator = LogisticRegression(
            C=1.0, max_iter=2000, solver="lbfgs", random_state=self.random_seed
        )
        self.calibrator.fit(np.log(np.clip(raw_probability, 1e-15, 1.0)), calibration_y)
        return self

    def predict_proba(
        self, rows: Sequence[Mapping[str, Any]], target: str, *, calibrated: bool = True
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.int64]]:
        self.scope.require_fitted()
        x, _, indices = model_matrix(rows, self.features, target)
        scaled = self.scaler.transform(self.imputer.transform(x))
        raw = _aligned_probabilities(self.estimator.predict_proba(scaled), self.estimator.classes_)
        if calibrated and self.calibrator is not None:
            calibrated_probability = self.calibrator.predict_proba(np.log(np.clip(raw, 1e-15, 1.0)))
            return _aligned_probabilities(calibrated_probability, self.calibrator.classes_), indices
        return raw, indices

    def predict(
        self, rows: Sequence[Mapping[str, Any]], target: str, *, calibrated: bool = True
    ) -> tuple[npt.NDArray[np.int8], npt.NDArray[np.int64]]:
        probability, indices = self.predict_proba(rows, target, calibrated=calibrated)
        return CLASSES[np.argmax(probability, axis=1)], indices

    def parameters(self) -> dict[str, Any]:
        self.scope.require_fitted()
        value: dict[str, Any] = {
            "features": list(self.features),
            "c_value": self.c_value,
            "calibration_fraction": self.calibration_fraction,
            "classes": self.estimator.classes_.tolist(),
            "coefficient": self.estimator.coef_.tolist(),
            "intercept": self.estimator.intercept_.tolist(),
            "imputer_statistics": self.imputer.statistics_.tolist(),
            "scaler_mean": self.scaler.mean_.tolist(),
            "scaler_scale": self.scaler.scale_.tolist(),
            "calibration_reason": self.calibration_reason,
        }
        if self.calibrator is not None:
            value["calibrator"] = {
                "classes": self.calibrator.classes_.tolist(),
                "coefficient": self.calibrator.coef_.tolist(),
                "intercept": self.calibrator.intercept_.tolist(),
            }
        return value


def zero_change_regression(count: int) -> npt.NDArray[np.float64]:
    return np.zeros(count, dtype=np.float64)


def empirical_class_prior(train_direction: npt.ArrayLike, count: int) -> npt.NDArray[np.float64]:
    train = np.asarray(train_direction, dtype=np.int8)
    prior = np.asarray([np.mean(train == label) for label in CLASSES], dtype=np.float64)
    return np.tile(prior, (count, 1))


def majority_class(train_direction: npt.ArrayLike, count: int) -> npt.NDArray[np.int8]:
    train = np.asarray(train_direction, dtype=np.int8)
    counts = np.asarray([np.sum(train == label) for label in CLASSES])
    return np.full(count, CLASSES[int(np.argmax(counts))], dtype=np.int8)


def sign_baseline(values: npt.ArrayLike) -> npt.NDArray[np.int8]:
    array = np.asarray(values, dtype=np.float64)
    return np.sign(array).astype(np.int8)


def hard_prediction_probabilities(
    prediction: npt.ArrayLike, confidence: float = 0.98
) -> npt.NDArray[np.float64]:
    predicted = np.asarray(prediction, dtype=np.int8)
    output = np.full((predicted.size, 3), (1.0 - confidence) / 2.0, dtype=np.float64)
    for index, label in enumerate(CLASSES):
        output[predicted == label, index] = confidence
    return output
