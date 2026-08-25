from __future__ import annotations

from typing import Any

import pytest


def make_event(
    *,
    sequence: int = 1,
    timestamp_ns: int = 1_000_000_000,
    symbol: str = "TEST",
    bid: int = 100,
    ask: int = 102,
    bid_quantity: int = 10,
    ask_quantity: int = 10,
    depth: int = 10,
    session_state: str = "market_hours",
    trading_state: str | None = "T",
    two_sided: bool = True,
    crossed: bool = False,
) -> dict[str, Any]:
    bids = [
        {"price4": bid - level, "aggregate_qty": bid_quantity + level} for level in range(depth)
    ]
    asks = [
        {"price4": ask + level, "aggregate_qty": ask_quantity + level} for level in range(depth)
    ]
    if not two_sided:
        asks = []
    return {
        "session_date": "2026-08-24",
        "sequence": sequence,
        "source_offset": sequence * 40,
        "timestamp_ns": timestamp_ns,
        "stock_locate": 1,
        "symbol": symbol,
        "message_type": "A",
        "action": "add",
        "side": "B",
        "order_ref": sequence,
        "new_order_ref": None,
        "event_qty": 1,
        "display_price4": bid,
        "execution_price4": None,
        "match_number": None,
        "session_state": session_state,
        "trading_state": trading_state,
        "two_sided": two_sided,
        "locked": two_sided and bid == ask,
        "crossed": crossed,
        "bids": bids,
        "asks": asks,
    }


@pytest.fixture
def event_factory():
    return make_event
