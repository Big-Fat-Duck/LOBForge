"""Train-only calibration metadata for interpretable Round 4 strategies."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import numpy.typing as npt


@dataclass(frozen=True)
class FrozenStrategyParameters:
    sigma_squared: float
    arrival_intensity_k: float
    signal_coefficient_price4: float
    fitted_partition: str
    fitted_protocol_sha256: str
    fitted_start_timestamp_ns: int
    fitted_end_timestamp_ns: int
    observations: int


def fit_strategy_parameters(
    *,
    mid2: npt.ArrayLike,
    timestamp_ns: npt.ArrayLike,
    signal: npt.ArrayLike,
    target_mid2_delta: npt.ArrayLike,
    partition: str,
    protocol_sha256: str,
) -> FrozenStrategyParameters:
    """Fit only on an explicitly named training partition; never update during transform."""

    if partition != "train":
        raise ValueError("Round 4 strategy parameters may only be fitted on train")
    if len(protocol_sha256) != 64:
        raise ValueError("protocol SHA-256 must contain 64 hexadecimal characters")
    try:
        int(protocol_sha256, 16)
    except ValueError as error:
        raise ValueError("protocol SHA-256 is not hexadecimal") from error
    prices = np.asarray(mid2, dtype=np.float64)
    times = np.asarray(timestamp_ns, dtype=np.int64)
    signals = np.asarray(signal, dtype=np.float64)
    targets = np.asarray(target_mid2_delta, dtype=np.float64)
    if not (prices.ndim == times.ndim == signals.ndim == targets.ndim == 1):
        raise ValueError("calibration arrays must be one-dimensional")
    if not (prices.size == times.size == signals.size == targets.size) or prices.size < 3:
        raise ValueError("calibration arrays must have equal length of at least three")
    if not (
        np.all(np.isfinite(prices))
        and np.all(np.isfinite(signals))
        and np.all(np.isfinite(targets))
    ):
        raise ValueError("calibration arrays must be finite")
    elapsed = np.diff(times)
    if np.any(elapsed <= 0):
        raise ValueError("calibration timestamps must be strictly increasing")
    price4_changes = np.diff(prices) / 2.0
    seconds = float((times[-1] - times[0]) / 1_000_000_000.0)
    sigma_squared = float(np.var(price4_changes, ddof=1) / np.mean(elapsed / 1e9))
    arrival_intensity_k = float((prices.size - 1) / seconds)
    centered_signal = signals - np.mean(signals)
    centered_target = targets - np.mean(targets)
    denominator = float(centered_signal @ centered_signal)
    coefficient = (
        0.0 if denominator == 0.0 else float(centered_signal @ centered_target / denominator)
    )
    return FrozenStrategyParameters(
        sigma_squared=sigma_squared,
        arrival_intensity_k=arrival_intensity_k,
        signal_coefficient_price4=coefficient / 2.0,
        fitted_partition=partition,
        fitted_protocol_sha256=protocol_sha256,
        fitted_start_timestamp_ns=int(times[0]),
        fitted_end_timestamp_ns=int(times[-1]),
        observations=int(prices.size),
    )


def assert_fit_dates_are_train_only(
    fit_dates: set[str], *, train_dates: set[str], validation_dates: set[str], test_dates: set[str]
) -> None:
    if validation_dates & test_dates or train_dates & validation_dates or train_dates & test_dates:
        raise ValueError("date partitions overlap")
    if not fit_dates or not fit_dates <= train_dates:
        raise ValueError("fitting dates must be a non-empty subset of train dates")
