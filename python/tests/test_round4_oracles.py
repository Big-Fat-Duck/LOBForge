from __future__ import annotations

import math

import pytest
from hypothesis import given
from hypothesis import strategies as st

from lobforge_research.round4_oracle import (
    AccountingState,
    MarketPoint,
    QueueMutation,
    QueueOracle,
    apply_accounting_fill,
    gross_equity_at_mid2,
    net_equity_at_mid2,
    right_continuous_markout,
    strategy_quote_scalar,
)


def test_queue_models_and_fifo_boundaries() -> None:
    primary = QueueOracle("mbo_fifo_conservative", "buy", 100, {1: 10, 2: 20})
    assert primary.quantity_ahead == 30
    assert primary.apply(QueueMutation("E", "sell", 1, 5, 100)).quantity == 0
    assert primary.apply(QueueMutation("A", "buy", 99, 5, 101)).quantity == 0
    assert primary.apply(QueueMutation("X", "buy", 1, 4, 100)).quantity == 0
    assert primary.ahead[1] == 6
    primary.apply(QueueMutation("C", "buy", 1, 10, 100))
    primary.apply(QueueMutation("U", "buy", 2, 20, 101))
    assert primary.quantity_ahead == 0
    fill = primary.apply(QueueMutation("E", "buy", 99, 7, 100))
    assert (fill.quantity, fill.reason) == (7, "factual_execution")
    through = primary.apply(QueueMutation("E", "buy", 99, 3, 99))
    assert (through.quantity, through.reason) == (3, "trade_through")
    with pytest.raises(ValueError, match="non-negative"):
        primary.apply(QueueMutation("X", "buy", 1, -1, 100))

    lower = QueueOracle("trade_through_only", "buy", 100)
    upper = QueueOracle("front_of_queue", "buy", 100)
    assert lower.apply(QueueMutation("E", "buy", 1, 4, 100)).quantity == 0
    assert upper.apply(QueueMutation("C", "buy", 1, 4, 100)).quantity == 4
    assert upper.apply(QueueMutation("X", "buy", 1, 4, 100)).quantity == 0


@given(
    quantity=st.integers(min_value=1, max_value=10_000),
    price4=st.integers(min_value=1, max_value=1_000_000),
    fee=st.integers(min_value=0, max_value=1_000),
    rebate=st.integers(min_value=0, max_value=1_000),
)
def test_accounting_exact_integer_properties(
    quantity: int, price4: int, fee: int, rebate: int
) -> None:
    initial = AccountingState(0, 0, 0, 0, 0)
    bought = apply_accounting_fill(
        initial,
        side="buy",
        quantity=quantity,
        price4=price4,
        fee_nanos_per_share=fee,
        rebate_nanos_per_share=rebate,
    )
    assert bought.inventory == quantity
    assert bought.trade_cash_nanos == -price4 * 100_000 * quantity
    assert bought.turnover_quantity == quantity
    assert net_equity_at_mid2(bought, price4 * 2) == (rebate - fee) * quantity


def test_accounting_rejections_and_sell() -> None:
    initial = AccountingState(0, 0, 0, 0, 0)
    sold = apply_accounting_fill(
        initial,
        side="sell",
        quantity=2,
        price4=105,
        fee_nanos_per_share=3,
        rebate_nanos_per_share=1,
    )
    assert sold.inventory == -2
    assert gross_equity_at_mid2(sold, 200) == 1_000_000
    with pytest.raises(ValueError, match="invalid"):
        apply_accounting_fill(
            initial,
            side="buy",
            quantity=0,
            price4=100,
            fee_nanos_per_share=0,
            rebate_nanos_per_share=0,
        )
    with pytest.raises(ValueError, match="mid2"):
        gross_equity_at_mid2(initial, -1)
    with pytest.raises(OverflowError):
        apply_accounting_fill(
            initial,
            side="buy",
            quantity=2**62,
            price4=1,
            fee_nanos_per_share=0,
            rebate_nanos_per_share=0,
        )


def test_strategy_scalar_math_rounding_and_invalid_states() -> None:
    common = {
        "timestamp_ns": 1_000_000_000,
        "session_end_ns": 2_000_000_000,
        "inventory": 0,
        "bids": [100, 99, 98],
        "asks": [102, 103, 104],
        "tick_size_price4": 1,
        "maximum_distance_ticks": 10,
        "symmetric_half_spread_price4": 1.0,
        "gamma": 0.1,
        "sigma_squared": 0.01,
        "arrival_intensity_k": 2.0,
    }
    symmetric = strategy_quote_scalar(kind="symmetric_quote", **common)
    assert (symmetric.bid_price4, symmetric.ask_price4) == (100, 102)
    assert symmetric.reservation_price4 == 101.0
    as_quote = strategy_quote_scalar(kind="avellaneda_stoikov", **common)
    assert as_quote.total_spread_price4 == pytest.approx(0.001 + 20.0 * math.log1p(0.05))
    signaled = strategy_quote_scalar(
        kind="signal_aware_as",
        causal_signal=2.0,
        signal_coefficient_price4=0.5,
        **common,
    )
    assert signaled.reservation_price4 == pytest.approx(102.0)
    tiny = strategy_quote_scalar(kind="avellaneda_stoikov", **{**common, "gamma": 1e-10})
    assert tiny.total_spread_price4 == pytest.approx(1.0)
    for overrides in (
        {"bids": []},
        {"bids": [102]},
        {"timestamp_ns": 2_000_000_000},
        {"gamma": 0.0},
        {"arrival_intensity_k": 0.0},
        {"sigma_squared": -1.0},
        {"gamma": math.inf},
    ):
        invalid = strategy_quote_scalar(kind="avellaneda_stoikov", **{**common, **overrides})
        assert invalid.bid_price4 is None and invalid.ask_price4 is None


def test_right_continuous_markout_and_invalid_boundary() -> None:
    points = [
        MarketPoint(100, 1, True, 200),
        MarketPoint(110, 1, True, 202),
        MarketPoint(110, 2, True, 204),
        MarketPoint(120, 3, False, 0),
        MarketPoint(121, 4, True, 206),
    ]
    assert (
        right_continuous_markout(
            points,
            fill_timestamp_ns=100,
            horizon_ns=10,
            side="buy",
            fill_price4=100,
            quantity=2,
        )
        == 400_000
    )
    assert (
        right_continuous_markout(
            points,
            fill_timestamp_ns=110,
            horizon_ns=10,
            side="sell",
            fill_price4=102,
            quantity=1,
        )
        is None
    )
    assert (
        right_continuous_markout(
            points,
            fill_timestamp_ns=100,
            horizon_ns=0,
            side="sell",
            fill_price4=100,
            quantity=1,
        )
        == 0
    )
    assert (
        right_continuous_markout(
            [],
            fill_timestamp_ns=100,
            horizon_ns=10,
            side="buy",
            fill_price4=100,
            quantity=1,
        )
        is None
    )
    with pytest.raises(ValueError):
        right_continuous_markout(
            points,
            fill_timestamp_ns=100,
            horizon_ns=-1,
            side="buy",
            fill_price4=100,
            quantity=1,
        )
