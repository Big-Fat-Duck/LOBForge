#!/usr/bin/env python3
"""Dependency-free, redacting release audit and deterministic manifest generator.

The scanner never prints matched text. Findings contain only a rule name, a repository-relative
path, a line number, a scope and an object/fingerprint identifier. The canonical manifest digest
is computed from logical release content and deliberately excludes generation time and the
manifest's own physical bytes.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import urllib.parse
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = PurePosixPath("artifacts/round4_5/release_manifest.json")
LARGE_FILE_BYTES = 10 * 1024 * 1024
BLOCKING_FILE_BYTES = 50 * 1024 * 1024
APACHE_2_LICENSE_SHA256 = "4224e35b95f09142566a33614347ac43be0ad3e14b2be7c102cf54d2f8762a06"

CANONICAL_MANIFEST_KEYS = (
    "schema",
    "version",
    "release_candidate",
    "release_files",
    "summary",
    "manifest_hash_policy",
    "protocols",
    "round1_to_round4_contracts",
    "key_semantic_digests",
    "content_assertions",
)

PHYSICAL_MANIFEST_KEYS = {
    "generated_at_utc",
    "generated_at_excluded_from_canonical_digest",
    "physical_manifest_bytes_excluded_from_canonical_digest",
    "canonical_manifest_digest",
    "release_scope",
    "audit_summary",
    "largest_source_file_excluding_manifest",
    "physical_manifest_bytes",
    "physical_total_release_bytes",
}

FORBIDDEN_SUFFIXES = {
    ".arrow",
    ".dbn",
    ".feather",
    ".itch",
    ".joblib",
    ".key",
    ".onnx",
    ".p12",
    ".parquet",
    ".pcap",
    ".pcapng",
    ".pem",
    ".pickle",
    ".pkl",
}

EXCLUDED_PARTS = {
    ".git",
    ".idea",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
    ".venv",
    ".vscode",
    "__pycache__",
}

SOURCE_PREFIXES = {
    ".github",
    "apps",
    "artifacts/round4_5",
    "benchmarks",
    "configs",
    "docs",
    "fuzz",
    "include",
    "python",
    "src",
    "tests",
    "tools",
}

ROOT_SOURCE_FILES = {
    ".clang-format",
    ".gitattributes",
    ".gitignore",
    "CMakeLists.txt",
    "LICENSE",
    "LICENSE.md",
    "LICENSE.txt",
    "NOTICE",
    "README.md",
    "THIRD_PARTY_NOTICES.md",
}

PLACEHOLDER_WORDS = {
    "changeme",
    "dummy",
    "example",
    "placeholder",
    "redacted",
    "replace_me",
    "test-only",
    "unknown",
}


@dataclass(frozen=True, order=True)
class Finding:
    rule: str
    path: str
    line: int
    scope: str
    object_id: str


def run_git(*args: str, check: bool = True) -> str:
    completed = subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
    )
    if check and completed.returncode != 0:
        message = completed.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(args)} failed: {message}")
    return completed.stdout.decode("utf-8", errors="surrogateescape")


def git_lines(*args: str) -> list[str]:
    return [line for line in run_git(*args).splitlines() if line]


def normalized(path: str | Path | PurePosixPath) -> str:
    return str(PurePosixPath(str(path).replace("\\", "/")))


def is_project_source(path: str) -> bool:
    pure = PurePosixPath(path)
    if any(part in EXCLUDED_PARTS or part.startswith("build") for part in pure.parts):
        return False
    if "artifacts" in pure.parts and path != normalized(MANIFEST_PATH):
        return False
    if path in ROOT_SOURCE_FILES:
        return True
    return any(path == prefix or path.startswith(prefix + "/") for prefix in SOURCE_PREFIXES)


def discover_release_files() -> tuple[list[str], set[str], set[str], list[str]]:
    tracked = set(git_lines("ls-files"))
    untracked = set(git_lines("ls-files", "--others", "--exclude-standard"))
    ignored = git_lines("ls-files", "--others", "--ignored", "--exclude-standard")
    candidates = sorted(
        path
        for path in tracked | untracked
        if is_project_source(path)
        and (REPO_ROOT / Path(path)).is_file()
        and not (path.startswith("artifacts/") and path != normalized(MANIFEST_PATH))
    )
    return candidates, tracked, untracked, ignored


def scope_classification_findings(
    files: Iterable[str], tracked: set[str], untracked: set[str]
) -> list[Finding]:
    release_paths = set(files)
    findings = [
        Finding("tracked_file_outside_release_scope", path, 0, "release", "scope")
        for path in sorted(tracked - release_paths)
    ]
    findings.extend(
        Finding("untracked_file_outside_release_scope", path, 0, "release", "scope")
        for path in sorted(untracked - release_paths)
    )
    return findings


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def text_and_lines(data: bytes) -> tuple[str, list[str]] | None:
    if b"\0" in data[:8192]:
        return None
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    return text, text.splitlines()


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


SECRET_RULES: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "private_key",
        re.compile(r"-{5}BEGIN (?:OPENSSH |RSA |EC |DSA )?PRIVATE KEY-{5}"),
    ),
    ("github_token", re.compile(r"\bgh[pousr]_[A-Za-z0-9_]{20,}\b")),
    ("github_fine_grained_token", re.compile(r"\bgithub_pat_[A-Za-z0-9_]{40,}\b")),
    ("gitlab_token", re.compile(r"\bglpat-[A-Za-z0-9_-]{20,}\b")),
    ("aws_access_key", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
    ("google_api_key", re.compile(r"\bAIza[A-Za-z0-9_-]{30,}\b")),
    ("openai_api_key", re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{20,}\b")),
    ("anthropic_api_key", re.compile(r"\bsk-ant-[A-Za-z0-9_-]{20,}\b")),
    ("slack_token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b")),
    ("npm_token", re.compile(r"\bnpm_[A-Za-z0-9]{30,}\b")),
    ("pypi_token", re.compile(r"\bpypi-[A-Za-z0-9_-]{40,}\b")),
    (
        "jwt",
        re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b"),
    ),
    ("bearer_token", re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{24,}", re.IGNORECASE)),
    (
        "credential_url",
        re.compile(r"\b(?:https?|postgres(?:ql)?|mysql|mongodb(?:\+srv)?):\/\/[^\s/@:]+:[^\s/@]+@"),
    ),
    (
        "generic_secret_assignment",
        re.compile(
            r"\b(?:api[_-]?key|access[_-]?token|client[_-]?secret|password|passwd|secret)"
            r"\s*[:=]\s*[\"']([^\"']{8,})[\"']",
            re.IGNORECASE,
        ),
    ),
    (
        "temporary_signed_url",
        re.compile(r"[?&](?:X-Amz-Signature|sig|signature|token)=[A-Za-z0-9%._~+/=-]{16,}"),
    ),
    (
        "personal_email",
        re.compile(r"\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b", re.IGNORECASE),
    ),
    (
        "telephone_number",
        re.compile(r"(?<!\d)(?:\+\d{1,3}[ .-])?(?:\(\d{2,4}\)|\d{3})[ .-]\d{3,4}[ .-]\d{4}(?!\d)"),
    ),
    (
        "user_home_path",
        re.compile(
            r"(?<![A-Za-z0-9+.-])(?:[A-Za-z]:[\\/]Users[\\/][^\\/\s]+|"
            r"/" + r"Users/[^/\s]+|/" + r"home/[^/\s]+)"
        ),
    ),
)

ABSOLUTE_PATH_RULES: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "windows_absolute_path",
        re.compile(r"(?<![A-Za-z0-9+.-])[A-Za-z]:[\\/](?![\\/])"),
    ),
    ("wsl_mount_path", re.compile(r"(?<![A-Za-z0-9+.-])/mnt/[a-z]/")),
    ("unix_home_path", re.compile(r"(?<![A-Za-z0-9+.-])/(?:home|Users)/[^/\s]+/")),
    ("root_home_path", re.compile(r"(?<![A-Za-z0-9+.-])/" + r"root/")),
    ("file_uri", re.compile(r"file:" + r"//", re.IGNORECASE)),
)


def is_placeholder(match: re.Match[str]) -> bool:
    lowered = match.group(0).lower()
    return any(word in lowered for word in PLACEHOLDER_WORDS) or "${" in lowered or "<" in lowered


def scan_text(
    text: str,
    path: str,
    scope: str,
    object_id: str,
    rules: Iterable[tuple[str, re.Pattern[str]]],
    *,
    placeholders_allowed: bool,
) -> list[Finding]:
    findings: list[Finding] = []
    for rule_name, pattern in rules:
        for match in pattern.finditer(text):
            if placeholders_allowed and is_placeholder(match):
                continue
            findings.append(
                Finding(
                    rule=rule_name,
                    path=path,
                    line=line_number(text, match.start()),
                    scope=scope,
                    object_id=object_id,
                )
            )
    return findings


def scan_worktree(files: Iterable[str]) -> tuple[list[Finding], list[Finding]]:
    secrets: list[Finding] = []
    paths: list[Finding] = []
    for relative in files:
        data = (REPO_ROOT / Path(relative)).read_bytes()
        decoded = text_and_lines(data)
        if decoded is None:
            continue
        text, _ = decoded
        object_id = sha256_bytes(data)[:16]
        secrets.extend(
            scan_text(
                text,
                relative,
                "worktree",
                object_id,
                SECRET_RULES,
                placeholders_allowed=True,
            )
        )
        paths.extend(
            scan_text(
                text,
                relative,
                "worktree",
                object_id,
                ABSOLUTE_PATH_RULES,
                placeholders_allowed=False,
            )
        )
    return sorted(set(secrets)), sorted(set(paths))


def all_git_blobs() -> list[tuple[str, str, int]]:
    known_paths: dict[str, str] = {}
    for line in git_lines("rev-list", "--objects", "--all"):
        oid, _, path = line.partition(" ")
        if path:
            known_paths.setdefault(oid, path)
    checked = subprocess.run(
        [
            "git",
            "cat-file",
            "--batch-all-objects",
            "--batch-check=%(objectname) %(objecttype) %(objectsize)",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        check=True,
    ).stdout.decode()
    result: list[tuple[str, str, int]] = []
    for line in checked.splitlines():
        oid, kind, size_text = line.split()
        if kind == "blob":
            result.append(
                (
                    oid,
                    known_paths.get(oid, "<unreachable-or-unknown-path>"),
                    int(size_text),
                )
            )
    return result


def scan_history() -> tuple[list[Finding], list[Finding], int]:
    secrets: list[Finding] = []
    paths: list[Finding] = []
    blobs = all_git_blobs()
    for oid, path, _size in blobs:
        completed = subprocess.run(
            ["git", "cat-file", "blob", oid],
            cwd=REPO_ROOT,
            capture_output=True,
            check=True,
        )
        text = completed.stdout.decode("utf-8", errors="ignore")
        secrets.extend(
            scan_text(
                text,
                path,
                "git_blob",
                oid[:16],
                SECRET_RULES,
                placeholders_allowed=True,
            )
        )
        paths.extend(
            scan_text(
                text,
                path,
                "git_blob",
                oid[:16],
                ABSOLUTE_PATH_RULES,
                placeholders_allowed=False,
            )
        )
    return sorted(set(secrets)), sorted(set(paths)), len(blobs)


def scan_index() -> tuple[list[Finding], list[Finding], int]:
    secrets: list[Finding] = []
    paths: list[Finding] = []
    entries = git_lines("ls-files", "-s")
    scanned = 0
    seen: set[tuple[str, str]] = set()
    for entry in entries:
        metadata, separator, path = entry.partition("\t")
        if not separator:
            continue
        parts = metadata.split()
        if len(parts) != 3:
            continue
        oid = parts[1]
        if (oid, path) in seen:
            continue
        seen.add((oid, path))
        completed = subprocess.run(
            ["git", "cat-file", "blob", oid],
            cwd=REPO_ROOT,
            capture_output=True,
            check=True,
        )
        text = completed.stdout.decode("utf-8", errors="ignore")
        scanned += 1
        secrets.extend(
            scan_text(
                text,
                path,
                "index_blob",
                oid[:16],
                SECRET_RULES,
                placeholders_allowed=True,
            )
        )
        paths.extend(
            scan_text(
                text,
                path,
                "index_blob",
                oid[:16],
                ABSOLUTE_PATH_RULES,
                placeholders_allowed=False,
            )
        )
    return sorted(set(secrets)), sorted(set(paths)), scanned


def index_blob_contents() -> tuple[dict[str, bytes], list[Finding]]:
    blobs: dict[str, bytes] = {}
    findings: list[Finding] = []
    for entry in git_lines("ls-files", "-s"):
        metadata, separator, path = entry.partition("\t")
        parts = metadata.split()
        if not separator or len(parts) != 3:
            findings.append(
                Finding("malformed_index_entry", path or "<unknown>", 0, "index", "index")
            )
            continue
        _mode, oid, stage = parts
        if stage != "0":
            findings.append(Finding("unmerged_index_entry", path, 0, "index", oid[:16]))
            continue
        completed = subprocess.run(
            ["git", "cat-file", "blob", oid],
            cwd=REPO_ROOT,
            capture_output=True,
            check=True,
        )
        blobs[path] = completed.stdout
    return blobs, findings


def verify_index_against_manifest(
    manifest: dict[str, object], files: Iterable[str]
) -> list[Finding]:
    blobs, findings = index_blob_contents()
    expected_paths = set(files)
    actual_paths = set(blobs)
    for path in sorted(expected_paths - actual_paths):
        findings.append(Finding("release_file_missing_from_index", path, 0, "index", "missing"))
    for path in sorted(actual_paths - expected_paths):
        findings.append(Finding("unexpected_index_file", path, 0, "index", "extra"))

    rows = manifest.get("release_files")
    if not isinstance(rows, list):
        return [
            *findings,
            Finding(
                "manifest_release_files_invalid",
                normalized(MANIFEST_PATH),
                0,
                "index",
                "manifest",
            ),
        ]
    by_path = {
        row.get("path"): row
        for row in rows
        if isinstance(row, dict) and isinstance(row.get("path"), str)
    }
    for path in sorted(expected_paths & actual_paths):
        data = blobs[path]
        if path == normalized(MANIFEST_PATH):
            worktree_data = (REPO_ROOT / Path(path)).read_bytes()
            if data != worktree_data:
                findings.append(
                    Finding(
                        "staged_manifest_differs_from_worktree",
                        path,
                        0,
                        "index",
                        sha256_bytes(data)[:16],
                    )
                )
            continue
        row = by_path.get(path)
        if not isinstance(row, dict):
            findings.append(
                Finding("staged_file_missing_manifest_row", path, 0, "index", "manifest")
            )
            continue
        if row.get("bytes") != len(data) or row.get("sha256") != sha256_bytes(data):
            findings.append(
                Finding(
                    "staged_blob_manifest_mismatch",
                    path,
                    0,
                    "index",
                    sha256_bytes(data)[:16],
                )
            )
    return sorted(set(findings))


def remote_has_embedded_credential() -> bool:
    for name in git_lines("remote"):
        urls = git_lines("remote", "get-url", "--all", name)
        urls.extend(git_lines("remote", "get-url", "--push", "--all", name))
        for url in urls:
            if re.search(r"https?://[^/@\s]+@", url):
                return True
            if re.search(r"[?&](?:access_token|token|key)=", url, re.IGNORECASE):
                return True
    return False


def github_slug(text: str) -> str:
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"!?(?:\[([^]]*)\])\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text, flags=re.UNICODE)
    return re.sub(r" +", "-", text)


def markdown_anchors(path: Path) -> set[str]:
    counts: collections.Counter[str] = collections.Counter()
    anchors: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not match:
            continue
        base = github_slug(match.group(1))
        if not base:
            continue
        index = counts[base]
        counts[base] += 1
        anchors.add(base if index == 0 else f"{base}-{index}")
    return anchors


def case_sensitive_file(path: Path) -> bool:
    try:
        relative = path.resolve().relative_to(REPO_ROOT.resolve())
    except (OSError, ValueError):
        return False
    current = REPO_ROOT.resolve()
    for component in relative.parts:
        try:
            names = {entry.name for entry in current.iterdir()}
        except OSError:
            return False
        if component not in names:
            return False
        current /= component
    return current.is_file()


def markdown_link_findings(files: Iterable[str]) -> list[Finding]:
    findings: list[Finding] = []
    link_pattern = re.compile(r"!?\[[^]]*\]\(([^)]+)\)")
    anchor_cache: dict[Path, set[str]] = {}
    for relative in files:
        if not relative.lower().endswith(".md"):
            continue
        source = REPO_ROOT / Path(relative)
        text = source.read_text(encoding="utf-8")
        for match in link_pattern.finditer(text):
            raw = match.group(1).strip()
            if raw.startswith("<") and raw.endswith(">"):
                raw = raw[1:-1]
            if not raw or re.match(r"^(?:https?|mailto):", raw, re.IGNORECASE):
                continue
            if raw.lower().startswith("file:" + "//"):
                findings.append(
                    Finding(
                        "markdown_file_uri",
                        relative,
                        line_number(text, match.start()),
                        "worktree",
                        "link",
                    )
                )
                continue
            target_text, separator, fragment = raw.partition("#")
            target_text = urllib.parse.unquote(target_text.split("?", 1)[0])
            target = (
                source
                if not target_text
                else source.parent / Path(target_text.replace("/", os.sep))
            )
            if not case_sensitive_file(target):
                findings.append(
                    Finding(
                        "markdown_missing_or_case_mismatch",
                        relative,
                        line_number(text, match.start()),
                        "worktree",
                        "link",
                    )
                )
                continue
            if separator and fragment and target.suffix.lower() == ".md":
                resolved = target.resolve()
                anchors = anchor_cache.setdefault(resolved, markdown_anchors(resolved))
                if urllib.parse.unquote(fragment).lower() not in anchors:
                    findings.append(
                        Finding(
                            "markdown_missing_anchor",
                            relative,
                            line_number(text, match.start()),
                            "worktree",
                            "link",
                        )
                    )
    return sorted(set(findings))


def workflow_findings(files: Iterable[str]) -> list[Finding]:
    findings: list[Finding] = []
    workflows = [path for path in files if path.startswith(".github/workflows/")]
    for relative in workflows:
        text = (REPO_ROOT / Path(relative)).read_text(encoding="utf-8")
        oid = sha256_bytes(text.encode())[:16]
        if re.search(r"^\s*pull_request_target\s*:", text, re.MULTILINE):
            findings.append(Finding("dangerous_pull_request_target", relative, 1, "worktree", oid))
        if not re.search(r"^permissions:\s*\n\s+contents:\s*read\s*$", text, re.MULTILINE):
            findings.append(Finding("missing_minimal_permissions", relative, 1, "worktree", oid))
        for match in re.finditer(r"^\s*-?\s*uses:\s*([^\s#]+)", text, re.MULTILINE):
            value = match.group(1)
            if value.startswith("./"):
                continue
            revision = value.rsplit("@", 1)[-1] if "@" in value else ""
            if not re.fullmatch(r"[0-9a-f]{40}", revision):
                findings.append(
                    Finding(
                        "github_action_not_commit_pinned",
                        relative,
                        line_number(text, match.start()),
                        "worktree",
                        oid,
                    )
                )
    return sorted(set(findings))


def release_file_findings(
    files: Iterable[str],
) -> tuple[list[Finding], list[dict[str, object]]]:
    findings: list[Finding] = []
    large: list[dict[str, object]] = []
    file_set = set(files)
    for relative in file_set:
        if relative == normalized(MANIFEST_PATH):
            continue
        path = REPO_ROOT / Path(relative)
        size = path.stat().st_size
        suffix = path.suffix.lower()
        if suffix in FORBIDDEN_SUFFIXES:
            findings.append(Finding("restricted_data_or_artifact", relative, 0, "release", suffix))
        if path.name == ".env" or (path.name.startswith(".env.") and path.name != ".env.example"):
            findings.append(Finding("environment_file", relative, 0, "release", "env"))
        if size > LARGE_FILE_BYTES:
            large.append(
                {
                    "path": relative,
                    "bytes": size,
                    "over_50_mib": size > BLOCKING_FILE_BYTES,
                }
            )
            if size > BLOCKING_FILE_BYTES:
                findings.append(Finding("file_over_50_mib", relative, 0, "release", str(size)))

    license_path = REPO_ROOT / "LICENSE"
    if "LICENSE" not in file_set or not license_path.is_file():
        findings.append(Finding("missing_project_license", "LICENSE", 0, "release", "license"))
    elif sha256_file(license_path) != APACHE_2_LICENSE_SHA256:
        findings.append(
            Finding(
                "apache_2_license_text_mismatch",
                "LICENSE",
                0,
                "release",
                sha256_file(license_path)[:16],
            )
        )

    notice_path = REPO_ROOT / "NOTICE"
    if "NOTICE" not in file_set or not notice_path.is_file():
        findings.append(Finding("missing_project_notice", "NOTICE", 0, "release", "notice"))
    else:
        notice = notice_path.read_text(encoding="utf-8")
        if "LOBForge" not in notice or "Copyright 2026 Haoxiang Sang" not in notice:
            findings.append(
                Finding(
                    "project_notice_content_mismatch",
                    "NOTICE",
                    0,
                    "release",
                    sha256_bytes(notice.encode())[:16],
                )
            )

    pyproject_path = REPO_ROOT / "python/pyproject.toml"
    pyproject = pyproject_path.read_text(encoding="utf-8")
    if not re.search(r'^license\s*=\s*"Apache-2\.0"\s*$', pyproject, re.MULTILINE):
        findings.append(
            Finding(
                "project_license_spdx_mismatch",
                "python/pyproject.toml",
                0,
                "release",
                "spdx",
            )
        )
    return sorted(set(findings)), sorted(
        large, key=lambda row: (-int(row["bytes"]), str(row["path"]))
    )


def declared_tests(files: Iterable[str]) -> dict[str, int]:
    python_tests = 0
    cpp_named = 0
    registered_ctests = 0
    test_files = 0
    file_set = set(files)
    if "CMakeLists.txt" in file_set:
        cmake_text = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        registered_ctests = len(re.findall(r"\badd_test\s*\(", cmake_text))
    for relative in file_set:
        if not (relative.startswith("tests/") or relative.startswith("python/tests/")):
            continue
        test_files += 1
        path = REPO_ROOT / Path(relative)
        decoded = text_and_lines(path.read_bytes())
        if decoded is None:
            continue
        text, _ = decoded
        if relative.endswith(".py"):
            python_tests += len(re.findall(r"^def test_[A-Za-z0-9_]+\s*\(", text, re.MULTILINE))
        elif relative.endswith((".cpp", ".cc", ".cxx")):
            for match in re.finditer(
                r"const\s+std::vector<[^;\n]+>\s+tests\s*\{(.*?)\n\s*\};",
                text,
                re.DOTALL,
            ):
                cpp_named += len(re.findall(r'\{\s*"[^"]+"\s*,', match.group(1)))
    return {
        "test_source_files": test_files,
        "declared_python_test_functions": python_tests,
        "declared_cpp_named_tests": cpp_named,
        "registered_ctest_count": registered_ctests,
    }


def top_level_ignored_counts(paths: Iterable[str]) -> dict[str, int]:
    counter: collections.Counter[str] = collections.Counter()
    for path in paths:
        counter[path.split("/", 1)[0]] += 1
    return dict(sorted(counter.items()))


def file_entries(files: Iterable[str]) -> tuple[list[dict[str, object]], int]:
    entries: list[dict[str, object]] = []
    total = 0
    for relative in files:
        if relative == normalized(MANIFEST_PATH):
            continue
        path = REPO_ROOT / Path(relative)
        size = path.stat().st_size
        total += size
        entries.append({"path": relative, "bytes": size, "sha256": sha256_file(path)})
    entries.append(
        {
            "path": normalized(MANIFEST_PATH),
            "kind": "generated_manifest",
            "physical_bytes_excluded_from_canonical_digest": True,
        }
    )
    return sorted(entries, key=lambda row: str(row["path"])), total


def documented_digests() -> dict[str, dict[str, str]]:
    return {
        "round2_factual_book": {
            "digest": "eaa0ddd8309c94c0",
            "evidence": "docs/round2_validation_report.md",
        },
        "round3_synthetic_dataset": {
            "digest": "d3f11ab4fe308743",
            "evidence": "docs/round3_validation_report.md",
        },
        "round4_shadow_audit": {
            "digest": "49c0a04b17a0de32",
            "evidence": "docs/round4_validation_report.md",
        },
    }


def canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode(
        "utf-8"
    )


def canonical_manifest_projection(manifest: dict[str, object]) -> dict[str, object]:
    return {key: manifest.get(key) for key in CANONICAL_MANIFEST_KEYS}


def manifest_integrity_errors(
    manifest: object, actual_physical_bytes: int | None = None
) -> list[str]:
    if not isinstance(manifest, dict):
        return ["manifest_not_object"]
    errors: list[str] = []
    expected_keys = set(CANONICAL_MANIFEST_KEYS) | PHYSICAL_MANIFEST_KEYS
    if set(manifest) != expected_keys:
        errors.append("top_level_schema_mismatch")

    rows = manifest.get("release_files")
    summary = manifest.get("summary")
    if not isinstance(rows, list) or not isinstance(summary, dict):
        return [*errors, "release_rows_or_summary_invalid"]
    paths: list[str] = []
    hashed_rows: list[dict[str, object]] = []
    sentinels: list[dict[str, object]] = []
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("path"), str):
            errors.append("release_row_invalid")
            continue
        paths.append(str(row["path"]))
        if row.get("kind") == "generated_manifest":
            sentinels.append(row)
            if set(row) != {
                "path",
                "kind",
                "physical_bytes_excluded_from_canonical_digest",
            }:
                errors.append("manifest_sentinel_fields_invalid")
            if row.get("physical_bytes_excluded_from_canonical_digest") is not True:
                errors.append("manifest_sentinel_policy_invalid")
        else:
            hashed_rows.append(row)
            if set(row) != {"path", "bytes", "sha256"}:
                errors.append("hashed_release_row_fields_invalid")
            if not isinstance(row.get("bytes"), int) or int(row.get("bytes", -1)) < 0:
                errors.append("hashed_release_row_bytes_invalid")
            if not isinstance(row.get("sha256"), str) or not re.fullmatch(
                r"[0-9a-f]{64}", str(row.get("sha256", ""))
            ):
                errors.append("hashed_release_row_sha256_invalid")

    if paths != sorted(paths) or len(paths) != len(set(paths)):
        errors.append("release_paths_not_unique_sorted")
    if len(sentinels) != 1 or sentinels[0].get("path") != normalized(MANIFEST_PATH):
        errors.append("generated_manifest_sentinel_invalid")
    if summary.get("planned_release_file_count") != len(rows):
        errors.append("planned_release_file_count_mismatch")
    if summary.get("hashed_release_file_count") != len(hashed_rows):
        errors.append("hashed_release_file_count_mismatch")
    if summary.get("generated_manifest_entries_without_sha256") != len(sentinels):
        errors.append("generated_manifest_count_mismatch")
    source_bytes = sum(
        int(row["bytes"])
        for row in hashed_rows
        if isinstance(row.get("bytes"), int) and int(row["bytes"]) >= 0
    )
    if summary.get("source_bytes_excluding_manifest") != source_bytes:
        errors.append("source_bytes_mismatch")

    policy = manifest.get("manifest_hash_policy")
    if policy != {
        "manifest_path": normalized(MANIFEST_PATH),
        "self_sha256": "excluded_to_avoid_recursive_hashing",
        "canonical_digest_covers": "typed_release_rows_and_release_contracts",
    }:
        errors.append("manifest_hash_policy_invalid")

    projection = canonical_manifest_projection(manifest)
    calculated_digest = sha256_bytes(canonical_json(projection))
    if manifest.get("canonical_manifest_digest") != calculated_digest:
        errors.append("canonical_digest_not_self_consistent")
    if manifest.get("generated_at_excluded_from_canonical_digest") is not True:
        errors.append("generation_time_policy_invalid")
    if manifest.get("physical_manifest_bytes_excluded_from_canonical_digest") is not True:
        errors.append("physical_manifest_policy_invalid")

    physical_bytes = manifest.get("physical_manifest_bytes")
    physical_total = manifest.get("physical_total_release_bytes")
    if not isinstance(physical_bytes, int) or physical_bytes < 0:
        errors.append("physical_manifest_bytes_invalid")
    else:
        if actual_physical_bytes is not None and physical_bytes != actual_physical_bytes:
            errors.append("physical_manifest_bytes_mismatch")
        if physical_total != source_bytes + physical_bytes:
            errors.append("physical_total_release_bytes_mismatch")
    return sorted(set(errors))


def make_manifest(
    files: list[str],
    tracked: set[str],
    untracked: set[str],
    ignored: list[str],
    worktree_secrets: list[Finding],
    worktree_paths: list[Finding],
    history_secrets: list[Finding],
    history_paths: list[Finding],
    history_blob_count: int,
    index_secrets: list[Finding],
    index_paths: list[Finding],
    index_blob_count: int,
    link_findings: list[Finding],
    workflow_issues: list[Finding],
    release_issues: list[Finding],
    large_files: list[dict[str, object]],
) -> dict[str, object]:
    entries, total_without_manifest = file_entries(files)
    suffix_counts: collections.Counter[str] = collections.Counter()
    for relative in files:
        suffix = PurePosixPath(relative).suffix.lower() or "[no-extension]"
        suffix_counts[suffix] += 1
    biggest = max(
        (entry for entry in entries if "bytes" in entry),
        key=lambda row: int(row["bytes"]),
        default={"path": normalized(MANIFEST_PATH), "bytes": 0},
    )
    protocols = {}
    for round_name, relative in (
        ("round3", "configs/round3_protocol.toml"),
        ("round4", "configs/round4_protocol.toml"),
    ):
        path = REPO_ROOT / relative
        protocols[round_name] = {"path": relative, "sha256": sha256_file(path)}

    canonical = {
        "schema": "lobforge.release_manifest",
        "version": 1,
        "release_candidate": "v0.4.0-rc1",
        "release_files": entries,
        "summary": {
            "planned_release_file_count": len(entries),
            "hashed_release_file_count": sum("sha256" in entry for entry in entries),
            "generated_manifest_entries_without_sha256": sum(
                entry.get("kind") == "generated_manifest" for entry in entries
            ),
            "source_bytes_excluding_manifest": total_without_manifest,
            "file_type_statistics": dict(sorted(suffix_counts.items())),
            "documentation_files": sum(path.endswith(".md") for path in files),
            **declared_tests(files),
        },
        "manifest_hash_policy": {
            "manifest_path": normalized(MANIFEST_PATH),
            "self_sha256": "excluded_to_avoid_recursive_hashing",
            "canonical_digest_covers": "typed_release_rows_and_release_contracts",
        },
        "protocols": protocols,
        "round1_to_round4_contracts": {
            "round1": "docs/adr/0001-matching-engine-semantics.md",
            "round2": "docs/book_event_v1.md",
            "round3": "configs/round3_protocol.toml",
            "round4": "configs/round4_protocol.toml",
        },
        "key_semantic_digests": documented_digests(),
        "content_assertions": {
            "contains_real_or_restricted_market_data": any(
                finding.rule == "restricted_data_or_artifact" for finding in release_issues
            ),
            "contains_detected_secrets_or_private_data": bool(
                worktree_secrets or index_secrets or history_secrets
            ),
            "contains_local_absolute_paths": bool(worktree_paths),
            "license_file_exists": "LICENSE" in files and (REPO_ROOT / "LICENSE").is_file(),
            "license_file_in_release": "LICENSE" in files,
            "notice_file_in_release": "NOTICE" in files,
            "apache_2_license_text_verified": (
                "LICENSE" in files
                and (REPO_ROOT / "LICENSE").is_file()
                and sha256_file(REPO_ROOT / "LICENSE") == APACHE_2_LICENSE_SHA256
            ),
            "notice_copyright_verified": (
                "NOTICE" in files
                and (REPO_ROOT / "NOTICE").is_file()
                and "LOBForge" in (REPO_ROOT / "NOTICE").read_text(encoding="utf-8")
                and "Copyright 2026 Haoxiang Sang"
                in (REPO_ROOT / "NOTICE").read_text(encoding="utf-8")
            ),
            "project_license_spdx": "Apache-2.0",
        },
    }
    digest = sha256_bytes(canonical_json(canonical))
    generated = dt.datetime.now(dt.UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    manifest: dict[str, object] = {
        **canonical,
        "generated_at_utc": generated,
        "generated_at_excluded_from_canonical_digest": True,
        "physical_manifest_bytes_excluded_from_canonical_digest": True,
        "canonical_manifest_digest": digest,
        "release_scope": {
            "tracked_files_in_repository": len(tracked),
            "untracked_nonignored_files_in_repository": len(untracked),
            "ignored_files_in_worktree": len(ignored),
            "ignored_top_level_counts": top_level_ignored_counts(ignored),
            "planned_tracked_files": sum(path in tracked for path in files),
            "planned_untracked_files": sum(path in untracked for path in files),
        },
        "audit_summary": {
            "worktree_secret_findings": len(worktree_secrets),
            "worktree_absolute_path_findings": len(worktree_paths),
            "history_secret_findings": len(history_secrets),
            "history_absolute_path_findings": len(history_paths),
            "history_blobs_scanned": history_blob_count,
            "index_secret_findings": len(index_secrets),
            "index_absolute_path_findings": len(index_paths),
            "index_blobs_scanned": index_blob_count,
            "markdown_link_findings": len(link_findings),
            "workflow_findings": len(workflow_issues),
            "release_content_findings": len(release_issues),
            "large_files_over_10_mib": large_files,
            "remote_address_has_embedded_credential": remote_has_embedded_credential(),
        },
        "largest_source_file_excluding_manifest": biggest,
        "physical_manifest_bytes": 0,
        "physical_total_release_bytes": total_without_manifest,
    }
    for _ in range(8):
        encoded = (
            json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
        ).encode("utf-8")
        new_size = len(encoded)
        if manifest["physical_manifest_bytes"] == new_size:
            break
        manifest["physical_manifest_bytes"] = new_size
        manifest["physical_total_release_bytes"] = total_without_manifest + new_size
    return manifest


def write_manifest(path: Path, manifest: dict[str, object]) -> None:
    encoded = (json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode(
        "utf-8"
    )
    errors = manifest_integrity_errors(manifest, len(encoded))
    if errors:
        raise RuntimeError(f"refusing to write inconsistent manifest: {','.join(errors)}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded)


def print_findings(label: str, findings: list[Finding]) -> None:
    print(f"{label}={len(findings)}")
    for finding in findings[:20]:
        print(
            "  "
            f"rule={finding.rule} path={finding.path} line={finding.line} "
            f"scope={finding.scope} object={finding.object_id}"
        )
    if len(findings) > 20:
        print(f"  ... {len(findings) - 20} additional redacted findings")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=REPO_ROOT / Path(MANIFEST_PATH),
        help="manifest output path",
    )
    parser.add_argument(
        "--no-write", action="store_true", help="validate without writing a manifest"
    )
    parser.add_argument(
        "--verify-existing",
        action="store_true",
        help="strictly verify the existing manifest and compare its canonical content",
    )
    parser.add_argument(
        "--verify-index",
        action="store_true",
        help="require the complete Git index tree and staged bytes to match the manifest",
    )
    parser.add_argument(
        "--include-history-paths-as-errors",
        action="store_true",
        help="treat non-private historical build paths as blockers",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    os.chdir(REPO_ROOT)
    files, tracked, untracked, ignored = discover_release_files()
    manifest_relative = normalized(args.manifest.resolve().relative_to(REPO_ROOT.resolve()))
    if manifest_relative == normalized(MANIFEST_PATH) and manifest_relative not in files:
        files.append(manifest_relative)
        files.sort()

    worktree_secrets, worktree_paths = scan_worktree(
        path for path in files if path != manifest_relative
    )
    history_secrets, history_paths, history_blob_count = scan_history()
    index_secrets, index_paths, index_blob_count = scan_index()
    links = markdown_link_findings(path for path in files if path != manifest_relative)
    workflow_issues = workflow_findings(files)
    release_issues, large_files = release_file_findings(files)
    release_issues = sorted(
        set(release_issues + scope_classification_findings(files, tracked, untracked))
    )
    remote_risk = remote_has_embedded_credential()

    manifest = make_manifest(
        files,
        tracked,
        untracked,
        ignored,
        worktree_secrets,
        worktree_paths,
        history_secrets,
        history_paths,
        history_blob_count,
        index_secrets,
        index_paths,
        index_blob_count,
        links,
        workflow_issues,
        release_issues,
        large_files,
    )
    verification_failed = False
    if args.verify_existing:
        if not args.manifest.is_file():
            print("manifest_verification=FAIL reason=missing_manifest")
            verification_failed = True
        else:
            try:
                existing_bytes = args.manifest.read_bytes()
                existing = json.loads(existing_bytes.decode("utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError):
                existing = None
                existing_bytes = b""
            integrity_errors = manifest_integrity_errors(existing, len(existing_bytes))
            if integrity_errors:
                print(
                    "manifest_verification=FAIL reason=integrity_error "
                    f"checks={','.join(integrity_errors)}"
                )
                verification_failed = True
            elif canonical_manifest_projection(existing) != canonical_manifest_projection(manifest):
                print("manifest_verification=FAIL reason=canonical_content_mismatch")
                verification_failed = True
            else:
                print("manifest_verification=PASS")

    index_verification: list[Finding] = []
    if args.verify_index:
        index_verification = verify_index_against_manifest(manifest, files)
    if not args.no_write:
        write_manifest(args.manifest, manifest)

    print(f"release_files={manifest['summary']['planned_release_file_count']}")
    print(f"release_files_with_sha256={manifest['summary']['hashed_release_file_count']}")
    print(
        "generated_manifest_entries_without_sha256="
        f"{manifest['summary']['generated_manifest_entries_without_sha256']}"
    )
    print(f"release_bytes={manifest['physical_total_release_bytes']}")
    print(f"history_blobs_scanned={history_blob_count}")
    print_findings("worktree_secret_findings", worktree_secrets)
    print_findings("worktree_absolute_path_findings", worktree_paths)
    print_findings("history_secret_findings", history_secrets)
    print(f"history_absolute_path_findings={len(history_paths)}")
    print_findings("index_secret_findings", index_secrets)
    print_findings("index_absolute_path_findings", index_paths)
    print_findings("index_manifest_findings", index_verification)
    print(f"index_blobs_scanned={index_blob_count}")
    print_findings("markdown_link_findings", links)
    print_findings("workflow_findings", workflow_issues)
    print_findings("release_content_findings", release_issues)
    print(f"large_files_over_10_mib={len(large_files)}")
    print(f"remote_address_has_embedded_credential={str(remote_risk).lower()}")
    print(f"canonical_manifest_digest={manifest['canonical_manifest_digest']}")

    blockers = (
        worktree_secrets
        or worktree_paths
        or history_secrets
        or index_secrets
        or index_verification
        or (args.verify_index and index_paths)
        or links
        or workflow_issues
        or release_issues
        or remote_risk
        or verification_failed
        or (args.include_history_paths_as_errors and history_paths)
    )
    if blockers:
        print("release_audit=FAIL")
        return 1
    print("release_audit=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
