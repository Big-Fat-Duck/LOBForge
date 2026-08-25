from __future__ import annotations

import json
import os
import sys
from copy import deepcopy
from pathlib import Path

import pyarrow.json as paj
import pyarrow.parquet as pq
import pytest

from lobforge_research.arrow_ingest import WIRE_SCHEMA, ArrowStreamValidator
from lobforge_research.canonical import ColumnarLogicalDigest
from lobforge_research.errors import ResearchError
from lobforge_research.ingest import build_dataset, freeze_protocol
from lobforge_research.schema import BOOK_EVENT_SCHEMA, StreamValidator, parse_json_line

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "tests" / "fixtures" / "book_event_v1_golden.ndjson"
PROTOCOL = ROOT / "configs" / "round3_protocol.toml"


def records():
    return [json.loads(line) for line in GOLDEN.read_text(encoding="utf-8").splitlines()]


def validate(stream):
    validator = StreamValidator("2026-08-24", 810)
    output = []
    for row in stream:
        value = validator.accept(row)
        if value is not None:
            output.append(value)
    validator.finish()
    return validator, output


def test_golden_stream_validates_and_normalizes_depth():
    validator, output = validate(records())
    assert validator.event_count == 7
    assert validator.summary["factual_book_digest"] == "d3f11ab4fe308743"
    assert output[1]["bids"][0] == {"price4": 1234500, "aggregate_qty": 100}
    assert parse_json_line('{"record_type":"header"}', 1)["record_type"] == "header"
    with pytest.raises(ResearchError):
        parse_json_line("not-json", 1)
    with pytest.raises(ResearchError):
        parse_json_line("[]", 1)


def test_vectorized_arrow_validator_matches_scalar_and_batch_digest():
    scalar_validator, scalar = validate(records())
    del scalar_validator
    validator = ArrowStreamValidator("2026-08-24", 810)
    tables = []
    reader = paj.open_json(
        GOLDEN,
        read_options=paj.ReadOptions(block_size=1024),
        parse_options=paj.ParseOptions(
            explicit_schema=WIRE_SCHEMA, unexpected_field_behavior="error"
        ),
    )
    digest = ColumnarLogicalDigest(BOOK_EVENT_SCHEMA)
    for batch in reader:
        table = validator.accept(batch)
        if table is not None:
            tables.append(table)
            digest.update_table(table)
    validator.finish()
    assert [row for table in tables for row in table.to_pylist()] == scalar
    assert validator.event_count == 7
    assert len(digest.hexdigest()) == 64


@pytest.mark.parametrize(
    ("index", "field", "value"),
    [
        (0, "version", 2),
        (0, "session_date", "2026-08-25"),
        (0, "source_size", 999),
        (1, "sequence", 2),
        (2, "source_offset", 1),
        (2, "timestamp_ns", 1),
        (1, "stock_locate", 0),
        (1, "order_ref", True),
        (1, "new_order_ref", 2),
        (1, "execution_price4", 1),
        (1, "match_number", 1),
        (1, "trading_state", "Z"),
        (1, "two_sided", True),
        (-1, "records_output", 8),
        (-1, "factual_book_digest", "INVALID"),
    ],
)
def test_strict_schema_negative_cases(index, field, value):
    stream = records()
    stream[index][field] = value
    with pytest.raises(ResearchError):
        validate(stream)


def test_schema_order_depth_summary_and_lifecycle_failures():
    stream = records()
    reordered = {"schema": stream[0]["schema"], **stream[0]}
    stream[0] = reordered
    with pytest.raises(ResearchError):
        validate(stream)
    stream = records()
    stream[2]["bids"] = [[1234400, 1], [1234500, 1]]
    with pytest.raises(ResearchError):
        validate(stream)
    stream = records()
    stream[-1]["records_applied"] = stream[-1]["records_decoded"] + 1
    with pytest.raises(ResearchError):
        validate(stream)
    for stream in (
        records()[1:],
        records()[:-1],
        [records()[0], records()[0], *records()[1:]],
        [*records(), records()[1]],
        [dict(record_type="unknown")],
    ):
        with pytest.raises(ResearchError):
            validate(stream)


def _write_replay_script(tmp_path: Path, stream, *, exit_code: int = 0) -> tuple[Path, Path]:
    source = tmp_path / "source.itch"
    source.write_bytes(b"x" * 810)
    ndjson = tmp_path / "stream.ndjson"
    ndjson.write_text(
        "\n".join(json.dumps(row, separators=(",", ":")) for row in stream) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    script = tmp_path / "fake_replay.py"
    script.write_text(
        "from pathlib import Path\n"
        "import sys\n"
        f"sys.stdout.write(Path({str(ndjson)!r}).read_text(encoding='utf-8'))\n"
        f"sys.stderr.write('fake failure\\n' if {exit_code} else '')\n"
        f"raise SystemExit({exit_code})\n",
        encoding="utf-8",
        newline="\n",
    )
    return script, source


def test_streaming_ingest_parquet_manifest_atomic_and_ten_run_digest(tmp_path):
    script, source = _write_replay_script(tmp_path, records())
    digests = []
    for run in range(10):
        output = tmp_path / f"dataset-{run}"
        manifest = build_dataset(
            replay_cli=script,
            input_path=source,
            session_date="2026-08-24",
            output=output,
            depth=10,
            batch_rows=1 if run % 2 else 65_536,
            symbols=(),
            protocol_path=PROTOCOL,
        )
        digests.append(manifest["logical_digests"]["semantic"])
        assert (output / "manifest.json").is_file()
        assert (output / "metrics.json").is_file()
        assert manifest["rows"] == {"book_events": 7, "research_rows": 5, "errors": 0}
        assert pq.read_table(output / "book_events").num_rows == 7
        assert pq.read_table(output / "research_rows").num_rows == 5
    assert len(set(digests)) == 1
    assert freeze_protocol(PROTOCOL) == manifest["protocol"]["sha256"]
    with pytest.raises(ResearchError) as exists:
        build_dataset(
            replay_cli=script,
            input_path=source,
            session_date="2026-08-24",
            output=tmp_path / "dataset-0",
            protocol_path=PROTOCOL,
        )
    assert exists.value.exit_code == 6


@pytest.mark.parametrize("mutation", ["truncated", "version", "sequence", "range"])
def test_ingest_rejects_every_bad_stream_without_partial_output(tmp_path, mutation):
    stream = deepcopy(records())
    if mutation == "truncated":
        stream.pop()
    elif mutation == "version":
        stream[0]["version"] = 2
    elif mutation == "sequence":
        stream[2]["sequence"] = 1
    else:
        stream[1]["order_ref"] = 0
    script, source = _write_replay_script(tmp_path, stream)
    output = tmp_path / "dataset"
    with pytest.raises(ResearchError) as captured:
        build_dataset(
            replay_cli=script,
            input_path=source,
            session_date="2026-08-24",
            output=output,
            protocol_path=PROTOCOL,
        )
    assert captured.value.exit_code == 5
    assert not output.exists()


def test_ingest_distinguishes_subprocess_and_input_failures(tmp_path):
    script, source = _write_replay_script(tmp_path, records(), exit_code=9)
    with pytest.raises(ResearchError) as replay:
        build_dataset(
            replay_cli=script,
            input_path=source,
            session_date="2026-08-24",
            output=tmp_path / "out",
            protocol_path=PROTOCOL,
        )
    assert replay.value.exit_code == 4
    with pytest.raises(ResearchError) as missing:
        build_dataset(
            replay_cli=Path(sys.executable),
            input_path=tmp_path / "missing",
            session_date="2026-08-24",
            output=tmp_path / "out2",
            protocol_path=PROTOCOL,
        )
    assert missing.value.exit_code == 3


@pytest.mark.skipif(
    not (Path(os.environ.get("LOBFORGE_REPLAY_CLI", ""))).is_file(),
    reason="set LOBFORGE_REPLAY_CLI after the C++ build",
)
def test_real_cpp_exporter_to_parquet_integration(tmp_path):
    replay = Path(os.environ["LOBFORGE_REPLAY_CLI"])
    source = replay.parent / "synthetic_full_session.itch"
    manifest = build_dataset(
        replay_cli=replay,
        input_path=source,
        session_date="2026-08-24",
        output=tmp_path / "actual",
        protocol_path=PROTOCOL,
    )
    assert manifest["replay"]["factual_book_digest"] == "d3f11ab4fe308743"
