"""Global chronological date splits, purge, and train-only fitting guards."""

from __future__ import annotations

import datetime as dt
import math
from collections import Counter
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from typing import Any

DAY_NS = 86_400_000_000_000
FORBIDDEN_FEATURE_TOKENS = ("target", "future", "next", "lead")


@dataclass(slots=True)
class SplitResult:
    train: list[Mapping[str, Any]]
    validation: list[Mapping[str, Any]]
    test: list[Mapping[str, Any]]
    train_dates: tuple[str, ...]
    validation_dates: tuple[str, ...]
    test_dates: tuple[str, ...]
    purged_rows: dict[str, int]


def _absolute_ns(row: Mapping[str, Any]) -> int:
    date = dt.date.fromisoformat(str(row["session_date"]))
    return date.toordinal() * DAY_NS + int(row["anchor_timestamp_ns"])


def chronological_date_split(
    rows: Sequence[Mapping[str, Any]],
    *,
    train_fraction: float = 0.70,
    validation_fraction: float = 0.15,
    purge_ns: int = 1_000_000_000,
) -> SplitResult:
    """Split complete dates globally so all symbols on a date stay together."""

    if not 0 < train_fraction < 1 or not 0 < validation_fraction < 1:
        raise ValueError("split fractions must be in (0, 1)")
    if train_fraction + validation_fraction >= 1 or purge_ns < 0:
        raise ValueError("invalid split fractions or purge")
    dates = sorted({str(row["session_date"]) for row in rows})
    if len(dates) < 3:
        raise ValueError("at least three complete dates are required")
    train_count = max(1, math.floor(len(dates) * train_fraction))
    validation_count = max(1, math.floor(len(dates) * validation_fraction))
    if train_count + validation_count >= len(dates):
        validation_count = 1
        train_count = len(dates) - 2
    train_dates = tuple(dates[:train_count])
    validation_dates = tuple(dates[train_count : train_count + validation_count])
    test_dates = tuple(dates[train_count + validation_count :])
    validation_boundary = dt.date.fromisoformat(validation_dates[0]).toordinal() * DAY_NS
    test_boundary = dt.date.fromisoformat(test_dates[0]).toordinal() * DAY_NS

    unpurged: dict[str, list[Mapping[str, Any]]] = {
        "train": [],
        "validation": [],
        "test": [],
    }
    membership = {
        **dict.fromkeys(train_dates, "train"),
        **dict.fromkeys(validation_dates, "validation"),
        **dict.fromkeys(test_dates, "test"),
    }
    for row in rows:
        unpurged[membership[str(row["session_date"])]].append(row)

    train = [row for row in unpurged["train"] if _absolute_ns(row) + purge_ns < validation_boundary]
    validation = [
        row
        for row in unpurged["validation"]
        if _absolute_ns(row) - purge_ns >= validation_boundary
        and _absolute_ns(row) + purge_ns < test_boundary
    ]
    test = [row for row in unpurged["test"] if _absolute_ns(row) - purge_ns >= test_boundary]
    purged = {
        "train": len(unpurged["train"]) - len(train),
        "validation": len(unpurged["validation"]) - len(validation),
        "test": len(unpurged["test"]) - len(test),
    }
    return SplitResult(
        train,
        validation,
        test,
        train_dates,
        validation_dates,
        test_dates,
        purged,
    )


def validate_feature_columns(
    columns: Iterable[str], available_columns: Iterable[str]
) -> tuple[str, ...]:
    """Explicitly reject every common spelling of a future/label field."""

    selected = tuple(columns)
    available = set(available_columns)
    for column in selected:
        lowered = column.lower()
        if any(token in lowered for token in FORBIDDEN_FEATURE_TOKENS):
            raise ValueError(f"forbidden feature column: {column}")
        if column not in available:
            raise ValueError(f"unknown feature column: {column}")
    if not selected:
        raise ValueError("at least one feature column is required")
    return selected


def select_symbols_train_only(
    rows: Iterable[Mapping[str, Any]], *, limit: int, partition: str = "train"
) -> tuple[str, ...]:
    if partition != "train":
        raise ValueError("symbol selection may only use the train partition")
    counts = Counter(str(row["symbol"]) for row in rows if row.get("mid2") is not None)
    ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))
    return tuple(symbol for symbol, _ in ranked[:limit])


class FitScope:
    """Small reusable guard for estimators with train-only learned state."""

    def __init__(self) -> None:
        self.fitted = False

    def require_train(self, partition: str) -> None:
        if partition != "train":
            raise ValueError("learned state may only be fitted on train")
        if self.fitted:
            raise ValueError("fit may only be performed once")
        self.fitted = True

    def require_fitted(self) -> None:
        if not self.fitted:
            raise ValueError("transform requested before fit")
