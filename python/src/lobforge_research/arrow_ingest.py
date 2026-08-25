"""Vectorized strict validation for high-throughput book_event/v1 NDJSON batches."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc

from .schema import BOOK_EVENT_SCHEMA, DAY_NS

WIRE_DEPTH = pa.list_(pa.list_(pa.int64()))
WIRE_SCHEMA = pa.schema(
    [
        pa.field("record_type", pa.string()),
        pa.field("schema", pa.string()),
        pa.field("version", pa.int32()),
        pa.field("session_date", pa.string()),
        pa.field("price_scale", pa.int32()),
        pa.field("timestamp_unit", pa.string()),
        pa.field("depth", pa.int32()),
        pa.field("source_size", pa.uint64()),
        pa.field("sequence", pa.uint64()),
        pa.field("source_offset", pa.uint64()),
        pa.field("timestamp_ns", pa.uint64()),
        pa.field("stock_locate", pa.uint16()),
        pa.field("symbol", pa.string()),
        pa.field("message_type", pa.string()),
        pa.field("action", pa.string()),
        pa.field("side", pa.string()),
        pa.field("order_ref", pa.uint64()),
        pa.field("new_order_ref", pa.uint64()),
        pa.field("event_qty", pa.uint64()),
        pa.field("display_price4", pa.int64()),
        pa.field("execution_price4", pa.int64()),
        pa.field("match_number", pa.uint64()),
        pa.field("session_state", pa.string()),
        pa.field("trading_state", pa.string()),
        pa.field("two_sided", pa.bool_()),
        pa.field("locked", pa.bool_()),
        pa.field("crossed", pa.bool_()),
        pa.field("bids", WIRE_DEPTH),
        pa.field("asks", WIRE_DEPTH),
        pa.field("input_bytes", pa.uint64()),
        pa.field("records_seen", pa.uint64()),
        pa.field("records_decoded", pa.uint64()),
        pa.field("records_applied", pa.uint64()),
        pa.field("records_output", pa.uint64()),
        pa.field("records_skipped", pa.uint64()),
        pa.field("errors", pa.uint64()),
        pa.field("factual_book_digest", pa.string()),
    ]
)

EVENT_REQUIRED = (
    "session_date",
    "sequence",
    "source_offset",
    "timestamp_ns",
    "stock_locate",
    "symbol",
    "message_type",
    "action",
    "side",
    "order_ref",
    "event_qty",
    "display_price4",
    "session_state",
    "two_sided",
    "locked",
    "crossed",
    "bids",
    "asks",
)
HEADER_FIELDS = {
    "record_type",
    "schema",
    "version",
    "session_date",
    "price_scale",
    "timestamp_unit",
    "depth",
    "source_size",
}
SUMMARY_FIELDS = {
    "record_type",
    "schema",
    "version",
    "input_bytes",
    "records_seen",
    "records_decoded",
    "records_applied",
    "records_output",
    "records_skipped",
    "errors",
    "factual_book_digest",
}
EVENT_FIELDS = {
    "record_type",
    *EVENT_REQUIRED,
    "new_order_ref",
    "execution_price4",
    "match_number",
    "trading_state",
}


def _indices(mask: pa.Array) -> np.ndarray[Any, np.dtype[np.int64]]:
    return np.asarray(pc.indices_nonzero(mask).to_numpy(zero_copy_only=False), dtype=np.int64)


def _column(batch: pa.RecordBatch, name: str) -> pa.Array:
    return batch.column(batch.schema.get_field_index(name))


def _all_equal(array: pa.Array, value: Any) -> bool:
    return bool(pc.all(pc.equal(array, pa.scalar(value, type=array.type))).as_py())


def _none_null(batch: pa.RecordBatch, names: tuple[str, ...]) -> None:
    for name in names:
        if _column(batch, name).null_count:
            raise ValueError(f"book_event {name} contains null")


def _reject_extra_values(row: dict[str, Any], allowed: set[str]) -> None:
    if any(value is not None for key, value in row.items() if key not in allowed):
        raise ValueError("record has a field that is not part of its record type")


def _normalize_depth(array: pa.Array, descending: bool, maximum_depth: int) -> pa.ListArray:
    outer = array
    assert isinstance(outer, pa.ListArray)
    offsets = np.asarray(outer.offsets.to_numpy(zero_copy_only=False), dtype=np.int64)
    lengths = np.diff(offsets)
    if np.any(lengths > maximum_depth):
        raise ValueError("depth exceeds header limit")
    inner = outer.values.slice(int(offsets[0]), int(offsets[-1] - offsets[0]))
    assert isinstance(inner, pa.ListArray)
    inner_offsets = np.asarray(inner.offsets.to_numpy(zero_copy_only=False), dtype=np.int64)
    if np.any(np.diff(inner_offsets) != 2):
        raise ValueError("depth level is not a [price4, aggregate_qty] pair")
    flat = np.asarray(inner.values.to_numpy(zero_copy_only=False), dtype=np.int64)
    prices = flat[0::2]
    quantities = flat[1::2]
    if np.any(prices <= 0) or np.any(prices > 2**32 - 1) or np.any(quantities <= 0):
        raise ValueError("depth price or quantity is outside its range")
    row_index = np.repeat(np.arange(lengths.size), lengths)
    if prices.size > 1:
        same_row = row_index[1:] == row_index[:-1]
        difference = prices[1:] - prices[:-1]
        bad = same_row & (difference >= 0 if descending else difference <= 0)
        if np.any(bad):
            raise ValueError("depth is not strictly best-to-worst")
    levels = pa.StructArray.from_arrays(
        [pa.array(prices, type=pa.int64()), pa.array(quantities, type=pa.uint64())],
        fields=list(BOOK_EVENT_SCHEMA.field("bids").type.value_type),
    )
    normalized_offsets = pa.array(offsets - offsets[0], type=pa.int32())
    return pa.ListArray.from_arrays(normalized_offsets, levels)


def _first_prices(
    depth: pa.ListArray, lengths: np.ndarray[Any, np.dtype[np.int64]]
) -> np.ndarray[Any, np.dtype[np.int64]]:
    offsets = np.asarray(depth.offsets.to_numpy(zero_copy_only=False), dtype=np.int64)
    price_values = np.asarray(
        depth.values.field("price4").to_numpy(zero_copy_only=False), dtype=np.int64
    )
    result = np.zeros(lengths.size, dtype=np.int64)
    present = lengths > 0
    result[present] = price_values[offsets[:-1][present]]
    return result


@dataclass(slots=True)
class ArrowStreamValidator:
    expected_session_date: str
    expected_source_size: int
    depth: int | None = None
    rows_seen: int = 0
    event_count: int = 0
    last_sequence: int = 0
    last_source_offset: int = -1
    last_timestamp: int = -1
    header_seen: bool = False
    summary: dict[str, Any] | None = None

    def accept(self, batch: pa.RecordBatch) -> pa.Table | None:
        record_type = _column(batch, "record_type")
        if record_type.null_count:
            raise ValueError("record_type contains null")
        header_indices = _indices(pc.equal(record_type, "header"))
        event_indices = _indices(pc.equal(record_type, "book_event"))
        summary_indices = _indices(pc.equal(record_type, "summary"))
        if header_indices.size + event_indices.size + summary_indices.size != batch.num_rows:
            raise ValueError("unknown record_type")
        if header_indices.size:
            if (
                self.header_seen
                or header_indices.size != 1
                or self.rows_seen + header_indices[0] != 0
            ):
                raise ValueError("header must be first and unique")
            self._header(batch.slice(int(header_indices[0]), 1).to_pylist()[0])
        if summary_indices.size and (
            self.summary is not None
            or summary_indices.size != 1
            or int(summary_indices[0]) != batch.num_rows - 1
        ):
            raise ValueError("summary must be unique and final")
        if self.summary is not None and event_indices.size:
            raise ValueError("book_event appears after summary")
        if summary_indices.size and np.any(event_indices > int(summary_indices[0])):
            raise ValueError("book_event appears after summary")
        normalized = None
        if event_indices.size:
            mask = pc.equal(record_type, "book_event")
            events = batch.filter(mask)
            normalized = self._events(events)
        if summary_indices.size:
            self._summary(batch.slice(int(summary_indices[0]), 1).to_pylist()[0])
        self.rows_seen += batch.num_rows
        return normalized

    def _header(self, row: dict[str, Any]) -> None:
        _reject_extra_values(row, HEADER_FIELDS)
        required = {
            "schema": "lobforge.book_event",
            "version": 1,
            "session_date": self.expected_session_date,
            "price_scale": 10_000,
            "timestamp_unit": "ns_since_midnight",
            "source_size": self.expected_source_size,
        }
        if any(row.get(key) != value for key, value in required.items()):
            raise ValueError("invalid book_event/v1 header")
        depth = row.get("depth")
        if not isinstance(depth, int) or not 1 <= depth <= 10:
            raise ValueError("invalid header depth")
        self.depth = depth
        self.header_seen = True

    def _summary(self, row: dict[str, Any]) -> None:
        if not self.header_seen:
            raise ValueError("summary appears before header")
        _reject_extra_values(row, SUMMARY_FIELDS)
        if any(row.get(field) is None for field in SUMMARY_FIELDS):
            raise ValueError("summary is missing a required field")
        if (
            row.get("schema") != "lobforge.book_event"
            or row.get("version") != 1
            or row.get("input_bytes") != self.expected_source_size
        ):
            raise ValueError("invalid summary schema or source size")
        digest = row.get("factual_book_digest")
        if (
            not isinstance(digest, str)
            or len(digest) != 16
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise ValueError("invalid factual book digest")
        self.summary = row

    def _events(self, events: pa.RecordBatch) -> pa.Table:
        if not self.header_seen or self.depth is None or self.summary is not None:
            raise ValueError("book_event is outside header/summary boundaries")
        _none_null(events, EVENT_REQUIRED)
        for field in WIRE_SCHEMA:
            if (
                field.name not in EVENT_FIELDS
                and _column(events, field.name).null_count != events.num_rows
            ):
                raise ValueError("book_event contains a non-event field")
        if not _all_equal(_column(events, "session_date"), self.expected_session_date):
            raise ValueError("event session date mismatch")
        sequence = np.asarray(
            _column(events, "sequence").to_numpy(zero_copy_only=False), dtype=np.uint64
        )
        expected = np.arange(
            self.last_sequence + 1,
            self.last_sequence + events.num_rows + 1,
            dtype=np.uint64,
        )
        if not np.array_equal(sequence, expected):
            raise ValueError("event sequence is not contiguous")
        offsets = np.asarray(
            _column(events, "source_offset").to_numpy(zero_copy_only=False), dtype=np.uint64
        )
        timestamps = np.asarray(
            _column(events, "timestamp_ns").to_numpy(zero_copy_only=False), dtype=np.uint64
        )
        if (
            (offsets.size and int(offsets[0]) <= self.last_source_offset)
            or np.any(offsets[1:] <= offsets[:-1])
            or (timestamps.size and int(timestamps[0]) < self.last_timestamp)
            or np.any(timestamps >= DAY_NS)
            or np.any(np.diff(timestamps.astype(np.int64)) < 0)
        ):
            raise ValueError("source offset or timestamp order/range violation")
        message_type = np.asarray(_column(events, "message_type").to_numpy(zero_copy_only=False))
        action = np.asarray(_column(events, "action").to_numpy(zero_copy_only=False))
        action_map = {
            "A": "add",
            "F": "add",
            "E": "execute",
            "C": "execute_with_price",
            "X": "cancel",
            "D": "delete",
            "U": "replace",
        }
        expected_actions = np.asarray([action_map.get(str(value), "") for value in message_type])
        if not np.array_equal(action, expected_actions):
            raise ValueError("message type/action mismatch")
        if np.any(
            ~np.isin(
                np.asarray(_column(events, "side").to_numpy(zero_copy_only=False)),
                ["B", "S"],
            )
        ):
            raise ValueError("invalid side")
        stock_locate = np.asarray(
            _column(events, "stock_locate").to_numpy(zero_copy_only=False), dtype=np.uint16
        )
        order_ref = np.asarray(
            _column(events, "order_ref").to_numpy(zero_copy_only=False), dtype=np.uint64
        )
        event_quantity = np.asarray(
            _column(events, "event_qty").to_numpy(zero_copy_only=False), dtype=np.uint64
        )
        display_price = np.asarray(
            _column(events, "display_price4").to_numpy(zero_copy_only=False), dtype=np.int64
        )
        symbols = np.asarray(_column(events, "symbol").to_numpy(zero_copy_only=False))
        trading = np.asarray(
            _column(events, "trading_state").fill_null("").to_numpy(zero_copy_only=False)
        )
        if (
            np.any(stock_locate == 0)
            or np.any(order_ref == 0)
            or np.any(event_quantity == 0)
            or np.any(display_price <= 0)
            or np.any(display_price > 2**32 - 1)
            or np.any(symbols == "")
            or np.any(~np.isin(trading, ["", "H", "P", "Q", "T"]))
        ):
            raise ValueError("event scalar field is outside its allowed range")
        new_ref_valid = np.asarray(
            _column(events, "new_order_ref").is_valid().to_numpy(zero_copy_only=False)
        )
        execution_valid = np.asarray(
            _column(events, "execution_price4").is_valid().to_numpy(zero_copy_only=False)
        )
        match_valid = np.asarray(
            _column(events, "match_number").is_valid().to_numpy(zero_copy_only=False)
        )
        if (
            not np.array_equal(new_ref_valid, message_type == "U")
            or not np.array_equal(execution_valid, message_type == "C")
            or not np.array_equal(match_valid, np.isin(message_type, ["E", "C"]))
        ):
            raise ValueError("mutation-field nullability mismatch")
        bids = _normalize_depth(_column(events, "bids"), True, self.depth)
        asks = _normalize_depth(_column(events, "asks"), False, self.depth)
        bid_lengths = np.asarray(
            pc.list_value_length(bids).to_numpy(zero_copy_only=False), dtype=np.int64
        )
        ask_lengths = np.asarray(
            pc.list_value_length(asks).to_numpy(zero_copy_only=False), dtype=np.int64
        )
        two_sided = (bid_lengths > 0) & (ask_lengths > 0)
        first_bid = _first_prices(bids, bid_lengths)
        first_ask = _first_prices(asks, ask_lengths)
        locked = two_sided & (first_bid == first_ask)
        crossed = two_sided & (first_bid > first_ask)
        if (
            not np.array_equal(
                two_sided, np.asarray(_column(events, "two_sided").to_numpy(zero_copy_only=False))
            )
            or not np.array_equal(
                locked, np.asarray(_column(events, "locked").to_numpy(zero_copy_only=False))
            )
            or not np.array_equal(
                crossed, np.asarray(_column(events, "crossed").to_numpy(zero_copy_only=False))
            )
        ):
            raise ValueError("book flags disagree with depth")
        arrays = []
        for field in BOOK_EVENT_SCHEMA:
            if field.name == "bids":
                arrays.append(bids)
            elif field.name == "asks":
                arrays.append(asks)
            else:
                arrays.append(pc.cast(_column(events, field.name), field.type, safe=True))
        self.event_count += events.num_rows
        self.last_sequence = int(sequence[-1])
        self.last_source_offset = int(offsets[-1])
        self.last_timestamp = int(timestamps[-1])
        return pa.Table.from_arrays(arrays, schema=BOOK_EVENT_SCHEMA)

    def finish(self) -> None:
        if not self.header_seen or self.summary is None:
            raise ValueError("stream is missing header or summary")
        if self.summary["records_output"] != self.event_count:
            raise ValueError("summary output count mismatch")
        if (
            self.summary["records_decoded"] > self.summary["records_seen"]
            or self.summary["records_applied"] > self.summary["records_decoded"]
        ):
            raise ValueError("summary counters are inconsistent")
