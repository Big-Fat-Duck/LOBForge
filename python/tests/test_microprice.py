from __future__ import annotations

import json

import numpy as np
import pytest

from lobforge_research.microprice import StoikovMicroprice, solve_absorbing_adjustment
from lobforge_research.synthetic import generate_planted_signal


def test_absorbing_chain_matches_analytic_answer_and_detects_pathology():
    adjustment, condition, residual, reason = solve_absorbing_adjustment(
        np.asarray([[0.5]], dtype=np.float64),
        np.asarray([1.0], dtype=np.float64),
        1e10,
        1e-12,
    )
    assert reason is None and adjustment is not None
    np.testing.assert_allclose(adjustment, [2.0])
    assert condition == 1.0 and residual == 0.0
    adjustment, _, _, reason = solve_absorbing_adjustment(
        np.asarray([[1.0]], dtype=np.float64),
        np.asarray([0.0], dtype=np.float64),
        1e10,
        1e-12,
    )
    assert adjustment is None and reason == "ill_conditioned_transition_matrix"
    with pytest.raises(ValueError):
        solve_absorbing_adjustment(np.ones((2, 3)), np.ones(2), 1e10, 1e-12)
    with pytest.raises(ValueError):
        solve_absorbing_adjustment(np.ones((2, 2)), np.ones(3), 1e10, 1e-12)


def test_microprice_train_only_serialization_and_transform_is_frozen(tmp_path):
    rows = generate_planted_signal(days=5, symbols=2, rows_per_symbol_day=300)
    train = [row for row in rows if row["session_date"] <= "2026-01-07"]
    test = [row for row in rows if row["session_date"] > "2026-01-07"]
    model = StoikovMicroprice(minimum_state_samples=5).fit(train)
    assert model.diagnostics is not None
    assert model.diagnostics.maximum_probability_row_error < 1e-12
    assert model.diagnostics.failure_reason is None
    before = json.dumps(model.to_dict(), sort_keys=True)
    transformed = model.transform(test)
    after = json.dumps(model.to_dict(), sort_keys=True)
    assert before == after
    adjustments = [
        row["stoikov_microprice_displacement"]
        for row in transformed
        if row["stoikov_microprice_displacement"] is not None
    ]
    assert adjustments and np.mean(np.asarray(adjustments) > 0) > 0.3
    path = tmp_path / "microprice.json"
    model.save(path)
    assert json.loads(path.read_text(encoding="utf-8"))["version"] == 1
    with pytest.raises(ValueError):
        StoikovMicroprice().fit(train, partition="validation")


def test_microprice_explicit_fallback_reasons():
    rows = generate_planted_signal(days=2, symbols=1, rows_per_symbol_day=100)
    model = StoikovMicroprice(minimum_state_samples=10_000).fit(rows)
    insufficient = model.transform_one(rows[0])
    assert insufficient["stoikov_fallback_reason"] == "insufficient_state_samples"
    unseen = dict(rows[0], spread_price4=999)
    assert model.transform_one(unseen)["stoikov_fallback_reason"] == "unseen_state"
    invalid = dict(rows[0], mid2=None)
    assert model.transform_one(invalid)["stoikov_fallback_reason"] == "invalid_book_state"
    unfitted = StoikovMicroprice().transform_one(rows[0])
    assert unfitted["stoikov_fallback_reason"] == "model_not_fitted"
    with pytest.raises(ValueError):
        StoikovMicroprice(imbalance_bins=1)
    with pytest.raises(ValueError):
        StoikovMicroprice().fit(rows[:1])
    with pytest.raises(ValueError):
        StoikovMicroprice().to_dict()

    unreachable_rows = [
        dict(rows[0], spread_price4=2, segment_id=1),
        dict(rows[1], spread_price4=4, segment_id=1),
    ]
    unreachable = StoikovMicroprice(minimum_state_samples=1).fit(unreachable_rows)
    assert unreachable.diagnostics is not None
    assert unreachable.diagnostics.failure_reason == "unreachable_state_without_outgoing_transition"
    assert (
        unreachable.transform_one(unreachable_rows[0])["stoikov_fallback_reason"]
        == "unreachable_state_without_outgoing_transition"
    )
