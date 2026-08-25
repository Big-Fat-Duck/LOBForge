from __future__ import annotations

import math

import numpy as np
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st

from lobforge_research.features import (
    FeatureEngine,
    cont_ofi_increment,
    cont_ofi_vectorized,
    static_book_features,
    static_features_vectorized,
)


@given(
    bid_quantity=st.integers(1, 10**9),
    ask_quantity=st.integers(1, 10**9),
    scale=st.integers(1, 10**5),
)
@settings(suppress_health_check=[HealthCheck.function_scoped_fixture])
def test_imbalance_properties(event_factory, bid_quantity, ask_quantity, scale):
    event = event_factory(bid_quantity=bid_quantity, ask_quantity=ask_quantity)
    features = static_book_features(event)
    imbalance = features["imbalance_l1"]
    assert imbalance is not None and -1.0 <= imbalance <= 1.0
    swapped = static_book_features(
        event_factory(bid_quantity=ask_quantity, ask_quantity=bid_quantity)
    )["imbalance_l1"]
    assert swapped == -imbalance
    scaled = static_book_features(
        event_factory(
            bid_quantity=bid_quantity * scale,
            ask_quantity=ask_quantity * scale,
        )
    )["imbalance_l1"]
    assert math.isclose(float(scaled), imbalance, rel_tol=0.0, abs_tol=1e-15)


def test_static_features_exact_identity_bounds_and_missing(event_factory):
    event = event_factory(bid=100, ask=104, bid_quantity=30, ask_quantity=10)
    features = static_book_features(event)
    assert features["mid2"] == 204
    assert features["spread_price4"] == 4
    assert features["imbalance_l1"] == 0.5
    assert features["weighted_midprice_l1"] == 103.0
    assert features["weighted_midprice_displacement"] == 1.0
    assert 100 <= float(features["weighted_midprice_l1"]) <= 104
    assert float(features["weighted_midprice_l1"]) - 102 == 4 * 0.5 / 2
    equal = static_book_features(event_factory(bid_quantity=50, ask_quantity=50))
    assert equal["imbalance_l1"] == 0.0
    shallow = static_book_features(event_factory(depth=4))
    assert shallow["imbalance_l1"] is not None
    assert shallow["imbalance_l5"] is None
    assert shallow["imbalance_l10"] is None
    for invalid in (
        event_factory(two_sided=False),
        event_factory(bid=103, ask=102, crossed=True),
        event_factory(trading_state="H"),
        event_factory(session_state="market_hours_ended"),
    ):
        assert all(value is None for value in static_book_features(invalid).values())


def test_cont_ofi_golden_cases():
    previous = (100, 10, 101, 12)
    cases = [
        ((100, 15, 101, 12), 5),
        ((100, 7, 101, 12), -3),
        ((100, 10, 101, 20), -8),
        ((100, 10, 101, 7), 5),
        ((101, 8, 101, 12), 8),
        ((99, 9, 101, 12), -10),
        ((100, 10, 100, 8), -8),
        ((100, 10, 102, 9), 12),
        ((101, 8, 100, 8), 0),
    ]
    for current, expected in cases:
        assert cont_ofi_increment(*previous, *current) == expected


def test_feature_engine_windows_multisymbol_same_timestamp_and_resets(event_factory):
    engine = FeatureEngine()
    first = engine.accept(event_factory(sequence=1, timestamp_ns=0, symbol="A"))
    assert first["ofi_l1_events_10"] is None
    engine.accept(event_factory(sequence=2, timestamp_ns=0, symbol="B"))
    latest = first
    for sequence in range(3, 24):
        latest = engine.accept(
            event_factory(
                sequence=sequence,
                timestamp_ns=(sequence - 2) * 1_000_000,
                symbol="A",
                bid_quantity=10 + sequence,
            )
        )
    assert latest["ofi_l1_events_10"] == 10
    assert latest["ofi_l1_10ms"] == 10
    sampled = engine.sample("A", int(latest["anchor_timestamp_ns"]) + 10_000_000)
    assert sampled is not None
    assert sampled["max_feature_sequence"] == latest["anchor_sequence"]
    invalid = engine.accept(
        event_factory(sequence=24, timestamp_ns=30_000_000, symbol="A", trading_state="P")
    )
    assert invalid["mid2"] is None and engine.sample("A", 30_000_000) is None
    resumed = engine.accept(event_factory(sequence=25, timestamp_ns=31_000_000, symbol="A"))
    assert resumed["mid2"] is not None and resumed["ofi_l1_10ms"] is None
    gap = engine.accept(event_factory(sequence=27, timestamp_ns=32_000_000, symbol="A"))
    assert gap["ofi_l1_events_10"] is None


def test_vectorized_features_and_100k_ofi_match_scalar_oracle():
    generator = np.random.default_rng(7)
    rows = 100_001
    bid = generator.integers(100, 110, size=rows, dtype=np.int64)
    ask = bid + generator.integers(1, 4, size=rows, dtype=np.int64)
    bid_quantity = generator.integers(1, 1000, size=rows, dtype=np.int64)
    ask_quantity = generator.integers(1, 1000, size=rows, dtype=np.int64)
    vectorized = cont_ofi_vectorized(bid, bid_quantity, ask, ask_quantity)
    oracle = np.zeros(rows, dtype=np.int64)
    for index in range(1, rows):
        oracle[index] = cont_ofi_increment(
            int(bid[index - 1]),
            int(bid_quantity[index - 1]),
            int(ask[index - 1]),
            int(ask_quantity[index - 1]),
            int(bid[index]),
            int(bid_quantity[index]),
            int(ask[index]),
            int(ask_quantity[index]),
        )
    np.testing.assert_array_equal(vectorized, oracle)
    bid_prices = np.column_stack([bid - level for level in range(10)])
    ask_prices = np.column_stack([ask + level for level in range(10)])
    bid_quantities = np.tile(bid_quantity[:, None], (1, 10))
    ask_quantities = np.tile(ask_quantity[:, None], (1, 10))
    features = static_features_vectorized(bid_prices, ask_prices, bid_quantities, ask_quantities)
    np.testing.assert_array_equal(features["mid2"], bid + ask)
    np.testing.assert_allclose(
        features["imbalance_l10"],
        (bid_quantity - ask_quantity) / (bid_quantity + ask_quantity),
    )


def test_vectorized_rejects_bad_shapes():
    one = np.ones(3, dtype=np.int64)
    try:
        cont_ofi_vectorized(one, one[:-1], one, one)
    except ValueError as error:
        assert "identical" in str(error)
    else:
        raise AssertionError("shape mismatch accepted")
    try:
        static_features_vectorized(one, one, one, one)
    except ValueError as error:
        assert "level" in str(error)
    else:
        raise AssertionError("one-dimensional books accepted")
