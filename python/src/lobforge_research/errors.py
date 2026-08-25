"""Stable public failures and CLI exit codes."""

from __future__ import annotations

from dataclasses import dataclass

USAGE_ERROR = 2
INPUT_ERROR = 3
REPLAY_ERROR = 4
SCHEMA_ERROR = 5
OUTPUT_ERROR = 6
PROTOCOL_ERROR = 7
EVALUATION_ERROR = 8


@dataclass(slots=True)
class ResearchError(Exception):
    """An expected deterministic pipeline failure."""

    exit_code: int
    category: str
    detail: str

    def __str__(self) -> str:
        return f"{self.category}: {self.detail}"
