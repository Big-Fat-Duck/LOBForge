"""Deterministic analysis/report output for Round 4 artifacts and controls."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from .canonical import canonical_typed_bytes
from .round4_evaluation import validate_artifact_directory
from .round4_synthetic import run_round4_synthetic_controls


def _atomic_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    temporary.replace(path)


def write_round4_synthetic_report(output: Path, *, seed: int = 20260826) -> dict[str, Any]:
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    controls = run_round4_synthetic_controls(seed)
    _atomic_json(output / "metrics.json", controls)

    labels = [row["scenario"] for row in controls["scenarios"]]
    values = [1 if row["status"] == "PASS" else 0 for row in controls["scenarios"]]
    figure, axis = plt.subplots(figsize=(9, 4.5))
    axis.bar(range(len(labels)), values, color="#2f6f8f")
    axis.set_xticks(range(len(labels)), labels, rotation=35, ha="right")
    axis.set_ylim(0, 1.15)
    axis.set_ylabel("pre-registered assertion passed")
    axis.set_title("LOBForge Round 4 deterministic synthetic controls")
    figure.tight_layout()
    figure.savefig(output / "synthetic_controls.png", dpi=120, metadata={"Software": "LOBForge"})
    plt.close(figure)

    report = "\n".join(
        [
            "# Round 4 synthetic control report",
            "",
            f"Overall status: `{controls['status']}`.",
            "",
            "These deterministic scenarios validate implementation mechanics only. They do not",
            "establish real-market fills, alpha, profitability, causality, or production",
            "readiness.",
            "",
            *[f"- {row['scenario']}: {row['status']}" for row in controls["scenarios"]],
            "",
            "F1, F2, F3 and P1: `BLOCKED: DATASET_NOT_PROVIDED`.",
            "",
        ]
    )
    (output / "report.md").write_text(report, encoding="utf-8", newline="\n")
    return controls


def analyze_round4_artifacts(input_path: Path, output: Path) -> dict[str, Any]:
    """Validate immutable simulator output and publish only derived analysis."""

    if output.exists():
        raise FileExistsError(output)
    validated = validate_artifact_directory(input_path)
    output.mkdir(parents=True)
    result = {
        "schema": "lobforge.round4_analysis",
        "version": 1,
        "protocol_sha256": validated["manifest"]["protocol_sha256"],
        "semantic_digest": validated["manifest"]["semantic_digest"],
        "logical_digests": validated["logical_digests"],
        "summary": validated["summary"],
        "source_analysis_digest": hashlib.sha256(
            canonical_typed_bytes(validated["summary"])
        ).hexdigest(),
        "evidence_scope": "counterfactual mechanics; real evidence remains separately gated",
    }
    _atomic_json(output / "metrics.json", result)
    return result
