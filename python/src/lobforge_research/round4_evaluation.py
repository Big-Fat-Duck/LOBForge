"""Round 4 artifact validation, metrics, sensitivities, and day-block inference."""

from __future__ import annotations

import json
from collections.abc import Callable, Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

import numpy as np

from .canonical import LogicalDigest

SCHEMA_VERSIONS = {
    "lobforge.shadow_order": 1,
    "lobforge.shadow_fill": 1,
    "lobforge.inventory_event": 1,
    "lobforge.mm_summary": 1,
}

ORDER_STATES = {
    "submitted",
    "pending_ack",
    "active",
    "pending_cancel",
    "pending_replace",
    "partially_filled",
    "filled",
    "rejected",
    "cancelled",
    "session_ended",
    "risk_cancelled",
}


def _integer(value: object, *, minimum: int | None = None) -> bool:
    return type(value) is int and (minimum is None or value >= minimum)


def _required(row: Mapping[str, Any], key: str, line_number: int) -> Any:
    if key not in row:
        raise ValueError(f"missing field {key} line {line_number}")
    return row[key]


def _validate_audit_row(row: Mapping[str, Any], schema: str, line_number: int) -> None:
    for key in ("timestamp_ns", "local_sequence", "order_id"):
        if not _integer(_required(row, key, line_number), minimum=1):
            raise ValueError(f"invalid {key} line {line_number}")
    if schema == "lobforge.shadow_order":
        for key in ("price4", "quantity", "remaining_quantity", "queue_ahead_quantity"):
            if not _integer(_required(row, key, line_number), minimum=0):
                raise ValueError(f"invalid {key} line {line_number}")
        if row["price4"] <= 0 or row["remaining_quantity"] > row["quantity"]:
            raise ValueError(f"invalid order range line {line_number}")
        for key in ("symbol", "reason"):
            if not isinstance(_required(row, key, line_number), str) or not row[key]:
                raise ValueError(f"invalid {key} line {line_number}")
        if _required(row, "side", line_number) not in {"buy", "sell"}:
            raise ValueError(f"invalid side line {line_number}")
        for key in ("from_state", "to_state"):
            if _required(row, key, line_number) not in ORDER_STATES:
                raise ValueError(f"invalid {key} line {line_number}")
        return
    if schema == "lobforge.shadow_fill":
        for key in (
            "factual_sequence",
            "quantity",
            "accounting_price4",
            "factual_display_price4",
            "fee_nanos",
            "rebate_nanos",
        ):
            if not _integer(_required(row, key, line_number), minimum=0):
                raise ValueError(f"invalid {key} line {line_number}")
        if (
            row["quantity"] <= 0
            or row["accounting_price4"] <= 0
            or row["factual_display_price4"] <= 0
        ):
            raise ValueError(f"invalid fill range line {line_number}")
        for key in ("factual_execution_price4", "anchor_mid2", "match_number"):
            value = _required(row, key, line_number)
            if value is not None and not _integer(value, minimum=0):
                raise ValueError(f"invalid {key} line {line_number}")
        for key in ("symbol", "reason"):
            if not isinstance(_required(row, key, line_number), str) or not row[key]:
                raise ValueError(f"invalid {key} line {line_number}")
        if _required(row, "side", line_number) not in {"buy", "sell"}:
            raise ValueError(f"invalid side line {line_number}")
        return
    for key in (
        "inventory",
        "trade_cash_nanos",
        "fees_nanos",
        "rebates_nanos",
        "realized_gross_pnl_nanos",
    ):
        if not _integer(_required(row, key, line_number)):
            raise ValueError(f"invalid {key} line {line_number}")
    for key in (
        "gross_equity_nanos",
        "net_equity_nanos",
        "conservative_liquidation_equity_nanos",
    ):
        value = _required(row, key, line_number)
        if value is not None and not _integer(value):
            raise ValueError(f"invalid {key} line {line_number}")


def read_strict_ndjson(path: Path, expected_schema: str) -> list[dict[str, Any]]:
    """Read without skipping malformed, blank, wrong-schema, or wrong-version rows."""

    if expected_schema not in SCHEMA_VERSIONS:
        raise ValueError("unsupported Round 4 schema")
    rows: list[dict[str, Any]] = []
    previous_key: tuple[int, int] | None = None
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            if not line.endswith("\n") or not line.strip():
                raise ValueError(f"invalid NDJSON line {line_number}")
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"invalid JSON line {line_number}") from error
            if not isinstance(value, dict):
                raise ValueError(f"non-object JSON line {line_number}")
            if value.get("schema") != expected_schema:
                raise ValueError(f"schema mismatch line {line_number}")
            if value.get("version") != SCHEMA_VERSIONS[expected_schema]:
                raise ValueError(f"version mismatch line {line_number}")
            _validate_audit_row(value, expected_schema, line_number)
            key = (int(value["timestamp_ns"]), int(value["local_sequence"]))
            if previous_key is not None and key <= previous_key:
                raise ValueError(f"audit order regression line {line_number}")
            previous_key = key
            rows.append(value)
    return rows


def artifact_logical_digest(rows: Iterable[Mapping[str, Any]]) -> str:
    digest = LogicalDigest()
    for row in rows:
        digest.update(row)
    return digest.hexdigest()


def validate_artifact_directory(path: Path) -> dict[str, Any]:
    manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
    summary = json.loads((path / "summary.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != "lobforge.mm_manifest" or manifest.get("version") != 1:
        raise ValueError("manifest schema/version mismatch")
    if summary.get("schema") != "lobforge.mm_summary" or summary.get("version") != 1:
        raise ValueError("summary schema/version mismatch")
    if manifest.get("protocol_sha256") != summary.get("protocol_sha256"):
        raise ValueError("protocol digest mismatch")
    if manifest.get("semantic_digest") != summary.get("semantic_digest"):
        raise ValueError("semantic digest mismatch")
    protocol = manifest.get("protocol_sha256")
    if not isinstance(protocol, str) or len(protocol) != 64:
        raise ValueError("protocol digest malformed")
    try:
        int(protocol, 16)
    except ValueError as error:
        raise ValueError("protocol digest malformed") from error
    semantic = manifest.get("semantic_digest")
    if not isinstance(semantic, str) or not semantic:
        raise ValueError("semantic digest malformed")
    orders = read_strict_ndjson(path / "shadow_orders.ndjson", "lobforge.shadow_order")
    fills = read_strict_ndjson(path / "shadow_fills.ndjson", "lobforge.shadow_fill")
    inventory = read_strict_ndjson(path / "inventory_events.ndjson", "lobforge.inventory_event")
    for field, actual in (
        ("order_rows", len(orders)),
        ("fill_rows", len(fills)),
        ("inventory_rows", len(inventory)),
    ):
        if not _integer(manifest.get(field), minimum=0) or manifest[field] != actual:
            raise ValueError(f"manifest {field} mismatch")
    return {
        "manifest": manifest,
        "summary": summary,
        "rows": {"orders": orders, "fills": fills, "inventory": inventory},
        "logical_digests": {
            "orders": artifact_logical_digest(orders),
            "fills": artifact_logical_digest(fills),
            "inventory": artifact_logical_digest(inventory),
        },
    }


def equal_day_block_bootstrap(
    values_by_day: Mapping[str, float],
    *,
    seed: int,
    repetitions: int,
    confidence: float = 0.95,
) -> tuple[float, float]:
    """Resample whole days; adjacent events are never treated as IID observations."""

    if repetitions <= 0 or not 0.0 < confidence < 1.0 or not values_by_day:
        raise ValueError("invalid block-bootstrap input")
    values = np.asarray([values_by_day[key] for key in sorted(values_by_day)], dtype=np.float64)
    random = np.random.default_rng(seed)
    samples = random.choice(values, size=(repetitions, values.size), replace=True).mean(axis=1)
    tail = (1.0 - confidence) / 2.0
    low, high = np.quantile(samples, [tail, 1.0 - tail])
    return float(low), float(high)


def sensitivity_table(
    summaries: Sequence[Mapping[str, Any]],
    *,
    dimensions: Sequence[str] = ("latency_ns", "queue_model", "fee_schedule"),
) -> list[dict[str, Any]]:
    """Return stable, explicitly keyed latency/queue/fee comparisons."""

    output: list[dict[str, Any]] = []
    for summary in summaries:
        missing = [
            key for key in (*dimensions, "net_pnl_nanos", "filled_quantity") if key not in summary
        ]
        if missing:
            raise ValueError(f"sensitivity row missing keys: {','.join(missing)}")
        output.append(
            {
                **{key: summary[key] for key in dimensions},
                "net_pnl_nanos": int(summary["net_pnl_nanos"]),
                "filled_quantity": int(summary["filled_quantity"]),
            }
        )
    return sorted(output, key=lambda row: tuple(str(row[key]) for key in dimensions))


def prefix_invariant(
    baseline: Sequence[Mapping[str, Any]],
    perturbed: Sequence[Mapping[str, Any]],
    *,
    cutoff_timestamp_ns: int,
    timestamp: Callable[[Mapping[str, Any]], int] = lambda row: int(row["timestamp_ns"]),
) -> bool:
    """Compare canonical causal outputs whose timestamps do not exceed the cutoff."""

    baseline_prefix = [row for row in baseline if timestamp(row) <= cutoff_timestamp_ns]
    perturbed_prefix = [row for row in perturbed if timestamp(row) <= cutoff_timestamp_ns]
    return artifact_logical_digest(baseline_prefix) == artifact_logical_digest(perturbed_prefix)
