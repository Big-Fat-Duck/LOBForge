"""Strict book_event/v1 validation and fixed Arrow schemas."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

import pyarrow as pa

from .errors import SCHEMA_ERROR, ResearchError

SCHEMA_NAME = "lobforge.book_event"
SCHEMA_VERSION = 1
DAY_NS = 86_400_000_000_000
UINT32_MAX = 2**32 - 1
UINT64_MAX = 2**64 - 1
MUTATION_TYPES = {"A", "F", "E", "C", "X", "D", "U"}
ACTION_BY_TYPE = {
    "A": "add",
    "F": "add",
    "E": "execute",
    "C": "execute_with_price",
    "X": "cancel",
    "D": "delete",
    "U": "replace",
}

HEADER_KEYS = (
    "record_type",
    "schema",
    "version",
    "session_date",
    "price_scale",
    "timestamp_unit",
    "depth",
    "source_size",
)
BOOK_KEYS = (
    "record_type",
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
    "new_order_ref",
    "event_qty",
    "display_price4",
    "execution_price4",
    "match_number",
    "session_state",
    "trading_state",
    "two_sided",
    "locked",
    "crossed",
    "bids",
    "asks",
)
SUMMARY_KEYS = (
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
)

DEPTH_LEVEL_TYPE = pa.struct(
    [pa.field("price4", pa.int64(), nullable=False), pa.field("aggregate_qty", pa.uint64(), False)]
)
BOOK_EVENT_SCHEMA = pa.schema(
    [
        pa.field("session_date", pa.string(), False),
        pa.field("sequence", pa.uint64(), False),
        pa.field("source_offset", pa.uint64(), False),
        pa.field("timestamp_ns", pa.uint64(), False),
        pa.field("stock_locate", pa.uint16(), False),
        pa.field("symbol", pa.string(), False),
        pa.field("message_type", pa.string(), False),
        pa.field("action", pa.string(), False),
        pa.field("side", pa.string(), False),
        pa.field("order_ref", pa.uint64(), False),
        pa.field("new_order_ref", pa.uint64(), True),
        pa.field("event_qty", pa.uint64(), False),
        pa.field("display_price4", pa.int64(), False),
        pa.field("execution_price4", pa.int64(), True),
        pa.field("match_number", pa.uint64(), True),
        pa.field("session_state", pa.string(), False),
        pa.field("trading_state", pa.string(), True),
        pa.field("two_sided", pa.bool_(), False),
        pa.field("locked", pa.bool_(), False),
        pa.field("crossed", pa.bool_(), False),
        pa.field("bids", pa.list_(DEPTH_LEVEL_TYPE), False),
        pa.field("asks", pa.list_(DEPTH_LEVEL_TYPE), False),
    ]
)


def _fail(detail: str) -> ResearchError:
    return ResearchError(SCHEMA_ERROR, "SCHEMA_VIOLATION", detail)


def parse_json_line(line: str, line_number: int) -> dict[str, Any]:
    try:
        value = json.loads(line)
    except json.JSONDecodeError as error:
        raise _fail(f"line {line_number}: invalid JSON: {error.msg}") from error
    if not isinstance(value, dict):
        raise _fail(f"line {line_number}: record must be an object")
    return value


def _exact_keys(row: dict[str, Any], expected: tuple[str, ...], kind: str) -> None:
    if tuple(row) != expected:
        raise _fail(f"{kind}: keys or key order do not match schema")


def _integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise _fail(f"{name}: expected integer in [{minimum}, {maximum}]")
    return int(value)


def _nullable_integer(value: Any, name: str, maximum: int = UINT64_MAX) -> int | None:
    if value is None:
        return None
    return _integer(value, name, 0, maximum)


def _boolean(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        raise _fail(f"{name}: expected boolean")
    return value


def _depth(value: Any, name: str, maximum_depth: int, descending: bool) -> list[list[int]]:
    if not isinstance(value, list) or len(value) > maximum_depth:
        raise _fail(f"{name}: expected at most {maximum_depth} levels")
    previous: int | None = None
    result: list[list[int]] = []
    for index, level in enumerate(value):
        if not isinstance(level, list) or len(level) != 2:
            raise _fail(f"{name}[{index}]: expected [price4, aggregate_qty]")
        price = _integer(level[0], f"{name}[{index}].price4", 1, UINT32_MAX)
        quantity = _integer(level[1], f"{name}[{index}].aggregate_qty", 1, UINT64_MAX)
        if previous is not None and (
            (descending and price >= previous) or (not descending and price <= previous)
        ):
            raise _fail(f"{name}: levels are not strictly best-to-worst")
        previous = price
        result.append([price, quantity])
    return result


@dataclass(slots=True)
class StreamValidator:
    """Stateful strict validator for a single complete NDJSON stream."""

    expected_session_date: str
    expected_source_size: int
    header: dict[str, Any] | None = None
    summary: dict[str, Any] | None = None
    event_count: int = 0
    last_sequence: int = 0
    last_source_offset: int = -1
    last_timestamp_ns: int = -1

    def accept(self, row: dict[str, Any]) -> dict[str, Any] | None:
        record_type = row.get("record_type")
        if self.summary is not None:
            raise _fail("record appears after summary")
        if record_type == "header":
            self._accept_header(row)
            return None
        if record_type == "book_event":
            return self._accept_book_event(row)
        if record_type == "summary":
            self._accept_summary(row)
            return None
        raise _fail("unknown record_type")

    def _accept_header(self, row: dict[str, Any]) -> None:
        if self.header is not None or self.event_count != 0:
            raise _fail("header must be the first and only header")
        _exact_keys(row, HEADER_KEYS, "header")
        if row["schema"] != SCHEMA_NAME or row["version"] != SCHEMA_VERSION:
            raise _fail("unsupported schema or version")
        if row["session_date"] != self.expected_session_date:
            raise _fail("header session_date disagrees with requested date")
        if row["price_scale"] != 10_000 or row["timestamp_unit"] != "ns_since_midnight":
            raise _fail("unsupported price scale or timestamp unit")
        _integer(row["depth"], "depth", 1, 10)
        if row["source_size"] != self.expected_source_size:
            raise _fail("header source_size disagrees with source file")
        self.header = row

    def _accept_book_event(self, row: dict[str, Any]) -> dict[str, Any]:
        if self.header is None:
            raise _fail("book_event appears before header")
        _exact_keys(row, BOOK_KEYS, "book_event")
        if row["session_date"] != self.expected_session_date:
            raise _fail("book_event session_date mismatch")
        sequence = _integer(row["sequence"], "sequence", 1, UINT64_MAX)
        if sequence != self.last_sequence + 1:
            raise _fail("sequence is not contiguous and increasing")
        source_offset = _integer(row["source_offset"], "source_offset", 0, UINT64_MAX)
        if source_offset <= self.last_source_offset:
            raise _fail("source_offset did not increase")
        timestamp = _integer(row["timestamp_ns"], "timestamp_ns", 0, DAY_NS - 1)
        if timestamp < self.last_timestamp_ns:
            raise _fail("timestamp_ns moved backwards")
        _integer(row["stock_locate"], "stock_locate", 1, 65_535)
        if not isinstance(row["symbol"], str) or not row["symbol"]:
            raise _fail("symbol must be nonempty text")
        message_type = row["message_type"]
        if message_type not in MUTATION_TYPES or row["action"] != ACTION_BY_TYPE[message_type]:
            raise _fail("message_type/action mismatch")
        if row["side"] not in {"B", "S"}:
            raise _fail("side must be B or S")
        _integer(row["order_ref"], "order_ref", 1, UINT64_MAX)
        new_ref = _nullable_integer(row["new_order_ref"], "new_order_ref")
        if (message_type == "U") != (new_ref is not None):
            raise _fail("new_order_ref nullability disagrees with message type")
        _integer(row["event_qty"], "event_qty", 1, UINT64_MAX)
        _integer(row["display_price4"], "display_price4", 1, UINT32_MAX)
        execution = _nullable_integer(row["execution_price4"], "execution_price4", UINT32_MAX)
        if (message_type == "C") != (execution is not None):
            raise _fail("execution_price4 nullability disagrees with message type")
        match = _nullable_integer(row["match_number"], "match_number")
        if (message_type in {"E", "C"}) != (match is not None):
            raise _fail("match_number nullability disagrees with message type")
        if not isinstance(row["session_state"], str):
            raise _fail("session_state must be text")
        if row["trading_state"] is not None and row["trading_state"] not in {"H", "P", "Q", "T"}:
            raise _fail("trading_state is invalid")
        two_sided = _boolean(row["two_sided"], "two_sided")
        locked = _boolean(row["locked"], "locked")
        crossed = _boolean(row["crossed"], "crossed")
        bids = _depth(row["bids"], "bids", self.header["depth"], descending=True)
        asks = _depth(row["asks"], "asks", self.header["depth"], descending=False)
        calculated_two_sided = bool(bids and asks)
        calculated_locked = calculated_two_sided and bids[0][0] == asks[0][0]
        calculated_crossed = calculated_two_sided and bids[0][0] > asks[0][0]
        if (two_sided, locked, crossed) != (
            calculated_two_sided,
            calculated_locked,
            calculated_crossed,
        ):
            raise _fail("book-state flags disagree with depth")
        self.event_count += 1
        self.last_sequence = sequence
        self.last_source_offset = source_offset
        self.last_timestamp_ns = timestamp
        return normalize_book_event(row)

    def _accept_summary(self, row: dict[str, Any]) -> None:
        if self.header is None:
            raise _fail("summary appears before header")
        _exact_keys(row, SUMMARY_KEYS, "summary")
        if row["schema"] != SCHEMA_NAME or row["version"] != SCHEMA_VERSION:
            raise _fail("summary schema or version mismatch")
        if row["input_bytes"] != self.expected_source_size:
            raise _fail("summary input_bytes mismatch")
        for field in SUMMARY_KEYS[3:-1]:
            _integer(row[field], field, 0, UINT64_MAX)
        if row["records_output"] != self.event_count:
            raise _fail("summary records_output mismatch")
        if (
            row["records_decoded"] > row["records_seen"]
            or row["records_applied"] > row["records_decoded"]
        ):
            raise _fail("summary record counters are inconsistent")
        digest = row["factual_book_digest"]
        if (
            not isinstance(digest, str)
            or len(digest) != 16
            or any(c not in "0123456789abcdef" for c in digest)
        ):
            raise _fail("factual_book_digest must be 16 lowercase hexadecimal characters")
        self.summary = row

    def finish(self) -> None:
        if self.header is None:
            raise _fail("stream is missing header")
        if self.summary is None:
            raise _fail("stream is truncated or missing summary")


def normalize_book_event(row: dict[str, Any]) -> dict[str, Any]:
    """Convert wire depth pairs into the fixed Arrow logical representation."""

    normalized = {key: row[key] for key in BOOK_KEYS if key != "record_type"}
    normalized["bids"] = [
        {"price4": price, "aggregate_qty": quantity} for price, quantity in row["bids"]
    ]
    normalized["asks"] = [
        {"price4": price, "aggregate_qty": quantity} for price, quantity in row["asks"]
    ]
    return normalized
