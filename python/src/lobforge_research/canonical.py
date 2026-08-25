"""Platform-independent typed logical-row hashing."""

from __future__ import annotations

import hashlib
import json
import math
from collections.abc import Mapping, Sequence
from typing import Any

import numpy as np
import pyarrow as pa


def canonical_typed_bytes(value: Any) -> bytes:
    """Encode JSON-like typed values without relying on JSON float formatting."""

    if value is None:
        return b"n;"
    if isinstance(value, bool):
        return b"b1;" if value else b"b0;"
    if isinstance(value, int):
        return f"i{value};".encode("ascii")
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("canonical floats must be finite")
        return f"f{value.hex()};".encode("ascii")
    if isinstance(value, str):
        encoded = value.encode("utf-8")
        return f"s{len(encoded)}:".encode("ascii") + encoded + b";"
    if isinstance(value, Mapping):
        result = bytearray(b"{")
        for key in sorted(value):
            if not isinstance(key, str):
                raise TypeError("canonical mapping keys must be strings")
            result.extend(canonical_typed_bytes(key))
            result.extend(canonical_typed_bytes(value[key]))
        result.extend(b"}")
        return bytes(result)
    if isinstance(value, Sequence) and not isinstance(value, (bytes, bytearray)):
        result = bytearray(b"[")
        for item in value:
            result.extend(canonical_typed_bytes(item))
        result.extend(b"]")
        return bytes(result)
    raise TypeError(f"unsupported canonical type: {type(value).__name__}")


class LogicalDigest:
    """Incremental SHA-256 over length-delimited canonical typed rows."""

    def __init__(self) -> None:
        self._hash = hashlib.sha256()
        self.rows = 0

    def update(self, row: Mapping[str, Any]) -> None:
        # The Arrow schema fixes every logical field type. Canonical JSON gives a
        # platform-independent C-accelerated row encoding while still separating
        # integers, floats, booleans, nulls, strings, lists, and mappings.
        encoded = json.dumps(
            row,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        self._hash.update(len(encoded).to_bytes(8, "big"))
        self._hash.update(encoded)
        self.rows += 1

    def hexdigest(self) -> str:
        return self._hash.hexdigest()


class ColumnarLogicalDigest:
    """Batch-boundary-independent digest over a fixed Arrow logical schema."""

    def __init__(self, schema: pa.Schema) -> None:
        self.schema = schema
        self.rows = 0
        self._components: dict[str, Any] = {}

    def _hash(self, name: str, value: bytes) -> None:
        digest = self._components.setdefault(name, hashlib.sha256())
        digest.update(value)

    def _update_array(self, path: str, array: pa.Array) -> None:
        valid = np.asarray(array.is_valid().to_numpy(zero_copy_only=False), dtype=np.uint8)
        self._hash(f"{path}/valid", valid.tobytes())
        data_type = array.type
        if pa.types.is_integer(data_type) or pa.types.is_floating(data_type):
            filled = array.fill_null(0)
            values = np.asarray(filled.to_numpy(zero_copy_only=False))
            self._hash(f"{path}/values", values.astype(values.dtype.newbyteorder("<")).tobytes())
            return
        if pa.types.is_boolean(data_type):
            values = np.asarray(
                array.fill_null(False).to_numpy(zero_copy_only=False), dtype=np.uint8
            )
            self._hash(f"{path}/values", values.tobytes())
            return
        if pa.types.is_string(data_type) or pa.types.is_large_string(data_type):
            offset_buffer = array.buffers()[1]
            width = 8 if pa.types.is_large_string(data_type) else 4
            dtype = "<i8" if width == 8 else "<i4"
            if offset_buffer is None:
                offsets = np.zeros(len(array) + 1, dtype=np.int64)
            else:
                offsets = np.frombuffer(
                    offset_buffer,
                    dtype=dtype,
                    count=len(array) + 1,
                    offset=array.offset * width,
                ).astype(np.int64, copy=False)
            self._hash(
                f"{path}/lengths",
                np.diff(offsets).astype("<i8", copy=False).tobytes(),
            )
            data = array.buffers()[2]
            if data is not None and offsets.size:
                self._hash(
                    f"{path}/data",
                    data.slice(int(offsets[0]), int(offsets[-1] - offsets[0])).to_pybytes(),
                )
            return
        if pa.types.is_list(data_type) or pa.types.is_large_list(data_type):
            offsets = np.asarray(array.offsets.to_numpy(zero_copy_only=False), dtype=np.int64)
            self._hash(
                f"{path}/lengths",
                np.diff(offsets).astype("<i8", copy=False).tobytes(),
            )
            if offsets.size:
                values = array.values.slice(int(offsets[0]), int(offsets[-1] - offsets[0]))
                self._update_array(f"{path}/item", values)
            return
        if pa.types.is_struct(data_type):
            for index, field in enumerate(data_type):
                self._update_array(f"{path}/{field.name}", array.field(index))
            return
        raise TypeError(f"unsupported Arrow digest type: {data_type}")

    def update_table(self, table: pa.Table) -> None:
        if table.schema != self.schema:
            raise ValueError("logical digest schema mismatch")
        for field, column in zip(self.schema, table.columns, strict=True):
            for chunk in column.chunks:
                self._update_array(field.name, chunk)
        self.rows += table.num_rows

    def hexdigest(self) -> str:
        combined = hashlib.sha256()
        combined.update(self.schema.to_string().encode("utf-8"))
        for name in sorted(self._components):
            combined.update(len(name).to_bytes(4, "big"))
            combined.update(name.encode("utf-8"))
            combined.update(self._components[name].digest())
        combined.update(self.rows.to_bytes(8, "big"))
        return combined.hexdigest()
