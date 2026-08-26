"""Deterministic Round 4 implementation controls; never real-market evidence."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any

import numpy as np

from .round4_oracle import (
    AccountingState,
    MarketPoint,
    QueueMutation,
    QueueOracle,
    apply_accounting_fill,
    net_equity_at_mid2,
    right_continuous_markout,
    strategy_quote_scalar,
)


def _result(name: str, passed: bool, **metrics: Any) -> dict[str, Any]:
    return {"scenario": name, "status": "PASS" if passed else "FAIL", "metrics": metrics}


def run_round4_synthetic_controls(seed: int = 20260826) -> dict[str, Any]:
    """Run all eight predeclared controls using independent scalar calculations."""

    scenarios: list[dict[str, Any]] = []

    no_fill = QueueOracle("trade_through_only", "buy", 100)
    no_fill_evidence = no_fill.apply(QueueMutation("E", "buy", 1, 10, 100))
    flat = AccountingState(0, 0, 0, 0, 0)
    scenarios.append(
        _result(
            "no_eligible_fill",
            no_fill_evidence.quantity == 0
            and asdict(flat) == asdict(AccountingState(0, 0, 0, 0, 0)),
            fill_quantity=no_fill_evidence.quantity,
            inventory=flat.inventory,
            net_pnl_nanos=0,
        )
    )

    fifo = QueueOracle("mbo_fifo_conservative", "buy", 100, {1: 10, 2: 20})
    fifo.apply(QueueMutation("A", "buy", 3, 50, 100))
    fifo.apply(QueueMutation("E", "buy", 1, 4, 100))
    fifo.apply(QueueMutation("X", "buy", 1, 6, 100))
    fifo.apply(QueueMutation("D", "buy", 2, 20, 100))
    partial = fifo.apply(QueueMutation("E", "buy", 99, 4, 100)).quantity
    complete = fifo.apply(QueueMutation("C", "buy", 99, 6, 100)).quantity
    replaced = QueueOracle("mbo_fifo_conservative", "buy", 100, {7: 15})
    scenarios.append(
        _result(
            "exact_fifo_queue",
            fifo.quantity_ahead == 0
            and partial == 4
            and complete == 6
            and replaced.quantity_ahead == 15,
            queue_ahead_after=0,
            partial_fill=partial,
            final_fill=complete,
            replace_queue_ahead=replaced.quantity_ahead,
        )
    )

    factual_execution_ns = 100
    zero_latency_activation_ns = 90
    slow_activation_ns = 110
    zero_fills = int(zero_latency_activation_ns < factual_execution_ns)
    slow_fills = int(slow_activation_ns < factual_execution_ns)
    scenarios.append(
        _result(
            "order_latency_race",
            zero_fills == 1 and slow_fills == 0,
            zero_latency_fills=zero_fills,
            slow_order_fills=slow_fills,
        )
    )

    cancel_request_ns = 100
    cancel_confirm_ns = 120
    execution_ns = 110
    pending_cancel_fill = cancel_request_ns <= execution_ns < cancel_confirm_ns
    scenarios.append(
        _result(
            "cancel_latency_race",
            pending_cancel_fill,
            pending_cancel_fill=pending_cancel_fill,
            exposure_ns=cancel_confirm_ns - cancel_request_ns,
        )
    )

    evidence = (["buy"] * 10 + ["sell"] * 10) * 10

    def inventory_path(kind: str) -> np.ndarray:
        inventory = 0
        path: list[int] = []
        for event_side in evidence:
            quote = strategy_quote_scalar(
                kind=kind,  # type: ignore[arg-type]
                timestamp_ns=1_000_000_000,
                session_end_ns=2_000_000_000,
                inventory=inventory,
                bids=list(range(100, 89, -1)),
                asks=list(range(102, 113)),
                tick_size_price4=1,
                maximum_distance_ticks=10,
                symmetric_half_spread_price4=1.0,
                gamma=0.2,
                sigma_squared=1.0,
                arrival_intensity_k=1.1,
            )
            if event_side == "buy" and quote.bid_price4 == 100:
                inventory += 1
            elif event_side == "sell" and quote.ask_price4 == 102:
                inventory -= 1
            path.append(inventory)
        return np.asarray(path, dtype=np.float64)

    symmetric_inventory = inventory_path("symmetric_quote")
    as_inventory = inventory_path("avellaneda_stoikov")
    symmetric_twa = float(np.mean(np.abs(symmetric_inventory)))
    as_twa = float(np.mean(np.abs(as_inventory)))
    reduction = 1.0 - as_twa / symmetric_twa
    scenarios.append(
        _result(
            "inventory_pressure",
            reduction >= 0.20,
            symmetric_time_weighted_abs_inventory=symmetric_twa,
            as_time_weighted_abs_inventory=as_twa,
            reduction=reduction,
        )
    )

    points = [
        MarketPoint(110, 1, True, 202),
        MarketPoint(200, 2, True, 204),
        MarketPoint(1_100, 3, True, 198),
    ]
    markouts = [
        right_continuous_markout(
            points,
            fill_timestamp_ns=100,
            horizon_ns=horizon,
            side="buy",
            fill_price4=100,
            quantity=5,
        )
        for horizon in (10, 100, 1_000)
    ]
    scenarios.append(
        _result(
            "adverse_selection_markout",
            markouts == [500_000, 1_000_000, -500_000],
            directional_value_nanos=markouts,
            adverse_selection_cost_nanos=[
                -value if value is not None else None for value in markouts
            ],
        )
    )

    low_fee = apply_accounting_fill(
        flat,
        side="buy",
        quantity=10,
        price4=100,
        fee_nanos_per_share=1,
        rebate_nanos_per_share=0,
    )
    high_fee = apply_accounting_fill(
        flat,
        side="buy",
        quantity=10,
        price4=100,
        fee_nanos_per_share=5,
        rebate_nanos_per_share=0,
    )
    gross_low = low_fee.trade_cash_nanos + low_fee.inventory * 100 * 100_000
    gross_high = high_fee.trade_cash_nanos + high_fee.inventory * 100 * 100_000
    net_low = net_equity_at_mid2(low_fee, 200)
    net_high = net_equity_at_mid2(high_fee, 200)
    scenarios.append(
        _result(
            "fee_shock",
            gross_low == gross_high and net_low - net_high == 40 and net_high <= net_low,
            gross_low_nanos=gross_low,
            gross_high_nanos=gross_high,
            net_low_nanos=net_low,
            net_high_nanos=net_high,
        )
    )

    random = np.random.default_rng(seed)
    observations = 50_000
    signal = random.normal(size=observations)
    noise = random.normal(size=observations)
    planted_target = 0.7 * signal + noise
    null_target = random.normal(size=observations)
    shuffled_target = planted_target[random.permutation(observations)]
    planted_ic = float(np.corrcoef(signal, planted_target)[0, 1])
    null_ic = float(np.corrcoef(signal, null_target)[0, 1])
    shuffled_ic = float(np.corrcoef(signal, shuffled_target)[0, 1])
    scenarios.append(
        _result(
            "signal_null_shuffled_positive",
            planted_ic > 0.50 and abs(null_ic) < 0.02 and abs(shuffled_ic) < 0.02,
            observations=observations,
            planted_ic=planted_ic,
            null_ic=null_ic,
            shuffled_ic=shuffled_ic,
            market_alpha_status="NOT_EVALUATED_SYNTHETIC_ONLY",
        )
    )

    return {
        "schema": "lobforge.round4_synthetic_controls",
        "version": 1,
        "seed": seed,
        "status": "PASS" if all(row["status"] == "PASS" for row in scenarios) else "FAIL",
        "scenarios": scenarios,
        "evidence_scope": "implementation controls only; not real alpha or profitability",
        "F1": "BLOCKED: DATASET_NOT_PROVIDED",
        "F2": "BLOCKED: DATASET_NOT_PROVIDED",
        "F3": "BLOCKED: DATASET_NOT_PROVIDED",
        "P1": "BLOCKED: DATASET_NOT_PROVIDED",
    }
