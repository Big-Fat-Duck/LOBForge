"""Deterministic book features and past-only Cont-style L1 OFI."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from typing import Any

import numpy as np
import numpy.typing as npt

EVENT_WINDOWS = (10, 50, 100)
TIME_WINDOWS_NS = (10_000_000, 100_000_000, 1_000_000_000)


def is_valid_continuous_book(event: dict[str, Any]) -> bool:
    """Return whether price features may be computed for this post-event state."""

    return bool(
        event["session_state"] == "market_hours"
        and event["trading_state"] == "T"
        and event["two_sided"]
        and not event["crossed"]
        and event["bids"]
        and event["asks"]
    )


def _level_quantity(level: dict[str, int]) -> int:
    return level["aggregate_qty"]


def _imbalance(event: dict[str, Any], levels: int) -> float | None:
    bids = event["bids"]
    asks = event["asks"]
    if len(bids) < levels or len(asks) < levels:
        return None
    bid_quantity = sum(_level_quantity(level) for level in bids[:levels])
    ask_quantity = sum(_level_quantity(level) for level in asks[:levels])
    denominator = bid_quantity + ask_quantity
    if denominator == 0:
        return None
    return (bid_quantity - ask_quantity) / denominator


def static_book_features(event: dict[str, Any]) -> dict[str, int | float | None]:
    """Compute exact prices and documented static imbalance features."""

    result: dict[str, int | float | None] = {
        "mid2": None,
        "spread_price4": None,
        "imbalance_l1": None,
        "imbalance_l5": None,
        "imbalance_l10": None,
        "weighted_midprice_l1": None,
        "weighted_midprice_displacement": None,
    }
    if not is_valid_continuous_book(event):
        return result
    bid = event["bids"][0]["price4"]
    ask = event["asks"][0]["price4"]
    bid_quantity = event["bids"][0]["aggregate_qty"]
    ask_quantity = event["asks"][0]["aggregate_qty"]
    mid2 = bid + ask
    weighted = (ask * bid_quantity + bid * ask_quantity) / (bid_quantity + ask_quantity)
    result.update(
        {
            "mid2": mid2,
            "spread_price4": ask - bid,
            "imbalance_l1": _imbalance(event, 1),
            "imbalance_l5": _imbalance(event, 5),
            "imbalance_l10": _imbalance(event, 10),
            "weighted_midprice_l1": weighted,
            "weighted_midprice_displacement": weighted - mid2 / 2.0,
        }
    )
    return result


def cont_ofi_increment(
    previous_bid: int,
    previous_bid_quantity: int,
    previous_ask: int,
    previous_ask_quantity: int,
    bid: int,
    bid_quantity: int,
    ask: int,
    ask_quantity: int,
) -> int:
    """Compute the original Cont et al. L1 order-flow imbalance increment."""

    return (
        (bid_quantity if bid >= previous_bid else 0)
        - (previous_bid_quantity if bid <= previous_bid else 0)
        - (ask_quantity if ask <= previous_ask else 0)
        + (previous_ask_quantity if ask >= previous_ask else 0)
    )


@dataclass(slots=True, frozen=True)
class L1State:
    bid: int
    bid_quantity: int
    ask: int
    ask_quantity: int


@dataclass(slots=True)
class _SymbolOfiState:
    session_date: str = ""
    valid: bool = False
    previous: L1State | None = None
    segment_id: int = 0
    segment_start_ns: int = 0
    event_queues: dict[int, deque[int]] = field(
        default_factory=lambda: {window: deque() for window in EVENT_WINDOWS}
    )
    event_sums: dict[int, int] = field(default_factory=lambda: dict.fromkeys(EVENT_WINDOWS, 0))
    time_queues: dict[int, deque[tuple[int, int, int]]] = field(
        default_factory=lambda: {window: deque() for window in TIME_WINDOWS_NS}
    )
    time_sums: dict[int, int] = field(default_factory=lambda: dict.fromkeys(TIME_WINDOWS_NS, 0))
    last_feature: dict[str, Any] | None = None

    def reset(self) -> None:
        self.valid = False
        self.previous = None
        for window in EVENT_WINDOWS:
            self.event_queues[window].clear()
            self.event_sums[window] = 0
        for duration in TIME_WINDOWS_NS:
            self.time_queues[duration].clear()
            self.time_sums[duration] = 0
        self.last_feature = None
        self.segment_id += 1


class FeatureEngine:
    """Stateful, streaming feature calculator with explicit reset boundaries."""

    def __init__(self) -> None:
        self._symbols: dict[str, _SymbolOfiState] = {}
        self._last_global_sequence: int | None = None

    def reset_all(self) -> None:
        for state in self._symbols.values():
            state.reset()

    def sample(self, symbol: str, timestamp_ns: int) -> dict[str, Any] | None:
        """Sample the last valid state at a clock time before the next event is applied."""

        state = self._symbols.get(symbol)
        if state is None or not state.valid or state.last_feature is None:
            return None
        if int(state.last_feature["anchor_timestamp_ns"]) > timestamp_ns:
            raise ValueError("clock sample precedes the last applied event")
        result = dict(state.last_feature)
        result["sample_timestamp_ns"] = timestamp_ns
        for duration, suffix in zip(TIME_WINDOWS_NS, ("10ms", "100ms", "1s"), strict=True):
            queue = state.time_queues[duration]
            lower_bound = timestamp_ns - duration
            while queue and queue[0][0] <= lower_bound:
                state.time_sums[duration] -= queue.popleft()[2]
            result[f"ofi_l1_{suffix}"] = None
            if timestamp_ns - state.segment_start_ns >= duration:
                result[f"ofi_l1_{suffix}"] = state.time_sums[duration]
        return result

    def accept(self, event: dict[str, Any]) -> dict[str, Any]:
        sequence = int(event["sequence"])
        if self._last_global_sequence is not None and sequence != self._last_global_sequence + 1:
            self.reset_all()
        self._last_global_sequence = sequence

        symbol = str(event["symbol"])
        state = self._symbols.setdefault(symbol, _SymbolOfiState())
        session_date = str(event["session_date"])
        if state.session_date and state.session_date != session_date:
            state.reset()
        state.session_date = session_date

        base: dict[str, Any] = {
            "session_date": session_date,
            "symbol": symbol,
            "anchor_sequence": sequence,
            "anchor_timestamp_ns": int(event["timestamp_ns"]),
            "segment_id": state.segment_id,
            **static_book_features(event),
            "ofi_l1_events_10": None,
            "ofi_l1_events_50": None,
            "ofi_l1_events_100": None,
            "ofi_l1_10ms": None,
            "ofi_l1_100ms": None,
            "ofi_l1_1s": None,
            "max_feature_sequence": sequence,
        }
        if not is_valid_continuous_book(event):
            if state.valid or state.previous is not None:
                state.reset()
            base["segment_id"] = state.segment_id
            return base

        current = L1State(
            bid=int(event["bids"][0]["price4"]),
            bid_quantity=int(event["bids"][0]["aggregate_qty"]),
            ask=int(event["asks"][0]["price4"]),
            ask_quantity=int(event["asks"][0]["aggregate_qty"]),
        )
        timestamp = int(event["timestamp_ns"])
        if not state.valid or state.previous is None:
            state.valid = True
            state.previous = current
            state.segment_start_ns = timestamp
            base["segment_id"] = state.segment_id
            state.last_feature = dict(base)
            return base

        previous = state.previous
        increment = cont_ofi_increment(
            previous.bid,
            previous.bid_quantity,
            previous.ask,
            previous.ask_quantity,
            current.bid,
            current.bid_quantity,
            current.ask,
            current.ask_quantity,
        )
        state.previous = current
        for window in EVENT_WINDOWS:
            queue = state.event_queues[window]
            queue.append(increment)
            state.event_sums[window] += increment
            if len(queue) > window:
                state.event_sums[window] -= queue.popleft()
            if len(queue) >= window:
                base[f"ofi_l1_events_{window}"] = state.event_sums[window]
        for duration, suffix in zip(TIME_WINDOWS_NS, ("10ms", "100ms", "1s"), strict=True):
            time_queue = state.time_queues[duration]
            time_queue.append((timestamp, sequence, increment))
            state.time_sums[duration] += increment
            lower_bound = timestamp - duration
            while time_queue and time_queue[0][0] <= lower_bound:
                state.time_sums[duration] -= time_queue.popleft()[2]
            if timestamp - state.segment_start_ns >= duration:
                base[f"ofi_l1_{suffix}"] = state.time_sums[duration]
        base["segment_id"] = state.segment_id
        state.last_feature = dict(base)
        return base


def cont_ofi_vectorized(
    bid: npt.NDArray[np.int64],
    bid_quantity: npt.NDArray[np.int64],
    ask: npt.NDArray[np.int64],
    ask_quantity: npt.NDArray[np.int64],
) -> npt.NDArray[np.int64]:
    """Vectorized scalar-oracle-equivalent OFI increments; first value is zero."""

    if not (bid.shape == bid_quantity.shape == ask.shape == ask_quantity.shape):
        raise ValueError("L1 input arrays must have identical shapes")
    output = np.zeros(bid.shape, dtype=np.int64)
    if bid.size < 2:
        return output
    output[1:] = (
        np.where(bid[1:] >= bid[:-1], bid_quantity[1:], 0)
        - np.where(bid[1:] <= bid[:-1], bid_quantity[:-1], 0)
        - np.where(ask[1:] <= ask[:-1], ask_quantity[1:], 0)
        + np.where(ask[1:] >= ask[:-1], ask_quantity[:-1], 0)
    )
    return output


def static_features_vectorized(
    bid_prices: npt.NDArray[np.int64],
    ask_prices: npt.NDArray[np.int64],
    bid_quantities: npt.NDArray[np.int64],
    ask_quantities: npt.NDArray[np.int64],
) -> dict[str, npt.NDArray[np.int64] | npt.NDArray[np.float64]]:
    """Vectorized feature transform for dense valid books of equal depth."""

    if not (bid_prices.shape == ask_prices.shape == bid_quantities.shape == ask_quantities.shape):
        raise ValueError("book arrays must have identical [row, depth] shapes")
    if bid_prices.ndim != 2 or bid_prices.shape[1] < 1:
        raise ValueError("book arrays require at least one depth level")
    bid = bid_prices[:, 0]
    ask = ask_prices[:, 0]
    bid_quantity = bid_quantities[:, 0]
    ask_quantity = ask_quantities[:, 0]
    denominator = bid_quantity + ask_quantity
    result: dict[str, npt.NDArray[np.int64] | npt.NDArray[np.float64]] = {
        "mid2": bid + ask,
        "spread_price4": ask - bid,
        "imbalance_l1": (bid_quantity - ask_quantity) / denominator,
        "weighted_midprice_l1": (ask * bid_quantity + bid * ask_quantity) / denominator,
    }
    for depth in (5, 10):
        if bid_prices.shape[1] >= depth:
            bid_sum = bid_quantities[:, :depth].sum(axis=1)
            ask_sum = ask_quantities[:, :depth].sum(axis=1)
            result[f"imbalance_l{depth}"] = (bid_sum - ask_sum) / (bid_sum + ask_sum)
    return result
