from __future__ import annotations

import numpy as np
import pytest

from lobforge_research.round4_calibration import (
    assert_fit_dates_are_train_only,
    fit_strategy_parameters,
)


def test_train_only_fit_and_future_perturbation() -> None:
    timestamps = np.arange(10, dtype=np.int64) * 1_000_000_000
    mid2 = np.asarray([200, 202, 200, 204, 202, 206, 204, 208, 206, 210], dtype=np.float64)
    signal = np.linspace(-1.0, 1.0, 10)
    target = 2.0 * signal
    fitted = fit_strategy_parameters(
        mid2=mid2,
        timestamp_ns=timestamps,
        signal=signal,
        target_mid2_delta=target,
        partition="train",
        protocol_sha256="a" * 64,
    )
    assert fitted.fitted_partition == "train"
    assert fitted.fitted_protocol_sha256 == "a" * 64
    assert fitted.signal_coefficient_price4 == pytest.approx(1.0)
    assert fitted.sigma_squared > 0.0 and fitted.arrival_intensity_k == 1.0
    future_labels = target.copy()
    future_labels[8:] = 10_000
    prefix_again = fit_strategy_parameters(
        mid2=mid2[:8],
        timestamp_ns=timestamps[:8],
        signal=signal[:8],
        target_mid2_delta=target[:8],
        partition="train",
        protocol_sha256="a" * 64,
    )
    prefix_perturbed = fit_strategy_parameters(
        mid2=mid2[:8],
        timestamp_ns=timestamps[:8],
        signal=signal[:8],
        target_mid2_delta=future_labels[:8],
        partition="train",
        protocol_sha256="a" * 64,
    )
    assert prefix_again == prefix_perturbed


@pytest.mark.parametrize("partition", ["validation", "test", "all"])
def test_non_train_fit_rejected(partition: str) -> None:
    with pytest.raises(ValueError, match="only be fitted on train"):
        fit_strategy_parameters(
            mid2=[1, 2, 3],
            timestamp_ns=[1, 2, 3],
            signal=[0, 0, 0],
            target_mid2_delta=[0, 0, 0],
            partition=partition,
            protocol_sha256="a" * 64,
        )


def test_calibration_negative_inputs_and_date_scope() -> None:
    base = {
        "mid2": [1, 2, 3],
        "timestamp_ns": [1, 2, 3],
        "signal": [0, 1, 2],
        "target_mid2_delta": [0, 1, 2],
        "partition": "train",
        "protocol_sha256": "a" * 64,
    }
    for key, value, message in (
        ("protocol_sha256", "short", "64"),
        ("protocol_sha256", "z" * 64, "hexadecimal"),
        ("mid2", [[1], [2], [3]], "one-dimensional"),
        ("signal", [1, 2], "equal length"),
        ("signal", [1, np.nan, 2], "finite"),
        ("timestamp_ns", [1, 1, 3], "strictly increasing"),
    ):
        with pytest.raises(ValueError, match=message):
            fit_strategy_parameters(**{**base, key: value})
    assert_fit_dates_are_train_only(
        {"2026-01-01"},
        train_dates={"2026-01-01"},
        validation_dates={"2026-01-02"},
        test_dates={"2026-01-03"},
    )
    with pytest.raises(ValueError, match="subset"):
        assert_fit_dates_are_train_only(
            {"2026-01-02"},
            train_dates={"2026-01-01"},
            validation_dates={"2026-01-02"},
            test_dates={"2026-01-03"},
        )
    with pytest.raises(ValueError, match="overlap"):
        assert_fit_dates_are_train_only(
            {"2026-01-01"},
            train_dates={"2026-01-01"},
            validation_dates={"2026-01-01"},
            test_dates=set(),
        )
