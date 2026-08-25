"""Streaming C++ subprocess ingestion into deterministic logical Parquet datasets."""

from __future__ import annotations

import hashlib
import importlib.metadata
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import tomllib
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from .canonical import ColumnarLogicalDigest, canonical_typed_bytes
from .errors import INPUT_ERROR, OUTPUT_ERROR, REPLAY_ERROR, ResearchError
from .labels import RESEARCH_SCHEMA, ResearchStreamBuilder
from .schema import BOOK_EVENT_SCHEMA, StreamValidator, parse_json_line


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
    except OSError as error:
        raise ResearchError(INPUT_ERROR, "SOURCE_READ_FAILED", str(error)) from error
    return digest.hexdigest(), size


def _dependency_versions() -> dict[str, str]:
    names = ("matplotlib", "numpy", "pyarrow", "scikit-learn", "scipy")
    versions: dict[str, str] = {}
    for name in names:
        try:
            versions[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            versions[name] = "unknown"
    return versions


class PartitionWriter:
    """Write bounded Arrow batches while hashing canonical logical rows."""

    def __init__(self, directory: Path, schema: pa.Schema, batch_rows: int) -> None:
        self.directory = directory
        self.schema = schema
        self.batch_rows = batch_rows
        self.buffer: list[dict[str, Any]] = []
        self.full_digest = ColumnarLogicalDigest(schema)
        self.partitions: list[dict[str, Any]] = []
        directory.mkdir(parents=True)

    def append(self, row: dict[str, Any]) -> None:
        self.buffer.append(row)
        if len(self.buffer) >= self.batch_rows:
            self._flush()

    def _flush(self, *, allow_empty: bool = False) -> None:
        if not self.buffer and not allow_empty:
            return
        table = pa.Table.from_pylist(self.buffer, schema=self.schema)
        part_digest = ColumnarLogicalDigest(self.schema)
        part_digest.update_table(table)
        self.full_digest.update_table(table)
        name = f"part-{len(self.partitions):05d}.parquet"
        pq.write_table(
            table,
            self.directory / name,
            compression="zstd",
            use_dictionary=False,
            write_statistics=True,
        )
        self.partitions.append(
            {
                "path": f"{self.directory.name}/{name}",
                "rows": len(self.buffer),
                "logical_sha256": part_digest.hexdigest(),
            }
        )
        self.buffer.clear()

    def finish(self) -> None:
        self._flush(allow_empty=not self.partitions)

    @property
    def rows(self) -> int:
        return self.full_digest.rows


def _write_json(path: Path, value: Mapping[str, Any]) -> None:
    text = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    path.write_text(text, encoding="utf-8", newline="\n")


def _protocol(path: Path) -> tuple[dict[str, Any], str]:
    try:
        raw = path.read_bytes()
        parsed = tomllib.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise ResearchError(INPUT_ERROR, "PROTOCOL_READ_FAILED", str(error)) from error
    return parsed, hashlib.sha256(raw).hexdigest()


def freeze_protocol(path: Path) -> str:
    """Return the exact-byte SHA-256 used to bind all final-test evaluation."""

    _, digest = _protocol(path)
    return digest


def _semantic_digest(book_digest: str, research_digest: str, protocol_digest: str) -> str:
    value = {
        "book_events": book_digest,
        "protocol": protocol_digest,
        "research_rows": research_digest,
    }
    return hashlib.sha256(canonical_typed_bytes(value)).hexdigest()


def _replay_version(replay_cli: Path) -> str:
    command = [str(replay_cli), "--version"]
    if replay_cli.suffix.lower() == ".py":
        command.insert(0, sys.executable)
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            check=False,
            encoding="utf-8",
            errors="replace",
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    value = completed.stdout.strip()
    if completed.returncode == 0 and value.startswith("lobforge_replay_") and "\n" not in value:
        return value
    return "unknown"


def _cleanup_temp(temp_directory: Path, parent: Path) -> None:
    try:
        resolved = temp_directory.resolve(strict=False)
        resolved.relative_to(parent.resolve(strict=True))
    except (OSError, ValueError):
        return
    shutil.rmtree(resolved, ignore_errors=True)


def build_dataset(
    *,
    replay_cli: Path,
    input_path: Path,
    session_date: str,
    output: Path,
    depth: int = 10,
    batch_rows: int = 65_536,
    symbols: tuple[str, ...] = (),
    protocol_path: Path,
) -> dict[str, Any]:
    """Build a complete dataset or leave no output directory on any failure."""

    if not input_path.is_file():
        raise ResearchError(INPUT_ERROR, "SOURCE_NOT_FOUND", str(input_path))
    if not replay_cli.is_file():
        raise ResearchError(INPUT_ERROR, "REPLAY_NOT_FOUND", str(replay_cli))
    if not 1 <= depth <= 10 or batch_rows < 1:
        raise ResearchError(INPUT_ERROR, "INVALID_BUILD_ARGUMENT", "depth or batch size")
    if output.exists():
        raise ResearchError(OUTPUT_ERROR, "OUTPUT_EXISTS", str(output))

    source_digest, source_size = sha256_file(input_path)
    protocol, protocol_digest = _protocol(protocol_path)
    replay_version = _replay_version(replay_cli)
    output_parent = output.parent.resolve()
    try:
        output_parent.mkdir(parents=True, exist_ok=True)
        temp_directory = Path(tempfile.mkdtemp(prefix=f".{output.name}.tmp-", dir=output_parent))
    except OSError as error:
        raise ResearchError(OUTPUT_ERROR, "OUTPUT_CREATE_FAILED", str(error)) from error

    book_writer = PartitionWriter(temp_directory / "book_events", BOOK_EVENT_SCHEMA, batch_rows)
    research_writer = PartitionWriter(temp_directory / "research_rows", RESEARCH_SCHEMA, batch_rows)
    validator = StreamValidator(session_date, source_size)
    builder = ResearchStreamBuilder()
    command = []
    if replay_cli.suffix.lower() == ".py":
        command.append(sys.executable)
    command.extend(
        [
            str(replay_cli),
            "--input",
            str(input_path),
            "--session-date",
            session_date,
            "--strict",
            "--research-format",
            "book-event-v1",
            "--depth",
            str(depth),
            "--output",
            "-",
        ]
    )
    for symbol in symbols:
        command.extend(["--symbol", symbol])

    stderr_file = tempfile.TemporaryFile(mode="w+b")  # noqa: SIM115
    process: subprocess.Popen[str] | None = None
    try:
        environment = os.environ.copy()
        environment["LC_ALL"] = "C"
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=stderr_file,
            text=True,
            encoding="utf-8",
            errors="strict",
            bufsize=1,
            env=environment,
        )
        assert process.stdout is not None
        minimum_timestamp: int | None = None
        maximum_timestamp: int | None = None
        observed_symbols: set[str] = set()
        for line_number, line in enumerate(process.stdout, start=1):
            if not line.endswith("\n"):
                raise ResearchError(5, "SCHEMA_VIOLATION", "unterminated NDJSON line")
            wire_row = parse_json_line(line[:-1], line_number)
            event = validator.accept(wire_row)
            if event is None:
                continue
            timestamp = int(event["timestamp_ns"])
            minimum_timestamp = (
                timestamp if minimum_timestamp is None else min(minimum_timestamp, timestamp)
            )
            maximum_timestamp = (
                timestamp if maximum_timestamp is None else max(maximum_timestamp, timestamp)
            )
            observed_symbols.add(str(event["symbol"]))
            book_writer.append(event)
            builder.accept(event, research_writer.append)
        return_code = process.wait()
        if return_code != 0:
            stderr_file.seek(0)
            detail = stderr_file.read(65_536).decode("utf-8", errors="replace").strip()
            raise ResearchError(
                REPLAY_ERROR,
                "REPLAY_FAILED",
                f"exit={return_code}; stderr={detail}",
            )
        validator.finish()
        builder.finish(research_writer.append)
        book_writer.finish()
        research_writer.finish()
        assert validator.header is not None and validator.summary is not None

        package_root = Path(__file__).resolve().parents[2]
        lock_path = package_root / "uv.lock"
        lock_digest = sha256_file(lock_path)[0] if lock_path.is_file() else "unknown"
        semantic_digest = _semantic_digest(
            book_writer.full_digest.hexdigest(),
            research_writer.full_digest.hexdigest(),
            protocol_digest,
        )
        arguments: dict[str, Any] = {
            "session_date": session_date,
            "strict": True,
            "research_format": "book-event-v1",
            "depth": depth,
            "batch_rows": batch_rows,
            "symbols": list(symbols),
            "input_name": input_path.name,
            "replay_binary_name": replay_cli.name,
            "output_name": output.name,
        }
        manifest: dict[str, Any] = {
            "manifest_schema": "lobforge.research_manifest",
            "manifest_version": 1,
            "book_event_schema": "lobforge.book_event",
            "book_event_version": 1,
            "research_schema": "lobforge.research_rows",
            "research_version": 1,
            "source": {
                "sha256": source_digest,
                "size": source_size,
                "session_date": session_date,
            },
            "replay": {
                "factual_book_digest": validator.summary["factual_book_digest"],
                "binary": replay_cli.name,
                "version": replay_version,
                "commit": "unknown",
            },
            "arguments": arguments,
            "symbols": sorted(observed_symbols),
            "depth": depth,
            "price_scale": validator.header["price_scale"],
            "timestamp_unit": validator.header["timestamp_unit"],
            "python": {
                "version": platform.python_version(),
                "implementation": platform.python_implementation(),
                "dependencies": _dependency_versions(),
                "lock_sha256": lock_digest,
            },
            "batch_rows": batch_rows,
            "rows": {
                "book_events": book_writer.rows,
                "research_rows": research_writer.rows,
                "errors": validator.summary["errors"],
            },
            "time_range_ns": {"minimum": minimum_timestamp, "maximum": maximum_timestamp},
            "protocol": {"sha256": protocol_digest, "config": protocol},
            "partitions": {
                "book_events": book_writer.partitions,
                "research_rows": research_writer.partitions,
            },
            "logical_digests": {
                "book_events": book_writer.full_digest.hexdigest(),
                "research_rows": research_writer.full_digest.hexdigest(),
                "semantic": semantic_digest,
            },
        }
        metrics: dict[str, Any] = {
            "metrics_schema": "lobforge.round3_metrics",
            "version": 1,
            "semantic_digest": semantic_digest,
            "book_event_rows": book_writer.rows,
            "research_rows": research_writer.rows,
            "parser_errors": validator.summary["errors"],
            "real_data": {
                "D1": "BLOCKED: PROVENANCE_UNVERIFIED",
                "D2": "BLOCKED",
                "D3": "BLOCKED",
                "H1": "BLOCKED",
            },
        }
        _write_json(temp_directory / "manifest.json", manifest)
        _write_json(temp_directory / "metrics.json", metrics)
        temp_directory.replace(output)
        return manifest
    except ResearchError:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        _cleanup_temp(temp_directory, output_parent)
        raise
    except (OSError, UnicodeError, pa.ArrowException, ValueError) as error:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        _cleanup_temp(temp_directory, output_parent)
        raise ResearchError(OUTPUT_ERROR, "DATASET_BUILD_FAILED", str(error)) from error
    finally:
        stderr_file.close()
