"""Leakage-safe event-time labels and right-continuous clock-time sampling."""

from __future__ import annotations

from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

import pyarrow as pa

from .features import FeatureEngine

EVENT_HORIZONS = (1, 10, 50, 100)
CLOCK_GRIDS = (
    (10_000_000, "10ms"),
    (100_000_000, "100ms"),
    (1_000_000_000, "1s"),
)

FEATURE_COLUMNS = (
    "mid2",
    "spread_price4",
    "imbalance_l1",
    "imbalance_l5",
    "imbalance_l10",
    "weighted_midprice_l1",
    "weighted_midprice_displacement",
    "ofi_l1_events_10",
    "ofi_l1_events_50",
    "ofi_l1_events_100",
    "ofi_l1_10ms",
    "ofi_l1_100ms",
    "ofi_l1_1s",
)


def direction(delta: int) -> int:
    return (delta > 0) - (delta < 0)


def _empty_targets() -> dict[str, int | None]:
    result: dict[str, int | None] = {}
    for horizon in EVENT_HORIZONS:
        result[f"target_mid2_delta_event_{horizon}"] = None
        result[f"target_direction_event_{horizon}"] = None
    for _, suffix in CLOCK_GRIDS:
        result[f"target_mid2_delta_clock_{suffix}"] = None
        result[f"target_direction_clock_{suffix}"] = None
    return result


def research_row(
    feature: dict[str, Any], sample_kind: str, grid_ns: int | None = None
) -> dict[str, Any]:
    sample_timestamp = int(feature.get("sample_timestamp_ns", feature["anchor_timestamp_ns"]))
    source_timestamp = int(feature["anchor_timestamp_ns"])
    row: dict[str, Any] = {
        "session_date": feature["session_date"],
        "symbol": feature["symbol"],
        "sample_kind": sample_kind,
        "grid_ns": grid_ns,
        "anchor_sequence": int(feature["anchor_sequence"]),
        "anchor_timestamp_ns": sample_timestamp,
        "source_state_timestamp_ns": source_timestamp,
        "segment_id": int(feature["segment_id"]),
        "max_feature_sequence": int(feature["max_feature_sequence"]),
    }
    row.update({column: feature.get(column) for column in FEATURE_COLUMNS})
    row.update(
        {
            "stoikov_microprice": None,
            "stoikov_microprice_displacement": None,
            "stoikov_fallback_reason": None,
        }
    )
    row.update(_empty_targets())
    return row


@dataclass(slots=True)
class _EventState:
    segment_id: int | None = None
    pending: deque[dict[str, Any]] = field(default_factory=deque)


class EventTimeLabeler:
    """Attach h-next-valid-mutation labels without crossing reset boundaries."""

    def __init__(self) -> None:
        self._states: dict[str, _EventState] = {}

    @staticmethod
    def _flush(state: _EventState) -> list[dict[str, Any]]:
        output = list(state.pending)
        state.pending.clear()
        return output

    def accept(self, feature: dict[str, Any]) -> list[dict[str, Any]]:
        symbol = str(feature["symbol"])
        state = self._states.setdefault(symbol, _EventState())
        if feature["mid2"] is None:
            state.segment_id = None
            return self._flush(state)
        segment = int(feature["segment_id"])
        output: list[dict[str, Any]] = []
        if state.segment_id is not None and state.segment_id != segment:
            output.extend(self._flush(state))
        state.segment_id = segment
        current_mid2 = int(feature["mid2"])
        for horizon in EVENT_HORIZONS:
            if len(state.pending) >= horizon:
                anchor = state.pending[-horizon]
                delta = current_mid2 - int(anchor["mid2"])
                anchor[f"target_mid2_delta_event_{horizon}"] = delta
                anchor[f"target_direction_event_{horizon}"] = direction(delta)
        state.pending.append(research_row(feature, "event"))
        if len(state.pending) > max(EVENT_HORIZONS):
            output.append(state.pending.popleft())
        return output

    def finish(self) -> list[dict[str, Any]]:
        output: list[dict[str, Any]] = []
        for symbol in sorted(self._states):
            output.extend(self._flush(self._states[symbol]))
        return output


@dataclass(slots=True)
class _ClockSeries:
    grid_ns: int
    suffix: str
    next_grid_ns: int | None = None
    segment_id: int | None = None
    pending: dict[str, Any] | None = None

    def boundary(self) -> list[dict[str, Any]]:
        output = [] if self.pending is None else [self.pending]
        self.pending = None
        self.segment_id = None
        return output

    def sample(self, feature: dict[str, Any] | None) -> list[dict[str, Any]]:
        if feature is None or feature["mid2"] is None:
            return self.boundary()
        segment = int(feature["segment_id"])
        output: list[dict[str, Any]] = []
        if self.segment_id is not None and segment != self.segment_id:
            output.extend(self.boundary())
        self.segment_id = segment
        if self.pending is not None:
            delta = int(feature["mid2"]) - int(self.pending["mid2"])
            self.pending[f"target_mid2_delta_clock_{self.suffix}"] = delta
            self.pending[f"target_direction_clock_{self.suffix}"] = direction(delta)
            output.append(self.pending)
        self.pending = research_row(feature, f"clock_{self.suffix}", self.grid_ns)
        return output


class ClockTimeLabeler:
    """Create fixed grids using the last state with (timestamp, sequence) <= grid time."""

    def __init__(self, grids: tuple[tuple[int, str], ...] = CLOCK_GRIDS) -> None:
        self._series: dict[tuple[str, int], _ClockSeries] = {}
        self._grids = grids
        self._last_timestamp_ns: int | None = None

    def _register(self, symbol: str, timestamp_ns: int) -> None:
        for grid_ns, suffix in self._grids:
            key = (symbol, grid_ns)
            if key not in self._series:
                next_grid = ((timestamp_ns + grid_ns - 1) // grid_ns) * grid_ns
                self._series[key] = _ClockSeries(grid_ns, suffix, next_grid)

    def before_event(
        self,
        timestamp_ns: int,
        engine: FeatureEngine,
        emit: Callable[[dict[str, Any]], None],
    ) -> None:
        """Finalize grid points strictly before a newly arriving feed timestamp."""

        if self._last_timestamp_ns is not None and timestamp_ns < self._last_timestamp_ns:
            raise ValueError("feed timestamp moved backwards")
        if self._last_timestamp_ns == timestamp_ns:
            return
        while True:
            eligible = [
                series.next_grid_ns
                for series in self._series.values()
                if series.next_grid_ns is not None and series.next_grid_ns < timestamp_ns
            ]
            if not eligible:
                break
            next_timestamp = min(eligible)
            for (symbol, _), series in sorted(self._series.items()):
                if series.next_grid_ns != next_timestamp:
                    continue
                feature = engine.sample(symbol, series.next_grid_ns)
                for row in series.sample(feature):
                    emit(row)
                series.next_grid_ns += series.grid_ns
        self._last_timestamp_ns = timestamp_ns

    def after_event(self, feature: dict[str, Any], engine: FeatureEngine) -> list[dict[str, Any]]:
        symbol = str(feature["symbol"])
        timestamp = int(feature["anchor_timestamp_ns"])
        self._register(symbol, timestamp)
        if feature["mid2"] is not None:
            return []
        output: list[dict[str, Any]] = []
        for grid_ns, _ in self._grids:
            output.extend(self._series[(symbol, grid_ns)].boundary())
        return output

    def finish(self, engine: FeatureEngine) -> list[dict[str, Any]]:
        output: list[dict[str, Any]] = []
        if self._last_timestamp_ns is not None:
            while True:
                eligible = [
                    series.next_grid_ns
                    for series in self._series.values()
                    if series.next_grid_ns is not None
                    and series.next_grid_ns <= self._last_timestamp_ns
                ]
                if not eligible:
                    break
                next_timestamp = min(eligible)
                for (symbol, _), series in sorted(self._series.items()):
                    if series.next_grid_ns != next_timestamp:
                        continue
                    output.extend(series.sample(engine.sample(symbol, series.next_grid_ns)))
                    series.next_grid_ns += series.grid_ns
        for key in sorted(self._series):
            output.extend(self._series[key].boundary())
        return output


class ResearchStreamBuilder:
    """Compose clock sampling, feature state, and future-label queues."""

    def __init__(self) -> None:
        self.features = FeatureEngine()
        self.events = EventTimeLabeler()
        self.clocks = ClockTimeLabeler()

    def accept(
        self,
        event: dict[str, Any],
        emit: Callable[[dict[str, Any]], None] | None = None,
    ) -> list[dict[str, Any]]:
        timestamp = int(event["timestamp_ns"])
        output: list[dict[str, Any]] = []
        sink = output.append if emit is None else emit
        self.clocks.before_event(timestamp, self.features, sink)
        feature = self.features.accept(event)
        for row in self.events.accept(feature):
            sink(row)
        for row in self.clocks.after_event(feature, self.features):
            sink(row)
        return output

    def finish(self, emit: Callable[[dict[str, Any]], None] | None = None) -> list[dict[str, Any]]:
        output: list[dict[str, Any]] = []
        sink = output.append if emit is None else emit
        for row in self.clocks.finish(self.features):
            sink(row)
        for row in self.events.finish():
            sink(row)
        return output


RESEARCH_SCHEMA = pa.schema(
    [
        pa.field("session_date", pa.string(), False),
        pa.field("symbol", pa.string(), False),
        pa.field("sample_kind", pa.string(), False),
        pa.field("grid_ns", pa.uint64(), True),
        pa.field("anchor_sequence", pa.uint64(), False),
        pa.field("anchor_timestamp_ns", pa.uint64(), False),
        pa.field("source_state_timestamp_ns", pa.uint64(), False),
        pa.field("segment_id", pa.uint64(), False),
        pa.field("max_feature_sequence", pa.uint64(), False),
        pa.field("mid2", pa.int64(), False),
        pa.field("spread_price4", pa.int64(), False),
        pa.field("imbalance_l1", pa.float64(), True),
        pa.field("imbalance_l5", pa.float64(), True),
        pa.field("imbalance_l10", pa.float64(), True),
        pa.field("weighted_midprice_l1", pa.float64(), True),
        pa.field("weighted_midprice_displacement", pa.float64(), True),
        pa.field("ofi_l1_events_10", pa.int64(), True),
        pa.field("ofi_l1_events_50", pa.int64(), True),
        pa.field("ofi_l1_events_100", pa.int64(), True),
        pa.field("ofi_l1_10ms", pa.int64(), True),
        pa.field("ofi_l1_100ms", pa.int64(), True),
        pa.field("ofi_l1_1s", pa.int64(), True),
        pa.field("stoikov_microprice", pa.float64(), True),
        pa.field("stoikov_microprice_displacement", pa.float64(), True),
        pa.field("stoikov_fallback_reason", pa.string(), True),
        *[
            field
            for horizon in EVENT_HORIZONS
            for field in (
                pa.field(f"target_mid2_delta_event_{horizon}", pa.int64(), True),
                pa.field(f"target_direction_event_{horizon}", pa.int8(), True),
            )
        ],
        *[
            field
            for _, suffix in CLOCK_GRIDS
            for field in (
                pa.field(f"target_mid2_delta_clock_{suffix}", pa.int64(), True),
                pa.field(f"target_direction_clock_{suffix}", pa.int8(), True),
            )
        ],
    ]
)
