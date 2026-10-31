#!/usr/bin/env python3
"""Summarize save-data file I/O from a Vita3K SDK log.

This tool is deliberately read-only.  It selects only fsp-srv records that are
bound to a declared save-data root (the roots come from
``SVC_IPC_FS_SAVE_REQ root=...`` records), correlates each ``OpenFile`` on the
returned filesystem object with the returned file object, and groups reads by
that open lifetime.  Unrelated romfs paths, different handles, and different
opens cannot be combined into a false "full session" claim.

Real evidence shape (checked against F2)::

    SVC_IPC_FS_SAVE_REQ ... root=ux0:data/... result=0x00000000
    SVC_IPC_DOMAIN_CALL handle=0x108 object=1 call=OpenSaveDataFileSystem
        out_object=6 out_interface=32 result=0
    SVC_IPC_DOMAIN_CALL handle=0x108 object=6 call=OpenFile
        path="/system" out_object=7 result=0
    SVC_IPC_DOMAIN_CALL handle=0x108 object=7 call=GetSize
        path="ux0:data/.../system" size=5159100 result=0
    ... Read on object=7, then SVC_IPC_DOMAIN_CLOSE object=7.

The OpenFile parent (filesystem object) is not the file session.  A read must
resolve to the live file object returned by OpenFile, under the same handle,
and the absolute GetSize/Read path must match the file opened from the parent
root + relative OpenFile path.  A file object id reused without a close, a
read without an observed OpenFile, an unknown parent, or a path mismatch is
rejected instead of guessed across roots or handles.

What it proves:
  * which save files the game requested and with which open/getsize/read
    offset/size/returned values and result codes;
  * whether at least one single open session of each required file returned a
    contiguous byte range [0, declaredSize) with no interior gap;
  * whether every read request is internally valid and consistent with the
    declared file size, including legal EOF short reads.

What it cannot prove:
  * game semantics or that every gameplay field was parsed;
  * that the game read bytes it never requested;
  * that an unobserved enqueue/wakeup did not happen inside the emulator.

A short read is legal only at EOF: the request may extend past the declared
size when the returned bytes end exactly at that size.  A request that claims
to return bytes beyond the declared size, a short read before EOF, a nonzero
result, an unrequested/invalid bounds combination, or any write/flush/commit on
a save path is a failure.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

TOKEN_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=("(?:[^"\\]|\\.)*"|\S+)')
WRITE_CALLS = {"write", "flush", "commit", "setfilesize", "createfile", "deletefile", "rename"}


def unquote(value: str) -> str:
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1]
    return value


def parse_result(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def parse_object(value: str | None) -> int | str | None:
    """Return a numeric object id when possible, otherwise the raw token.

    Numeric normalization prevents 0x108 and 0x00000108 from being treated as
    independent identities merely because the log formatting differs.
    """
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return value


def normalize_path(value: str) -> str:
    return value.replace("\\", "/").rstrip("/")


def relative_under(path: str, root: str) -> str | None:
    path_n = normalize_path(path)
    root_n = normalize_path(root)
    if path_n == root_n:
        return ""
    prefix = root_n + "/"
    if path_n.startswith(prefix):
        return path_n[len(prefix):]
    return None


def is_absolute_path(value: str) -> bool:
    """True for a device-qualified path such as app0:/foo or ux0:/foo.

    A leading slash alone is not device-qualified: in the real fsp-srv shape
    OpenFile path="/system" is relative to the parent filesystem object.
    """
    normalized = normalize_path(value)
    return ":" in normalized.split("/", 1)[0]


def clean_relative(value: str) -> str | None:
    rel = normalize_path(value).strip("/")
    if not rel or rel == ".":
        return None
    parts = rel.split("/")
    if any(part in ("", ".", "..") for part in parts):
        return None
    return rel


def resolve_relative_path(path: str, parent_root: str) -> tuple[str | None, str | None]:
    """Resolve an OpenFile path against the parent filesystem root.

    Returns (relative_name, error).  Relative paths (including the real
    ``/system`` form) are relative to the parent root.  Absolute paths must be
    under the parent root; an absolute path under a different root is rejected
    rather than guessed.
    """
    normalized = normalize_path(path)
    if normalized.startswith("/"):
        rel = clean_relative(normalized)
    elif ":" in normalized.split("/", 1)[0]:
        rel = relative_under(normalized, parent_root)
        if rel is None:
            return None, "absolute path is outside the parent save root"
        rel = clean_relative(rel)
    else:
        rel = clean_relative(normalized)
    if rel is None:
        return None, "path is empty, invalid, or contains '..'"
    return rel, None


def is_under_any_root(path: str, roots: list[str]) -> bool:
    return any(relative_under(path, root) is not None for root in roots)


def merge_intervals(intervals: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for start, end in sorted(intervals):
        if start < 0 or end <= start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def exact_full_coverage(merged: list[tuple[int, int]], size: int) -> bool:
    # Exactly one interval [0, size) after merging: no interior gap and no tail.
    return len(merged) == 1 and merged[0] == (0, size)


def summarize(log_path: Path, expected_size: int | None, expected_uid: str | None,
              expected_root_substring: str | None, required_files: list[str]) -> dict:
    failures: list[str] = []

    # Pass 1: save-data roots and their request results.
    save_requests: list[dict] = []
    with log_path.open("r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, start=1):
            if "SVC_IPC_FS_SAVE_REQ" not in line:
                continue
            parts = {key: unquote(value) for key, value in TOKEN_RE.findall(line)}
            root = parts.get("root")
            if not root:
                continue
            save_requests.append(
                {
                    "line": line_number,
                    "root": root,
                    "result": parse_result(parts.get("result")),
                    "raw_result": parts.get("result"),
                }
            )

    roots = []
    for request in save_requests:
        normalized = normalize_path(request["root"])
        if normalized not in roots:
            roots.append(normalized)
    if not save_requests:
        failures.append("no SVC_IPC_FS_SAVE_REQ save-data evidence in the log")
    if len(roots) > 1:
        failures.append(f"multiple distinct save-data roots observed: {roots!r}")
    for request in save_requests:
        if request["result"] != 0:
            failures.append(
                f"save request at line {request['line']} failed: {request['raw_result']}"
            )
    if expected_uid:
        if not any(expected_uid in root for root in roots):
            failures.append(f"expected save UID {expected_uid!r} not found in save roots {roots!r}")
    if expected_root_substring:
        if not any(expected_root_substring in root for root in roots):
            failures.append(
                f"expected save-root substring {expected_root_substring!r} not found in {roots!r}"
            )

    # Pass 2: correlate filesystem objects, file opens, and per-open reads.
    save_request_by_line = {request["line"]: request for request in save_requests}
    pending_roots: list[dict] = []
    filesystem_objects: dict[tuple[object, object], dict] = {}
    filesystem_bindings: list[dict] = []
    live_files: dict[tuple[object, object], dict] = {}
    sessions: list[dict] = []
    writes: list[dict] = []
    out_of_scope = {"filesystem_calls": 0, "writes": 0}
    save_metadata_calls = 0
    ambiguous_rejections = 0
    openfile_calls = 0
    openfile_failures = 0
    getsize_calls = 0
    read_calls = 0
    close_calls = 0

    def reject_ambiguous(message: str) -> None:
        nonlocal ambiguous_rejections
        ambiguous_rejections += 1
        failures.append(message)

    def classify_write(path: str | None, handle_key: object, object_key: object) -> str:
        """Classify a write as save/out-of-scope/ambiguous without guessing."""
        if path is not None and is_under_any_root(path, roots):
            return "save"
        if (handle_key, object_key) in live_files:
            return "save"
        parent = filesystem_objects.get((handle_key, object_key))
        if parent is not None:
            if path is None:
                return "save"
            rel, _ = resolve_relative_path(path, parent["root"])
            if rel is not None:
                return "save"
            if is_absolute_path(path):
                return "out-of-scope"
            return "ambiguous"
        if path is None:
            return "ambiguous"
        if is_absolute_path(path):
            return "out-of-scope"
        return "ambiguous"

    if roots:
        with log_path.open("r", encoding="utf-8", errors="replace") as handle:
            for line_number, line in enumerate(handle, start=1):
                if line_number in save_request_by_line:
                    pending_roots.append(save_request_by_line[line_number])
                    continue

                if "SVC_IPC_DOMAIN_CLOSE" in line:
                    if "service=fsp-srv" not in line:
                        continue
                    parts = {key: unquote(value) for key, value in TOKEN_RE.findall(line)}
                    handle_key = parse_object(parts.get("handle"))
                    object_key = parse_object(parts.get("object"))
                    result = parse_result(parts.get("result"))
                    valid = parts.get("valid")
                    close_calls += 1
                    key = (handle_key, object_key)
                    if key in live_files:
                        session = live_files.pop(key)
                        session["close_line"] = line_number
                        session["close_result"] = result
                        if result != 0 or valid not in (None, "1"):
                            session["errors"].append(
                                f"close at line {line_number} failed: result={parts.get('result')} valid={valid}"
                            )
                    elif key in filesystem_objects:
                        fs = filesystem_objects.pop(key)
                        fs["close_line"] = line_number
                        fs["close_result"] = result
                        if result != 0:
                            failures.append(
                                f"filesystem close at line {line_number} failed: {parts.get('result')}"
                            )
                    else:
                        out_of_scope["filesystem_calls"] += 1
                    continue

                if "service=fsp-srv" not in line:
                    continue
                parts = {key: unquote(value) for key, value in TOKEN_RE.findall(line)}
                call = (parts.get("call") or "").lower()
                handle_key = parse_object(parts.get("handle"))
                object_key = parse_object(parts.get("object"))
                path = parts.get("path")
                result = parse_result(parts.get("result"))

                if call == "opensavedatafilesystem":
                    if not pending_roots:
                        reject_ambiguous(
                            f"OpenSaveDataFileSystem at line {line_number} has no preceding "
                            "SVC_IPC_FS_SAVE_REQ root"
                        )
                        continue
                    if len(pending_roots) > 1:
                        reject_ambiguous(
                            f"OpenSaveDataFileSystem at line {line_number} follows multiple "
                            f"pending save requests ({len(pending_roots)})"
                        )
                    request = pending_roots.pop(0)
                    if handle_key is None:
                        reject_ambiguous(
                            f"OpenSaveDataFileSystem at line {line_number} is missing handle"
                        )
                        continue
                    if result != 0:
                        failures.append(
                            f"OpenSaveDataFileSystem at line {line_number} failed: "
                            f"{parts.get('result')}"
                        )
                        continue
                    out_object = parse_object(parts.get("out_object"))
                    if out_object is None:
                        reject_ambiguous(
                            f"OpenSaveDataFileSystem at line {line_number} returned no out_object"
                        )
                        continue
                    key = (handle_key, out_object)
                    if key in filesystem_objects:
                        reject_ambiguous(
                            f"filesystem object {out_object!r} on handle {handle_key!r} "
                            f"was reused without a close at line {line_number}"
                        )
                        continue
                    binding = {
                        "handle": handle_key,
                        "object": out_object,
                        "root": normalize_path(request["root"]),
                        "line": line_number,
                        "out_interface": parts.get("out_interface"),
                        "close_line": None,
                        "close_result": None,
                    }
                    filesystem_objects[key] = binding
                    filesystem_bindings.append(binding)
                    continue

                if call == "openfile":
                    openfile_calls += 1
                    if handle_key is None or object_key is None:
                        reject_ambiguous(
                            f"OpenFile at line {line_number} is missing handle or parent object"
                        )
                        continue
                    parent = filesystem_objects.get((handle_key, object_key))
                    if parent is None:
                        # A relative OpenFile on an unknown parent cannot be
                        # proven to be outside the save root; an absolute path
                        # under a declared root is definitely save evidence.
                        # Both are rejected rather than assigned to a guessed
                        # filesystem root.
                        if path is not None and (
                            is_under_any_root(path, roots) or not is_absolute_path(path)
                        ):
                            reject_ambiguous(
                                f"OpenFile at line {line_number} on handle {handle_key!r} "
                                f"object {object_key!r} has no observed "
                                "OpenSaveDataFileSystem binding"
                            )
                        else:
                            out_of_scope["filesystem_calls"] += 1
                        continue
                    if result != 0:
                        openfile_failures += 1
                        failures.append(
                            f"open at line {line_number} failed: {parts.get('result')} "
                            f"for relative parent object {object_key!r}"
                        )
                        continue
                    if path is None:
                        reject_ambiguous(
                            f"OpenFile at line {line_number} is missing its relative path"
                        )
                        continue
                    rel, error = resolve_relative_path(path, parent["root"])
                    if rel is None:
                        reject_ambiguous(
                            f"OpenFile at line {line_number} path {path!r} cannot be "
                            f"resolved under parent root {parent['root']!r}: {error}"
                        )
                        continue
                    out_object = parse_object(parts.get("out_object"))
                    if out_object is None:
                        reject_ambiguous(
                            f"OpenFile at line {line_number} returned no out_object"
                        )
                        continue
                    key = (handle_key, out_object)
                    if key in live_files:
                        reject_ambiguous(
                            f"file object {out_object!r} on handle {handle_key!r} was "
                            f"opened again without a close at line {line_number}"
                        )
                        continue
                    session = {
                        "handle": handle_key,
                        "filesystem_object": object_key,
                        "object": out_object,
                        "open_line": line_number,
                        "name": rel,
                        "root": parent["root"],
                        "path": parent["root"] + "/" + rel,
                        "open_result": result,
                        "close_line": None,
                        "close_result": None,
                        "declared_size": None,
                        "declared_size_lines": [],
                        "reads": [],
                        "getsize_count": 0,
                        "errors": [],
                    }
                    live_files[key] = session
                    sessions.append(session)
                    continue

                if call == "getsize":
                    getsize_calls += 1
                    if handle_key is None or object_key is None:
                        if path is not None and is_under_any_root(path, roots):
                            reject_ambiguous(
                                f"GetSize at line {line_number} is missing handle or object"
                            )
                        else:
                            out_of_scope["filesystem_calls"] += 1
                        continue
                    session = live_files.get((handle_key, object_key))
                    if session is None:
                        if path is not None and is_under_any_root(path, roots):
                            reject_ambiguous(
                                f"GetSize at line {line_number} on handle {handle_key!r} "
                                f"object {object_key!r} has no observed OpenFile"
                            )
                        else:
                            out_of_scope["filesystem_calls"] += 1
                        continue
                    if path is None:
                        session["errors"].append(
                            f"GetSize at line {line_number} is missing path"
                        )
                    elif normalize_path(path) != session["path"]:
                        session["errors"].append(
                            f"GetSize at line {line_number} path {path!r} does not match "
                            f"open path {session['path']!r}"
                        )
                    session["getsize_count"] += 1
                    if result != 0:
                        session["errors"].append(
                            f"GetSize at line {line_number} failed: {parts.get('result')}"
                        )
                    size = parts.get("size")
                    try:
                        size_value = int(size, 0) if size is not None else None
                    except ValueError:
                        size_value = None
                    session["declared_size_lines"].append(
                        {
                            "line": line_number,
                            "size": size_value,
                            "result": result,
                            "raw_size": size,
                            "raw_result": parts.get("result"),
                        }
                    )
                    if size_value is None or size_value <= 0:
                        session["errors"].append(
                            f"GetSize at line {line_number} has invalid size {size!r}"
                        )
                    elif result == 0:
                        if session["declared_size"] is None:
                            session["declared_size"] = size_value
                        elif session["declared_size"] != size_value:
                            session["errors"].append(
                                f"GetSize at line {line_number} changed declared size "
                                f"{session['declared_size']} -> {size_value}"
                            )
                    continue

                if call == "read":
                    read_calls += 1
                    if handle_key is None or object_key is None:
                        if path is not None and is_under_any_root(path, roots):
                            reject_ambiguous(
                                f"Read at line {line_number} is missing handle or object"
                            )
                        else:
                            out_of_scope["filesystem_calls"] += 1
                        continue
                    session = live_files.get((handle_key, object_key))
                    if session is None:
                        if path is not None and is_under_any_root(path, roots):
                            reject_ambiguous(
                                f"Read at line {line_number} on handle {handle_key!r} "
                                f"object {object_key!r} has no observed OpenFile"
                            )
                        else:
                            out_of_scope["filesystem_calls"] += 1
                        continue
                    if path is None:
                        session["errors"].append(
                            f"read at line {line_number} is missing path"
                        )
                    elif normalize_path(path) != session["path"]:
                        session["errors"].append(
                            f"read at line {line_number} path {path!r} does not match "
                            f"open path {session['path']!r}"
                        )
                    try:
                        offset = int(parts["offset"], 0)
                        size_value = int(parts["size"], 0)
                        capacity = int(parts["capacity"], 0)
                        returned = int(parts["returned"], 0)
                    except (KeyError, ValueError):
                        session["errors"].append(
                            f"read at line {line_number} is missing numeric "
                            "offset/size/capacity/returned"
                        )
                        continue
                    session["reads"].append(
                        {
                            "line": line_number,
                            "offset": offset,
                            "size": size_value,
                            "capacity": capacity,
                            "returned": returned,
                            "result": result,
                            "raw_result": parts.get("result"),
                        }
                    )
                    continue

                if call in WRITE_CALLS:
                    classification = classify_write(path, handle_key, object_key)
                    if classification == "save":
                        writes.append(
                            {
                                "line": line_number,
                                "call": parts.get("call"),
                                "path": path,
                                "handle": handle_key,
                                "object": object_key,
                                "result": result,
                            }
                        )
                    elif classification == "ambiguous":
                        out_of_scope["filesystem_calls"] += 1
                        out_of_scope["writes"] += 1
                        reject_ambiguous(
                            f"unscoped write/flush/commit at line {line_number} "
                            "cannot be classified as non-save"
                        )
                    else:
                        out_of_scope["filesystem_calls"] += 1
                        out_of_scope["writes"] += 1
                    continue

                # Save-root metadata (for example GetEntryType) is expected and
                # is not required read evidence.  Calls proven outside the root
                # are counted as out of scope.
                parent = filesystem_objects.get((handle_key, object_key))
                if path is not None and is_under_any_root(path, roots):
                    save_metadata_calls += 1
                elif parent is not None:
                    save_metadata_calls += 1
                else:
                    out_of_scope["filesystem_calls"] += 1

    if pending_roots:
        failures.append(
            f"{len(pending_roots)} SVC_IPC_FS_SAVE_REQ record(s) had no "
            "OpenSaveDataFileSystem receipt"
        )

    # Validate every open-lifetime session against the declared size and the
    # backing IO contract.  Reads are never moved between sessions.
    for session in sessions:
        declared = session["declared_size"]
        intervals: list[tuple[int, int]] = []
        eof_short_reads = 0
        if declared is None:
            session["errors"].append("no valid GetSize declared size for this open session")
        for read in session["reads"]:
            offset = read["offset"]
            size_value = read["size"]
            capacity = read["capacity"]
            returned = read["returned"]
            line = read["line"]
            if read["result"] != 0:
                session["errors"].append(
                    f"read at line {line} failed: {read['raw_result']}"
                )
                continue
            if offset < 0 or size_value < 0 or capacity < 0 or returned < 0:
                session["errors"].append(f"read at line {line} has negative bounds")
                continue
            if capacity < size_value:
                session["errors"].append(
                    f"read at line {line} capacity {capacity} < requested size {size_value}"
                )
                continue
            if returned > size_value:
                session["errors"].append(
                    f"read at line {line} returned {returned} > requested size {size_value}"
                )
                continue
            if declared is None:
                # GetSize failed or was never observed; bounds cannot be
                # validated, so this session cannot be accepted as evidence.
                continue
            if offset > declared:
                session["errors"].append(
                    f"read at line {line} offset {offset} is beyond declared size {declared}"
                )
                continue
            requested_end = offset + size_value
            returned_end = offset + returned
            if offset == declared:
                if returned != 0:
                    session["errors"].append(
                        f"read at line {line} returned {returned} bytes at EOF offset {offset}"
                    )
                    continue
                eof_short_reads += 1
                continue
            if requested_end > declared:
                if returned != declared - offset:
                    session["errors"].append(
                        f"read at line {line} requests past EOF (offset {offset} + size "
                        f"{size_value} > {declared}) but returned {returned}, not "
                        f"{declared - offset}"
                    )
                    continue
                if size_value != 0:
                    eof_short_reads += 1
            elif returned < size_value:
                session["errors"].append(
                    f"short read at line {line}: requested {size_value} returned {returned}"
                )
                continue
            if returned_end > declared:
                session["errors"].append(
                    f"read at line {line} returns beyond declared size: "
                    f"{returned_end} > {declared}"
                )
                continue
            if returned > 0:
                intervals.append((offset, returned_end))
        merged = merge_intervals(intervals)
        complete = bool(declared is not None and exact_full_coverage(merged, declared))
        session["merged_intervals"] = [{"start": s, "end": e} for s, e in merged]
        session["covered_bytes"] = sum(e - s for s, e in merged)
        session["returned_bytes"] = sum(read["returned"] for read in session["reads"])
        session["requested_bytes"] = sum(read["size"] for read in session["reads"])
        session["read_count"] = len(session["reads"])
        session["eof_short_reads"] = eof_short_reads
        session["single_session_complete_coverage"] = complete
        session.pop("declared_size_lines", None)

    file_records: dict[str, dict] = {}
    for session in sessions:
        name = session["name"]
        record = file_records.setdefault(
            name,
            {
                "name": name,
                "sessions": [],
                "declared_sizes": [],
                "session_count": 0,
                "complete_session_count": 0,
                "combined_intervals": [],
                "combined_complete": False,
                "errors": [],
            },
        )
        record["sessions"].append(session)
        record["session_count"] += 1
        if session["declared_size"] is not None:
            record["declared_sizes"].append(session["declared_size"])
        if session["single_session_complete_coverage"]:
            record["complete_session_count"] += 1
        record["errors"].extend(
            f"handle {session['handle']!r} object {session['object']!r}: {error}"
            for error in session["errors"]
        )

    for name, record in file_records.items():
        sizes = sorted(set(record["declared_sizes"]))
        record["declared_size"] = sizes[0] if len(sizes) == 1 else None
        if len(sizes) != 1:
            record["errors"].append(f"inconsistent declared sizes across sessions: {sizes!r}")
        if expected_size is not None:
            if record["declared_size"] != expected_size:
                record["errors"].append(
                    f"declared size {record['declared_size']} != required {expected_size}"
                )
        combined: list[tuple[int, int]] = []
        for session in record["sessions"]:
            combined.extend(
                (item["start"], item["end"]) for item in session["merged_intervals"]
            )
        merged_combined = merge_intervals(combined)
        record["combined_intervals"] = [
            {"start": s, "end": e} for s, e in merged_combined
        ]
        record["combined_complete"] = bool(
            record["declared_size"] is not None
            and exact_full_coverage(merged_combined, record["declared_size"])
        )
        record["single_session_complete_coverage"] = (
            record["complete_session_count"] > 0
        )
        record["passed"] = (
            record["complete_session_count"] > 0
            and not record["errors"]
            and not any(
                write["handle"] == session["handle"]
                and (
                    write["object"] == session["object"]
                    or (
                        write["object"] == session["filesystem_object"]
                        and write["path"] is not None
                        and resolve_relative_path(write["path"], session["root"])[0]
                        == name
                    )
                )
                for write in writes
                for session in record["sessions"]
            )
        )
        if name in required_files and record["complete_session_count"] == 0:
            failures.append(
                f"{name}: no single open session covers [0,{record['declared_size']})"
            )
        failures.extend(f"{name}: {error}" for error in record["errors"])

    for required in required_files:
        if required not in file_records:
            failures.append(f"required save file not observed: {required}")

    if writes:
        failures.append(f"save-data write/flush/commit operations observed: {len(writes)}")

    total_returned_bytes = sum(session["returned_bytes"] for session in sessions)
    total_requested_bytes = sum(session["requested_bytes"] for session in sessions)

    return {
        "schema": "savedata-io-summary-v3",
        "log": str(log_path),
        "expected_size": expected_size,
        "expected_uid": expected_uid,
        "required_files": required_files,
        "save_requests": save_requests,
        "save_roots": roots,
        "identity_correlation": {
            "openfile_calls": openfile_calls,
            "openfile_successes": len(sessions),
            "openfile_failures": openfile_failures,
            "getsize_calls": getsize_calls,
            "read_calls": read_calls,
            "close_calls": close_calls,
            "save_metadata_calls": save_metadata_calls,
            "ambiguous_rejections": ambiguous_rejections,
            "filesystem_bindings": filesystem_bindings,
        },
        "total_returned_bytes": total_returned_bytes,
        "total_requested_bytes": total_requested_bytes,
        "files": file_records,
        "writes": writes,
        "out_of_scope": out_of_scope,
        "failures": failures,
        "passed": not failures,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument(
        "--expected-size",
        type=int,
        default=None,
        help="If set, every session's declared size must match this value.",
    )
    parser.add_argument("--expected-uid", default=None)
    parser.add_argument("--expected-root-substring", default=None)
    parser.add_argument(
        "--require-files",
        default="system,system_backup",
        help="Comma-separated relative save files that must be observed "
             "(default: system,system_backup).",
    )
    args = parser.parse_args(argv)
    required_files = [item.strip() for item in args.require_files.split(",") if item.strip()]
    result = summarize(
        args.log,
        args.expected_size,
        args.expected_uid,
        args.expected_root_substring,
        required_files,
    )
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
            newline="\n",
        )
    status = "PASS" if result["passed"] else "FAIL"
    total_sessions = sum(record["session_count"] for record in result["files"].values())
    complete_sessions = sum(
        record["complete_session_count"] for record in result["files"].values()
    )
    correlation = result["identity_correlation"]
    print(
        f"SAVEDATA_IO_{status} files={','.join(sorted(result['files'])) or 'none'} "
        f"sessions={total_sessions} complete_sessions={complete_sessions} "
        f"writes={len(result['writes'])} failures={len(result['failures'])} "
        f"opens={correlation['openfile_successes']}/{correlation['openfile_calls']} "
        f"getsize={correlation['getsize_calls']} reads={correlation['read_calls']}"
    )
    for name, record in sorted(result["files"].items()):
        print(
            f"SAVEDATA_IO_FILE name={name} declared={record['declared_size']} "
            f"sessions={record['session_count']} "
            f"complete_sessions={record['complete_session_count']} "
            f"single_session_complete={record['single_session_complete_coverage']} "
            f"combined_complete={record['combined_complete']} "
            f"errors={len(record['errors'])}"
        )
    for failure in result["failures"]:
        print(f"SAVEDATA_IO_FAIL {failure}", file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
