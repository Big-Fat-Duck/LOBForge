"""Train-only Stoikov-style finite-state learned micro-price."""

from __future__ import annotations

import json
import math
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np
import numpy.typing as npt


def solve_absorbing_adjustment(
    q_probability: npt.NDArray[np.float64],
    immediate_move: npt.NDArray[np.float64],
    condition_number_limit: float,
    tolerance: float,
) -> tuple[npt.NDArray[np.float64] | None, float, float, str | None]:
    """Solve (I-Q)g=r without ever forming an explicit matrix inverse."""

    if q_probability.ndim != 2 or q_probability.shape[0] != q_probability.shape[1]:
        raise ValueError("Q must be square")
    if immediate_move.shape != (q_probability.shape[0],):
        raise ValueError("immediate move vector has the wrong shape")
    system = np.eye(q_probability.shape[0], dtype=np.float64) - q_probability
    condition = float(np.linalg.cond(system))
    if not math.isfinite(condition) or condition > condition_number_limit:
        return None, condition, math.inf, "ill_conditioned_transition_matrix"
    try:
        adjustment = np.linalg.solve(system, immediate_move)
    except np.linalg.LinAlgError:
        return None, condition, math.inf, "singular_transition_matrix"
    residual = float(np.max(np.abs(system @ adjustment - immediate_move), initial=0.0))
    if not np.all(np.isfinite(adjustment)) or residual > tolerance:
        return None, condition, residual, "linear_solve_did_not_converge"
    return np.asarray(adjustment, dtype=np.float64), condition, residual, None


@dataclass(slots=True)
class MicropriceDiagnostics:
    fitted_rows: int
    transitions: int
    states: int
    maximum_probability_row_error: float
    condition_number: float | str
    solve_residual: float | str
    failure_reason: str | None


class StoikovMicroprice:
    """Finite-state estimate of expected next mid-price adjustment.

    States are ``(spread_price4, train-fitted imbalance_bin)``. Q contains
    no-mid-change transitions and R contains absorbing mid-change transitions.
    The adjustment solves the absorbing-chain conditional expectation.
    """

    def __init__(
        self,
        *,
        imbalance_bins: int = 5,
        minimum_state_samples: int = 20,
        condition_number_limit: float = 1e10,
        convergence_tolerance: float = 1e-10,
        smoothing: float = 0.0,
    ) -> None:
        if imbalance_bins < 2 or minimum_state_samples < 1 or smoothing < 0:
            raise ValueError("invalid micro-price configuration")
        self.imbalance_bins = imbalance_bins
        self.minimum_state_samples = minimum_state_samples
        self.condition_number_limit = condition_number_limit
        self.convergence_tolerance = convergence_tolerance
        self.smoothing = smoothing
        self.bin_edges: list[float] = []
        self.states: list[tuple[int, int]] = []
        self.state_counts: list[int] = []
        self.q_probability: npt.NDArray[np.float64] | None = None
        self.r_probability: npt.NDArray[np.float64] | None = None
        self.adjustment: npt.NDArray[np.float64] | None = None
        self.price_move_distribution: list[dict[str, int]] = []
        self.diagnostics: MicropriceDiagnostics | None = None
        self._state_index: dict[tuple[int, int], int] = {}

    def _bin(self, imbalance: float) -> int:
        return int(np.searchsorted(np.asarray(self.bin_edges), imbalance, side="right"))

    @staticmethod
    def _eligible_rows(rows: Iterable[Mapping[str, Any]]) -> list[Mapping[str, Any]]:
        return [
            row
            for row in rows
            if row.get("sample_kind", "event") == "event"
            and row.get("mid2") is not None
            and row.get("spread_price4") is not None
            and row.get("imbalance_l1") is not None
        ]

    def fit(
        self, rows: Sequence[Mapping[str, Any]], *, partition: str = "train"
    ) -> StoikovMicroprice:
        if partition != "train":
            raise ValueError("Stoikov micro-price may only be fitted on the train partition")
        eligible = self._eligible_rows(rows)
        if len(eligible) < 2:
            raise ValueError("at least two valid training rows are required")
        imbalances = np.asarray([float(row["imbalance_l1"]) for row in eligible])
        quantiles = np.linspace(0.0, 1.0, self.imbalance_bins + 1)[1:-1]
        self.bin_edges = sorted(set(float(value) for value in np.quantile(imbalances, quantiles)))
        observed_states = {
            (int(row["spread_price4"]), self._bin(float(row["imbalance_l1"]))) for row in eligible
        }
        self.states = sorted(observed_states)
        self._state_index = {state: index for index, state in enumerate(self.states)}
        size = len(self.states)
        q_counts = np.full((size, size), self.smoothing, dtype=np.float64)
        r_counts = np.full((size, size), self.smoothing, dtype=np.float64)
        move_numerator = np.zeros(size, dtype=np.float64)
        outgoing_observed = np.zeros(size, dtype=np.int64)
        distributions: list[dict[str, int]] = [dict() for _ in range(size)]
        transitions = 0
        previous_by_group: dict[tuple[str, str], Mapping[str, Any]] = {}
        for row in eligible:
            group = (str(row["session_date"]), str(row["symbol"]))
            previous = previous_by_group.get(group)
            previous_by_group[group] = row
            if previous is None or previous.get("segment_id") != row.get("segment_id"):
                continue
            previous_state = (
                int(previous["spread_price4"]),
                self._bin(float(previous["imbalance_l1"])),
            )
            next_state = (
                int(row["spread_price4"]),
                self._bin(float(row["imbalance_l1"])),
            )
            source = self._state_index[previous_state]
            destination = self._state_index[next_state]
            delta_price4 = (int(row["mid2"]) - int(previous["mid2"])) / 2.0
            outgoing_observed[source] += 1
            transitions += 1
            if delta_price4 == 0.0:
                q_counts[source, destination] += 1.0
            else:
                r_counts[source, destination] += 1.0
                move_numerator[source] += delta_price4
                key = float(delta_price4).hex()
                distributions[source][key] = distributions[source].get(key, 0) + 1

        total = q_counts.sum(axis=1) + r_counts.sum(axis=1)
        q_probability = np.divide(
            q_counts,
            total[:, None],
            out=np.zeros_like(q_counts),
            where=total[:, None] != 0,
        )
        r_probability = np.divide(
            r_counts,
            total[:, None],
            out=np.zeros_like(r_counts),
            where=total[:, None] != 0,
        )
        immediate_move = np.divide(
            move_numerator,
            total,
            out=np.zeros_like(move_numerator),
            where=total != 0,
        )
        row_error = float(
            np.max(np.abs(q_probability.sum(axis=1) + r_probability.sum(axis=1) - 1.0))
        )
        adjustment: npt.NDArray[np.float64] | None
        failure: str | None
        if np.any(total == 0):
            adjustment = None
            condition = math.inf
            residual = math.inf
            failure = "unreachable_state_without_outgoing_transition"
        else:
            adjustment, condition, residual, failure = solve_absorbing_adjustment(
                q_probability,
                immediate_move,
                self.condition_number_limit,
                self.convergence_tolerance,
            )
        self.state_counts = [int(value) for value in outgoing_observed]
        self.q_probability = q_probability
        self.r_probability = r_probability
        self.adjustment = adjustment
        self.price_move_distribution = distributions
        self.diagnostics = MicropriceDiagnostics(
            fitted_rows=len(eligible),
            transitions=transitions,
            states=size,
            maximum_probability_row_error=row_error,
            condition_number=condition if math.isfinite(condition) else "infinity",
            solve_residual=residual if math.isfinite(residual) else "infinity",
            failure_reason=failure,
        )
        return self

    def transform_one(self, row: Mapping[str, Any]) -> dict[str, Any]:
        output = dict(row)
        output["stoikov_microprice"] = None
        output["stoikov_microprice_displacement"] = None
        output["stoikov_fallback_reason"] = None
        if self.diagnostics is None or self.q_probability is None:
            output["stoikov_fallback_reason"] = "model_not_fitted"
            return output
        if self.diagnostics.failure_reason is not None or self.adjustment is None:
            output["stoikov_fallback_reason"] = self.diagnostics.failure_reason
            return output
        if (
            row.get("mid2") is None
            or row.get("spread_price4") is None
            or row.get("imbalance_l1") is None
        ):
            output["stoikov_fallback_reason"] = "invalid_book_state"
            return output
        state = (int(row["spread_price4"]), self._bin(float(row["imbalance_l1"])))
        index = self._state_index.get(state)
        if index is None:
            output["stoikov_fallback_reason"] = "unseen_state"
            return output
        if self.state_counts[index] < self.minimum_state_samples:
            output["stoikov_fallback_reason"] = "insufficient_state_samples"
            return output
        adjustment = float(self.adjustment[index])
        output["stoikov_microprice"] = int(row["mid2"]) / 2.0 + adjustment
        output["stoikov_microprice_displacement"] = adjustment
        return output

    def transform(self, rows: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
        """Transform validation/test rows without updating any fitted statistic."""

        return [self.transform_one(row) for row in rows]

    def to_dict(self) -> dict[str, Any]:
        if self.diagnostics is None or self.q_probability is None or self.r_probability is None:
            raise ValueError("model is not fitted")
        return {
            "model_schema": "lobforge.stoikov_microprice",
            "version": 1,
            "configuration": {
                "imbalance_bins": self.imbalance_bins,
                "minimum_state_samples": self.minimum_state_samples,
                "condition_number_limit": self.condition_number_limit,
                "convergence_tolerance": self.convergence_tolerance,
                "smoothing": self.smoothing,
            },
            "bin_edges": self.bin_edges,
            "states": [list(state) for state in self.states],
            "state_counts": self.state_counts,
            "q_probability": self.q_probability.tolist(),
            "r_probability": self.r_probability.tolist(),
            "adjustment": None if self.adjustment is None else self.adjustment.tolist(),
            "price_move_distribution": self.price_move_distribution,
            "diagnostics": asdict(self.diagnostics),
        }

    def save(self, path: Path) -> None:
        path.write_text(
            json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
