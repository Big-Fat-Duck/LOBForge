"""Independent scalar oracles for the Round 4 C++ shadow simulator."""

from __future__ import annotations

import bisect
import math
from dataclasses import dataclass, field
from typing import Literal

Side = Literal["buy", "sell"]
QueueModel = Literal["mbo_fifo_conservative", "trade_through_only", "front_of_queue"]

NANOS_PER_PRICE4 = 100_000
NANOS_PER_MID2 = 50_000
INT64_MIN = -(2**63)
INT64_MAX = 2**63 - 1


@dataclass(frozen=True)
class QueueMutation:
    message_type: str
    side: Side
    order_ref: int
    quantity: int
    display_price4: int


@dataclass(frozen=True)
class FillEvidence:
    quantity: int
    reason: str | None


@dataclass
class QueueOracle:
    """Deliberately slow reference implementation using a plain ordered mapping."""

    model: QueueModel
    side: Side
    limit_price4: int
    ahead: dict[int, int] = field(default_factory=dict)

    @property
    def quantity_ahead(self) -> int:
        return sum(self.ahead.values())

    def apply(self, mutation: QueueMutation) -> FillEvidence:
        if mutation.quantity < 0:
            raise ValueError("mutation quantity must be non-negative")
        if mutation.side != self.side:
            return FillEvidence(0, None)
        execution = mutation.message_type in {"E", "C"}
        trade_through = (
            mutation.display_price4 < self.limit_price4
            if self.side == "buy"
            else mutation.display_price4 > self.limit_price4
        )
        if execution and trade_through:
            return FillEvidence(mutation.quantity, "trade_through")
        if mutation.display_price4 != self.limit_price4 and mutation.message_type != "U":
            return FillEvidence(0, None)
        if self.model == "trade_through_only":
            return FillEvidence(0, None)
        if self.model == "front_of_queue":
            if execution and mutation.display_price4 == self.limit_price4:
                return FillEvidence(mutation.quantity, "factual_execution")
            return FillEvidence(0, None)
        if mutation.order_ref in self.ahead:
            if mutation.message_type in {"D", "U"}:
                del self.ahead[mutation.order_ref]
            elif mutation.message_type in {"E", "C", "X"}:
                remaining = max(0, self.ahead[mutation.order_ref] - mutation.quantity)
                if remaining:
                    self.ahead[mutation.order_ref] = remaining
                else:
                    del self.ahead[mutation.order_ref]
            return FillEvidence(0, None)
        if execution and mutation.display_price4 == self.limit_price4 and not self.ahead:
            return FillEvidence(mutation.quantity, "factual_execution")
        return FillEvidence(0, None)


@dataclass(frozen=True)
class AccountingState:
    inventory: int
    trade_cash_nanos: int
    fees_nanos: int
    rebates_nanos: int
    turnover_quantity: int


def apply_accounting_fill(
    state: AccountingState,
    *,
    side: Side,
    quantity: int,
    price4: int,
    fee_nanos_per_share: int,
    rebate_nanos_per_share: int,
) -> AccountingState:
    """Exact integer cash/inventory oracle; rejects values outside signed int64."""

    if quantity <= 0 or price4 < 0 or fee_nanos_per_share < 0 or rebate_nanos_per_share < 0:
        raise ValueError("invalid accounting input")
    signed_quantity = quantity if side == "buy" else -quantity
    trade_value = price4 * NANOS_PER_PRICE4 * quantity
    cash_delta = -trade_value if side == "buy" else trade_value
    result = AccountingState(
        inventory=state.inventory + signed_quantity,
        trade_cash_nanos=state.trade_cash_nanos + cash_delta,
        fees_nanos=state.fees_nanos + fee_nanos_per_share * quantity,
        rebates_nanos=state.rebates_nanos + rebate_nanos_per_share * quantity,
        turnover_quantity=state.turnover_quantity + quantity,
    )
    for value in (
        result.inventory,
        result.trade_cash_nanos,
        result.fees_nanos,
        result.rebates_nanos,
    ):
        if not INT64_MIN <= value <= INT64_MAX:
            raise OverflowError("accounting result exceeds signed int64")
    return result


def gross_equity_at_mid2(state: AccountingState, mid2: int) -> int:
    if mid2 < 0:
        raise ValueError("mid2 must be non-negative")
    result = state.trade_cash_nanos + state.inventory * mid2 * NANOS_PER_MID2
    if not INT64_MIN <= result <= INT64_MAX:
        raise OverflowError("equity exceeds signed int64")
    return result


def net_equity_at_mid2(state: AccountingState, mid2: int) -> int:
    return gross_equity_at_mid2(state, mid2) - state.fees_nanos + state.rebates_nanos


@dataclass(frozen=True)
class ScalarQuote:
    bid_price4: int | None
    ask_price4: int | None
    reservation_price4: float | None
    total_spread_price4: float | None


def strategy_quote_scalar(
    *,
    kind: Literal["symmetric_quote", "avellaneda_stoikov", "signal_aware_as"],
    timestamp_ns: int,
    session_end_ns: int,
    inventory: int,
    bids: list[int],
    asks: list[int],
    tick_size_price4: int,
    maximum_distance_ticks: int,
    symmetric_half_spread_price4: float,
    gamma: float,
    sigma_squared: float,
    arrival_intensity_k: float,
    causal_signal: float = 0.0,
    signal_coefficient_price4: float = 0.0,
) -> ScalarQuote:
    """Independent scalar implementation of the frozen strategy equations."""

    if not bids or not asks or bids[0] >= asks[0] or tick_size_price4 <= 0:
        return ScalarQuote(None, None, None, None)
    if timestamp_ns >= session_end_ns:
        return ScalarQuote(None, None, None, None)
    mid = (bids[0] + asks[0]) / 2.0
    reservation = mid
    total_spread = 2.0 * symmetric_half_spread_price4
    if kind != "symmetric_quote":
        if (
            gamma <= 0.0
            or arrival_intensity_k <= 0.0
            or sigma_squared < 0.0
            or not all(
                math.isfinite(value) for value in (gamma, arrival_intensity_k, sigma_squared)
            )
        ):
            return ScalarQuote(None, None, None, None)
        remaining = (session_end_ns - timestamp_ns) / 1_000_000_000.0
        reservation -= inventory * gamma * sigma_squared * remaining
        if kind == "signal_aware_as":
            reservation += signal_coefficient_price4 * causal_signal
        liquidity = (
            2.0 / arrival_intensity_k
            if gamma < 1e-9
            else 2.0 / gamma * math.log1p(gamma / arrival_intensity_k)
        )
        total_spread = gamma * sigma_squared * remaining + liquidity
    max_distance = maximum_distance_ticks * tick_size_price4
    rounded_bid = (
        math.floor((reservation - total_spread / 2.0) / tick_size_price4) * tick_size_price4
    )
    rounded_ask = (
        math.ceil((reservation + total_spread / 2.0) / tick_size_price4) * tick_size_price4
    )
    bid = next(
        (price for price in bids if price <= rounded_bid and bids[0] - price <= max_distance),
        None,
    )
    ask = next(
        (price for price in asks if price >= rounded_ask and price - asks[0] <= max_distance),
        None,
    )
    if bid is not None and bid >= asks[0]:
        bid = None
    if ask is not None and ask <= bids[0]:
        ask = None
    return ScalarQuote(bid, ask, reservation, total_spread)


@dataclass(frozen=True, order=True)
class MarketPoint:
    timestamp_ns: int
    factual_sequence: int
    valid: bool
    mid2: int


def right_continuous_markout(
    points: list[MarketPoint],
    *,
    fill_timestamp_ns: int,
    horizon_ns: int,
    side: Side,
    fill_price4: int,
    quantity: int,
) -> int | None:
    """Select the last valid state at or before fill+horizon; never look after it."""

    if horizon_ns < 0 or quantity <= 0:
        raise ValueError("invalid markout input")
    keys = [(point.timestamp_ns, point.factual_sequence) for point in points]
    due = fill_timestamp_ns + horizon_ns
    index = bisect.bisect_right(keys, (due, 2**64 - 1)) - 1
    if index < 0 or not points[index].valid:
        return None
    direction = 1 if side == "buy" else -1
    delta_mid2 = points[index].mid2 - 2 * fill_price4
    return direction * delta_mid2 * NANOS_PER_MID2 * quantity
