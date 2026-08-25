"""Independent deterministic planted-signal and null/shuffled controls."""

from __future__ import annotations

import copy
import datetime as dt
from collections.abc import Mapping, Sequence
from typing import Any

import numpy as np
from sklearn.metrics import balanced_accuracy_score

from .canonical import LogicalDigest
from .evaluation import decile_response, regression_metrics, symbol_day_spearman
from .microprice import StoikovMicroprice
from .models import TrainOnlyLogisticClassifier, TrainOnlyRegressor
from .splits import chronological_date_split


def _base_row(
    *,
    date: str,
    symbol: str,
    index: int,
    segment: int,
    mid2: int,
    imbalance: float,
    ofi: int,
    delta: int,
) -> dict[str, Any]:
    spread = 2
    weighted_displacement = spread * imbalance / 2.0
    direction = (delta > 0) - (delta < 0)
    return {
        "session_date": date,
        "symbol": symbol,
        "sample_kind": "event",
        "grid_ns": None,
        "anchor_sequence": index + 1,
        "anchor_timestamp_ns": index * 100_000_000,
        "source_state_timestamp_ns": index * 100_000_000,
        "segment_id": segment,
        "max_feature_sequence": index + 1,
        "mid2": mid2,
        "spread_price4": spread,
        "imbalance_l1": imbalance,
        "imbalance_l5": imbalance * 0.8,
        "imbalance_l10": imbalance * 0.6,
        "weighted_midprice_l1": mid2 / 2.0 + weighted_displacement,
        "weighted_midprice_displacement": weighted_displacement,
        "ofi_l1_events_10": ofi,
        "ofi_l1_events_50": ofi,
        "ofi_l1_events_100": ofi,
        "ofi_l1_10ms": ofi,
        "ofi_l1_100ms": ofi,
        "ofi_l1_1s": ofi,
        "stoikov_microprice": None,
        "stoikov_microprice_displacement": None,
        "stoikov_fallback_reason": None,
        "target_mid2_delta_event_1": delta,
        "target_direction_event_1": direction,
        "target_mid2_delta_event_10": None,
        "target_direction_event_10": None,
        "target_mid2_delta_event_50": None,
        "target_direction_event_50": None,
        "target_mid2_delta_event_100": None,
        "target_direction_event_100": None,
        "target_mid2_delta_clock_10ms": None,
        "target_direction_clock_10ms": None,
        "target_mid2_delta_clock_100ms": delta,
        "target_direction_clock_100ms": direction,
        "target_mid2_delta_clock_1s": None,
        "target_direction_clock_1s": None,
    }


def generate_planted_signal(
    *,
    seed: int = 20260825,
    days: int = 20,
    symbols: int = 5,
    rows_per_symbol_day: int = 500,
) -> list[dict[str, Any]]:
    """Positive oracle: future mid changes depend on already-observed OFI/imbalance."""

    generator = np.random.default_rng(seed)
    output: list[dict[str, Any]] = []
    start = dt.date(2026, 1, 5)
    sequence = 0
    for day_index in range(days):
        date = (start + dt.timedelta(days=day_index)).isoformat()
        for symbol_index in range(symbols):
            symbol = f"S{symbol_index + 1:02d}"
            mid2 = 2_000_000 + symbol_index * 10_000
            for _local_index in range(rows_per_symbol_day):
                sequence += 1
                raw_ofi = float(generator.normal())
                imbalance = float(np.tanh(0.75 * raw_ofi + generator.normal(scale=0.35)))
                ofi = round(raw_ofi * 1000.0)
                latent = 1.15 * raw_ofi + 0.55 * imbalance + generator.normal(scale=0.55)
                delta = 2 if latent > 0.45 else (-2 if latent < -0.45 else 0)
                output.append(
                    _base_row(
                        date=date,
                        symbol=symbol,
                        index=sequence,
                        segment=day_index * symbols + symbol_index,
                        mid2=mid2,
                        imbalance=imbalance,
                        ofi=ofi,
                        delta=delta,
                    )
                )
                mid2 += delta
    return output


def generate_null_control(
    *,
    seed: int = 20260826,
    days: int = 20,
    symbols: int = 5,
    rows_per_symbol_day: int = 1000,
) -> list[dict[str, Any]]:
    """Null oracle: future labels are generated independently of all past features."""

    generator = np.random.default_rng(seed)
    output: list[dict[str, Any]] = []
    start = dt.date(2026, 2, 2)
    sequence = 0
    choices = np.asarray([-2, 0, 2], dtype=np.int64)
    for day_index in range(days):
        date = (start + dt.timedelta(days=day_index)).isoformat()
        for symbol_index in range(symbols):
            symbol = f"N{symbol_index + 1:02d}"
            mid2 = 3_000_000 + symbol_index * 10_000
            for _ in range(rows_per_symbol_day):
                sequence += 1
                raw_ofi = float(generator.normal())
                imbalance = float(generator.uniform(-1.0, 1.0))
                ofi = round(raw_ofi * 1000.0)
                delta = int(generator.choice(choices))
                output.append(
                    _base_row(
                        date=date,
                        symbol=symbol,
                        index=sequence,
                        segment=day_index * symbols + symbol_index,
                        mid2=mid2,
                        imbalance=imbalance,
                        ofi=ofi,
                        delta=delta,
                    )
                )
                mid2 += delta
    return output


def shuffled_labels(
    rows: Sequence[Mapping[str, Any]], seed: int = 20260827
) -> list[dict[str, Any]]:
    generator = np.random.default_rng(seed)
    output = [copy.deepcopy(dict(row)) for row in rows]
    permutation = generator.permutation(len(rows))
    target_columns = [column for column in rows[0] if column.startswith("target_")]
    for destination, source in enumerate(permutation):
        for column in target_columns:
            output[destination][column] = rows[int(source)][column]
    return output


def feature_logical_digest(rows: Sequence[Mapping[str, Any]]) -> str:
    digest = LogicalDigest()
    for row in rows:
        digest.update({key: value for key, value in row.items() if not key.startswith("target_")})
    return digest.hexdigest()


def _truth(rows: Sequence[Mapping[str, Any]], column: str, indices: np.ndarray) -> np.ndarray:
    return np.asarray([rows[int(index)][column] for index in indices])


def run_synthetic_controls(
    *, seed: int = 20260825, rows_per_symbol_day: int = 500
) -> dict[str, Any]:
    positive = generate_planted_signal(seed=seed, rows_per_symbol_day=rows_per_symbol_day)
    null = generate_null_control(seed=seed + 1, rows_per_symbol_day=rows_per_symbol_day * 2)
    positive_split = chronological_date_split(positive)
    null_split = chronological_date_split(null)
    feature = "ofi_l1_100ms"
    regression_target = "target_mid2_delta_clock_100ms"
    direction_target = "target_direction_clock_100ms"

    positive_regression = TrainOnlyRegressor([feature]).fit(positive_split.train, regression_target)
    positive_prediction, positive_indices = positive_regression.predict(
        positive_split.test, regression_target
    )
    positive_rows_with_prediction: list[dict[str, Any]] = []
    for index, prediction in zip(positive_indices, positive_prediction, strict=True):
        row = dict(positive_split.test[int(index)])
        row["prediction"] = float(prediction)
        positive_rows_with_prediction.append(row)
    positive_ic = symbol_day_spearman(
        positive_rows_with_prediction, "prediction", regression_target
    )
    positive_classifier = TrainOnlyLogisticClassifier([feature], random_seed=seed).fit(
        positive_split.train, direction_target
    )
    positive_direction, positive_class_indices = positive_classifier.predict(
        positive_split.test, direction_target
    )
    positive_direction_truth = _truth(
        positive_split.test, direction_target, positive_class_indices
    ).astype(np.int8)
    deciles = decile_response(
        [row[feature] for row in positive_split.test],
        [row[regression_target] for row in positive_split.test],
    )
    decile_monotonicity = float(
        np.corrcoef(np.arange(len(deciles)), [float(row["mean_target"]) for row in deciles])[0, 1]
    )
    microprice = StoikovMicroprice(minimum_state_samples=5).fit(positive_split.train)
    microprice_rows = microprice.transform(positive_split.test)
    directional = [
        np.sign(float(row["stoikov_microprice_displacement"]))
        == np.sign(float(row["imbalance_l1"]))
        for row in microprice_rows
        if row["stoikov_microprice_displacement"] is not None and row["imbalance_l1"] != 0
    ]

    null_regression = TrainOnlyRegressor([feature]).fit(null_split.train, regression_target)
    null_prediction, null_indices = null_regression.predict(null_split.test, regression_target)
    null_truth = _truth(null_split.test, regression_target, null_indices)
    nonzero = null_truth != 0
    null_binary_balanced = float(
        balanced_accuracy_score(np.sign(null_truth[nonzero]), np.sign(null_prediction[nonzero]))
    )
    null_metrics = regression_metrics(null_truth, null_prediction)
    shuffled = shuffled_labels(null, seed + 2)
    feature_digest_unchanged = feature_logical_digest(null) == feature_logical_digest(shuffled)

    positive_balanced = float(balanced_accuracy_score(positive_direction_truth, positive_direction))
    positive_ic_mean = positive_ic["equal_weight_mean"]
    null_ic = null_metrics["spearman"]
    checks = {
        "positive_coefficient_direction": bool(
            np.asarray(positive_regression.estimator.coef_).reshape(-1)[0] > 0
        ),
        "positive_pooled_spearman_at_least_0_10": bool(
            positive_ic_mean is not None and positive_ic_mean >= 0.10
        ),
        "positive_balanced_accuracy_at_least_0_60": positive_balanced >= 0.60,
        "positive_decile_response_basically_monotonic": decile_monotonicity >= 0.90,
        "positive_microprice_direction": bool(directional and np.mean(directional) >= 0.60),
        "null_abs_ic_below_0_02": bool(null_ic is not None and abs(null_ic) < 0.02),
        "null_binary_balanced_accuracy_0_48_to_0_52": 0.48 <= null_binary_balanced <= 0.52,
        "shuffle_preserves_historical_feature_digest": feature_digest_unchanged,
        "does_not_claim_real_market_signal": True,
    }
    return {
        "control_schema": "lobforge.synthetic_controls",
        "version": 1,
        "seed": seed,
        "positive": {
            "rows": len(positive),
            "pooled_symbol_day_spearman": positive_ic_mean,
            "balanced_accuracy": positive_balanced,
            "decile_monotonicity": decile_monotonicity,
            "microprice_direction_fraction": None
            if not directional
            else float(np.mean(directional)),
        },
        "null": {
            "rows": len(null),
            "spearman": null_ic,
            "binary_nonzero_balanced_accuracy": null_binary_balanced,
        },
        "checks": checks,
        "engineering_only": True,
        "real_market_signal_status": "BLOCKED",
    }
