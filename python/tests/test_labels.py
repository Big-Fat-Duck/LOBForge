from __future__ import annotations

from copy import deepcopy

from lobforge_research.canonical import LogicalDigest
from lobforge_research.features import FeatureEngine
from lobforge_research.labels import EventTimeLabeler, ResearchStreamBuilder


def _feature_digest(rows):
    digest = LogicalDigest()
    ordered = sorted(
        rows,
        key=lambda row: (
            row["anchor_timestamp_ns"],
            row["anchor_sequence"],
            row["sample_kind"],
        ),
    )
    for row in ordered:
        digest.update({key: value for key, value in row.items() if not key.startswith("target_")})
    return digest.hexdigest()


def test_event_horizons_and_half_tick_are_exact(event_factory):
    engine = FeatureEngine()
    labeler = EventTimeLabeler()
    output = []
    for sequence in range(1, 102):
        feature = engine.accept(
            event_factory(
                sequence=sequence,
                timestamp_ns=sequence,
                bid=100 + sequence,
                ask=102 + sequence,
            )
        )
        output.extend(labeler.accept(feature))
    output.extend(labeler.finish())
    first = output[0]
    assert first["mid2"] == 204
    assert first["target_mid2_delta_event_1"] == 2
    assert first["target_mid2_delta_event_10"] == 20
    assert first["target_mid2_delta_event_50"] == 100
    assert first["target_mid2_delta_event_100"] == 200
    assert first["target_direction_event_100"] == 1
    assert output[-1]["target_mid2_delta_event_1"] is None


def test_event_labels_do_not_cross_invalid_boundary(event_factory):
    builder = ResearchStreamBuilder()
    rows = []
    rows.extend(builder.accept(event_factory(sequence=1, timestamp_ns=1, bid=100, ask=101)))
    rows.extend(builder.accept(event_factory(sequence=2, timestamp_ns=2, bid=101, ask=102)))
    rows.extend(
        builder.accept(
            event_factory(sequence=3, timestamp_ns=3, trading_state="H", bid=101, ask=102)
        )
    )
    rows.extend(builder.accept(event_factory(sequence=4, timestamp_ns=4, bid=110, ask=111)))
    rows.extend(builder.finish())
    events = [row for row in rows if row["sample_kind"] == "event"]
    assert events[0]["target_mid2_delta_event_1"] == 2
    assert events[1]["target_mid2_delta_event_1"] is None
    assert events[2]["target_mid2_delta_event_1"] is None


def test_clock_sampling_is_right_continuous_and_uses_last_same_timestamp(event_factory):
    builder = ResearchStreamBuilder()
    output = []
    output.extend(
        builder.accept(event_factory(sequence=1, timestamp_ns=5_000_000, bid=100, ask=102))
    )
    output.extend(
        builder.accept(event_factory(sequence=2, timestamp_ns=10_000_000, bid=101, ask=103))
    )
    output.extend(
        builder.accept(event_factory(sequence=3, timestamp_ns=10_000_000, bid=102, ask=104))
    )
    output.extend(
        builder.accept(event_factory(sequence=4, timestamp_ns=15_000_000, bid=103, ask=105))
    )
    output.extend(
        builder.accept(event_factory(sequence=5, timestamp_ns=25_000_000, bid=103, ask=105))
    )
    output.extend(builder.finish())
    clock_10 = [row for row in output if row["sample_kind"] == "clock_10ms"]
    at_10 = next(row for row in clock_10 if row["anchor_timestamp_ns"] == 10_000_000)
    assert at_10["anchor_sequence"] == 3
    assert at_10["mid2"] == 206
    assert at_10["target_mid2_delta_clock_10ms"] == 2
    assert at_10["max_feature_sequence"] <= at_10["anchor_sequence"]


def test_prefix_invariance_and_future_tail_perturbation(event_factory):
    prefix = [
        event_factory(
            sequence=sequence,
            timestamp_ns=sequence * 20_000_000,
            bid=100 + sequence % 2,
            ask=102 + sequence % 2,
            bid_quantity=10 + sequence,
        )
        for sequence in range(1, 20)
    ]
    tail_a = [
        event_factory(sequence=sequence, timestamp_ns=sequence * 20_000_000, bid=100, ask=102)
        for sequence in range(20, 30)
    ]
    tail_b = deepcopy(tail_a)
    for event in tail_b:
        event["bids"][0]["price4"] += 20
        event["asks"][0]["price4"] += 20

    def build(events):
        builder = ResearchStreamBuilder()
        rows = []
        for event in events:
            rows.extend(builder.accept(event))
        rows.extend(builder.finish())
        return rows

    prefix_rows = build(prefix)
    combined_a = build(prefix + tail_a)
    combined_b = build(prefix + tail_b)
    cutoff = prefix[-1]["timestamp_ns"]
    a_before = [row for row in combined_a if row["anchor_timestamp_ns"] <= cutoff]
    b_before = [row for row in combined_b if row["anchor_timestamp_ns"] <= cutoff]
    assert _feature_digest(a_before) == _feature_digest(b_before)
    assert _feature_digest(prefix_rows) == _feature_digest(a_before)
    event_anchor_a = next(
        row for row in combined_a if row["sample_kind"] == "event" and row["anchor_sequence"] == 19
    )
    event_anchor_b = next(
        row for row in combined_b if row["sample_kind"] == "event" and row["anchor_sequence"] == 19
    )
    assert event_anchor_a["ofi_l1_100ms"] == event_anchor_b["ofi_l1_100ms"]
    assert (
        event_anchor_a["target_mid2_delta_event_10"] != event_anchor_b["target_mid2_delta_event_10"]
    )
