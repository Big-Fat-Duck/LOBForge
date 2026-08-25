"""Round 3 performance measurements with deterministic pre-generated inputs."""

from __future__ import annotations

import importlib.metadata
import os
import platform
import statistics
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow.json as paj
import pyarrow.parquet as pq

from .arrow_ingest import WIRE_SCHEMA, ArrowStreamValidator
from .canonical import ColumnarLogicalDigest
from .features import cont_ofi_vectorized, static_features_vectorized
from .schema import BOOK_EVENT_SCHEMA


def _peak_rss_bytes() -> int | None:
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class ProcessMemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        get_current_process = ctypes.windll.kernel32.GetCurrentProcess
        get_current_process.restype = wintypes.HANDLE
        get_memory_info = ctypes.windll.psapi.GetProcessMemoryInfo
        get_memory_info.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(ProcessMemoryCounters),
            wintypes.DWORD,
        ]
        get_memory_info.restype = wintypes.BOOL
        process = get_current_process()
        success = get_memory_info(process, ctypes.byref(counters), counters.cb)
        return int(counters.PeakWorkingSetSize) if success else None
    try:
        import resource

        getrusage = getattr(resource, "getrusage")  # noqa: B009
        usage_self = getattr(resource, "RUSAGE_SELF")  # noqa: B009
        value = int(getrusage(usage_self).ru_maxrss)
        return value if platform.system() == "Darwin" else value * 1024
    except (ImportError, OSError):
        return None


def benchmark_vectorized_features(
    *, rows: int = 1_000_000, runs: int = 3, seed: int = 20260825
) -> dict[str, Any]:
    if rows < 100_000 or runs < 3:
        raise ValueError("full benchmark requires >=100,000 rows and >=3 runs")
    generator = np.random.default_rng(seed)
    base_bid = generator.integers(1_000_000, 2_000_000, size=(rows, 1), dtype=np.int64)
    offsets = np.arange(10, dtype=np.int64)[None, :] * 100
    bid_prices = base_bid - offsets
    ask_prices = base_bid + 100 + offsets
    bid_quantities = generator.integers(1, 10_000, size=(rows, 10), dtype=np.int64)
    ask_quantities = generator.integers(1, 10_000, size=(rows, 10), dtype=np.int64)

    static_features_vectorized(bid_prices, ask_prices, bid_quantities, ask_quantities)
    cont_ofi_vectorized(
        bid_prices[:, 0], bid_quantities[:, 0], ask_prices[:, 0], ask_quantities[:, 0]
    )
    timings: list[float] = []
    checksum = 0.0
    for _ in range(runs):
        start = time.perf_counter()
        features = static_features_vectorized(
            bid_prices, ask_prices, bid_quantities, ask_quantities
        )
        ofi = cont_ofi_vectorized(
            bid_prices[:, 0], bid_quantities[:, 0], ask_prices[:, 0], ask_quantities[:, 0]
        )
        elapsed = time.perf_counter() - start
        timings.append(elapsed)
        checksum += float(np.asarray(features["mid2"])[-1] + ofi[-1])
    median = statistics.median(timings)
    return {
        "benchmark": "vectorized_feature_transform",
        "rows": rows,
        "warmup_runs": 1,
        "measured_runs": runs,
        "seconds": timings,
        "median_seconds": median,
        "rows_per_second": rows / median,
        "threshold_rows_per_second": 1_000_000,
        "passes_threshold": rows / median >= 1_000_000,
        "peak_rss_bytes": _peak_rss_bytes(),
        "rss_limit_bytes": 1_610_612_736,
        "checksum": checksum,
        "environment": {
            "cpu": platform.processor() or "unknown",
            "os": platform.platform(),
            "python": platform.python_version(),
            "numpy": importlib.metadata.version("numpy"),
            "pyarrow": importlib.metadata.version("pyarrow"),
            "batch_size": None,
            "seed": seed,
        },
    }


def _write_benchmark_ndjson(path: Path, rows: int) -> int:
    source_size = rows * 40
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(
            '{"record_type":"header","schema":"lobforge.book_event","version":1,'
            '"session_date":"2026-08-24","price_scale":10000,'
            f'"timestamp_unit":"ns_since_midnight","depth":1,"source_size":{source_size}}}\n'
        )
        for index in range(1, rows + 1):
            timestamp = 1_000_000_000 + index * 1_000_000
            bid_quantity = 100 + index % 17
            ask_quantity = 100 + index % 19
            stream.write(
                '{"record_type":"book_event","session_date":"2026-08-24",'
                f'"sequence":{index},"source_offset":{index * 40},'
                f'"timestamp_ns":{timestamp},"stock_locate":1,"symbol":"SYNTH",'
                f'"message_type":"A","action":"add","side":"B","order_ref":{index},'
                '"new_order_ref":null,"event_qty":1,"display_price4":1000000,'
                '"execution_price4":null,"match_number":null,'
                '"session_state":"market_hours","trading_state":"T",'
                '"two_sided":true,"locked":false,"crossed":false,'
                f'"bids":[[1000000,{bid_quantity}]],"asks":[[1000100,{ask_quantity}]]}}\n'
            )
        stream.write(
            '{"record_type":"summary","schema":"lobforge.book_event","version":1,'
            f'"input_bytes":{source_size},"records_seen":{rows},'
            f'"records_decoded":{rows},"records_applied":{rows},'
            f'"records_output":{rows},"records_skipped":0,"errors":0,'
            '"factual_book_digest":"0000000000000000"}\n'
        )
    return source_size


def benchmark_ndjson_pipeline(
    *, rows: int = 1_000_000, runs: int = 3, batch_rows: int = 65_536
) -> dict[str, Any]:
    """Time strict batch JSON parsing, vector validation, logical digest, and Parquet."""

    if rows < 100_000 or runs < 3 or batch_rows < 1:
        raise ValueError("full pipeline benchmark requires >=100,000 rows and >=3 runs")
    timings: list[float] = []
    digests: list[str] = []
    with tempfile.TemporaryDirectory(prefix="lobforge-r3-benchmark-") as temporary:
        root = Path(temporary)
        ndjson = root / "input.ndjson"
        source_size = _write_benchmark_ndjson(ndjson, rows)
        for run in range(runs + 1):
            output = root / f"output-{run}.parquet"
            validator = ArrowStreamValidator("2026-08-24", source_size)
            digest = ColumnarLogicalDigest(BOOK_EVENT_SCHEMA)
            start = time.perf_counter()
            reader = paj.open_json(
                ndjson,
                read_options=paj.ReadOptions(block_size=max(1 << 20, batch_rows * 512)),
                parse_options=paj.ParseOptions(
                    explicit_schema=WIRE_SCHEMA,
                    unexpected_field_behavior="error",
                ),
            )
            with pq.ParquetWriter(
                output,
                BOOK_EVENT_SCHEMA,
                compression="zstd",
                use_dictionary=False,
            ) as writer:
                for batch in reader:
                    table = validator.accept(batch)
                    if table is not None and table.num_rows:
                        digest.update_table(table)
                        writer.write_table(table, row_group_size=batch_rows)
            validator.finish()
            elapsed = time.perf_counter() - start
            if run > 0:
                timings.append(elapsed)
                digests.append(digest.hexdigest())
    median = statistics.median(timings)
    rss = _peak_rss_bytes()
    return {
        "benchmark": "ndjson_vector_validation_logical_digest_parquet",
        "rows": rows,
        "batch_rows": batch_rows,
        "warmup_runs": 1,
        "measured_runs": runs,
        "seconds": timings,
        "median_seconds": median,
        "rows_per_second": rows / median,
        "threshold_rows_per_second": 100_000,
        "passes_throughput_threshold": rows / median >= 100_000,
        "peak_rss_bytes": rss,
        "rss_limit_bytes": 1_610_612_736,
        "passes_rss_threshold": rss is not None and rss <= 1_610_612_736,
        "logical_digest_stable": len(set(digests)) == 1,
        "logical_digest": digests[0],
        "environment": {
            "cpu": platform.processor() or "unknown",
            "os": platform.platform(),
            "python": platform.python_version(),
            "numpy": importlib.metadata.version("numpy"),
            "pyarrow": importlib.metadata.version("pyarrow"),
        },
    }
