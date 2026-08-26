"""Command line entry point for deterministic Round 3 research workflows."""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path

from . import __version__
from .benchmark import benchmark_ndjson_pipeline, benchmark_vectorized_features
from .errors import ResearchError
from .ingest import build_dataset, freeze_protocol
from .reporting import write_synthetic_report
from .round4_reporting import analyze_round4_artifacts, write_round4_synthetic_report


def _default_protocol() -> Path:
    source_protocol = Path(__file__).resolve().parents[3] / "configs" / "round3_protocol.toml"
    return (
        source_protocol
        if source_protocol.is_file()
        else Path(__file__).with_name("round3_protocol.toml")
    )


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="lobforge-research")
    root.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    commands = root.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build-dataset")
    build.add_argument("--replay-cli", type=Path, required=True)
    build.add_argument("--input", type=Path, required=True)
    build.add_argument("--session-date", required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--depth", type=int, default=10)
    build.add_argument("--batch-rows", type=int, default=65_536)
    build.add_argument("--symbol", action="append", default=[])
    build.add_argument("--protocol", type=Path, default=_default_protocol())

    freeze = commands.add_parser("freeze-protocol")
    freeze.add_argument("--protocol", type=Path, default=_default_protocol())

    synthetic = commands.add_parser("synthetic-report")
    synthetic.add_argument("--output", type=Path, required=True)
    synthetic.add_argument("--seed", type=int, default=20260825)
    synthetic.add_argument("--rows-per-symbol-day", type=int, default=500)
    synthetic.add_argument("--protocol", type=Path, default=_default_protocol())

    benchmark = commands.add_parser("benchmark-features")
    benchmark.add_argument("--rows", type=int, default=1_000_000)
    benchmark.add_argument("--runs", type=int, default=3)
    benchmark.add_argument("--seed", type=int, default=20260825)
    benchmark.add_argument("--output", type=Path)
    pipeline_benchmark = commands.add_parser("benchmark-pipeline")
    pipeline_benchmark.add_argument("--rows", type=int, default=1_000_000)
    pipeline_benchmark.add_argument("--runs", type=int, default=3)
    pipeline_benchmark.add_argument("--batch-rows", type=int, default=65_536)
    pipeline_benchmark.add_argument("--output", type=Path)
    round4_synthetic = commands.add_parser("round4-synthetic-report")
    round4_synthetic.add_argument("--output", type=Path, required=True)
    round4_synthetic.add_argument("--seed", type=int, default=20260826)
    round4_analysis = commands.add_parser("round4-analyze")
    round4_analysis.add_argument("--input", type=Path, required=True)
    round4_analysis.add_argument("--output", type=Path, required=True)
    return root


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        if arguments.command == "build-dataset":
            manifest = build_dataset(
                replay_cli=arguments.replay_cli,
                input_path=arguments.input,
                session_date=arguments.session_date,
                output=arguments.output,
                depth=arguments.depth,
                batch_rows=arguments.batch_rows,
                symbols=tuple(arguments.symbol),
                protocol_path=arguments.protocol,
            )
            print(manifest["logical_digests"]["semantic"])
        elif arguments.command == "freeze-protocol":
            print(freeze_protocol(arguments.protocol))
        elif arguments.command == "synthetic-report":
            manifest = write_synthetic_report(
                arguments.output,
                seed=arguments.seed,
                rows_per_symbol_day=arguments.rows_per_symbol_day,
                protocol_path=arguments.protocol,
            )
            print(json.dumps(manifest, sort_keys=True))
        elif arguments.command == "benchmark-features":
            result = benchmark_vectorized_features(
                rows=arguments.rows, runs=arguments.runs, seed=arguments.seed
            )
            text = json.dumps(result, indent=2, sort_keys=True) + "\n"
            if arguments.output is None:
                sys.stdout.write(text)
            else:
                arguments.output.write_text(text, encoding="utf-8", newline="\n")
        elif arguments.command == "benchmark-pipeline":
            result = benchmark_ndjson_pipeline(
                rows=arguments.rows,
                runs=arguments.runs,
                batch_rows=arguments.batch_rows,
            )
            text = json.dumps(result, indent=2, sort_keys=True) + "\n"
            if arguments.output is None:
                sys.stdout.write(text)
            else:
                arguments.output.write_text(text, encoding="utf-8", newline="\n")
        elif arguments.command == "round4-synthetic-report":
            result = write_round4_synthetic_report(arguments.output, seed=arguments.seed)
            print(json.dumps({"status": result["status"], "seed": result["seed"]}, sort_keys=True))
        elif arguments.command == "round4-analyze":
            result = analyze_round4_artifacts(arguments.input, arguments.output)
            print(result["semantic_digest"])
        return 0
    except ResearchError as error:
        print(str(error), file=sys.stderr)
        return error.exit_code
    except (FileExistsError, ValueError, OSError) as error:
        print(f"COMMAND_FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
