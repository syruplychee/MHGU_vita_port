#!/usr/bin/env python3
"""Build the per-stage texture-resource layout sidecar for the static bank.

The sidecar is derived only from offline evidence and never invents a runtime
binding:

* stage identity and stage index come from the locked ``shader-index.bin``
  (the exact SHA-256 and stage type of every stage);
* texture handle offsets/types come from the shader catalogue, which stores the
  Maxwell texture-descriptor metadata emitted by the pinned frontend;
* the catalogue's ``bank`` is the *offline environment* cbuf index (the
  compiler was configured with the bound-texture buffer at bank 0).  It is
  recorded as ``env_bank`` for audit only.  The runtime keeps using the live
  Maxwell texture-bound-buffer selector instead of copying this constant.

The tool refuses the whole package on an unknown descriptor shape, a duplicate
stage/offset, a stage that is missing from either source, or an inconsistent
Cg sampler binding order.  It writes no GXP and does not modify the index.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from collections import Counter
from pathlib import Path

MAGIC = b"VPSRES1\x00"
VERSION = 1
HEADER_SIZE = 96
INDEX_HEADER_SIZE = 64
STAGE_RECORD = 40
TEXTURE_RECORD = 16
FLAGS = 1
MAX_BYTES = 16 * 1024 * 1024
MAX_STAGES = 65535
# texture_start is a u16 in the fixed record.
MAX_TEXTURES = 65535
TYPE_VERTEX = 1
TYPE_FRAGMENT = 5
STAGE_NAMES = {"vert": TYPE_VERTEX, "frag": TYPE_FRAGMENT}
SUPPORTED_TEXTURE_TYPES = {
    0: "Color1D",
    1: "ColorArray1D",
    2: "Color2D",
    3: "ColorArray2D",
    4: "Color3D",
    5: "ColorCube",
    6: "ColorArrayCube",
    7: "Buffer",
    8: "Color2DRect",
}

# The runtime can expose a native depth allocation only through the reviewed
# 1-channel views, where .r is proven and the other channels are not.  The
# sidecar therefore records which components the exact compiled GLSL reads.
USAGE_R = 0x01
USAGE_G = 0x02
USAGE_B = 0x04
USAGE_A = 0x08
USAGE_MASK = 0x0F
USAGE_ANALYZED = 0x80
_COMPONENT_BITS = {
    "x": USAGE_R, "r": USAGE_R,
    "y": USAGE_G, "g": USAGE_G,
    "z": USAGE_B, "b": USAGE_B,
    "w": USAGE_A, "a": USAGE_A,
}


class LayoutError(Exception):
    pass


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest(path: Path) -> str:
    return digest_bytes(path.read_bytes())


def crc32_ieee(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)))
    return (~crc) & 0xFFFFFFFF


def read_index(path: Path):
    data = path.read_bytes()
    if len(data) < INDEX_HEADER_SIZE or data[:8] != b"VPSHIX1\x00":
        raise LayoutError("index magic/header mismatch")
    (version, header_size, flags) = struct.unpack_from("<HHI", data, 8)
    (stages, variants, programs, stage_size, variant_size, program_size,
     body_bytes, body_crc, abi) = struct.unpack_from("<IIIIIIIII", data, 16)
    if version != 1 or header_size != INDEX_HEADER_SIZE or flags != 1:
        raise LayoutError("unsupported index header")
    if stage_size != 48 or variant_size != 40 or program_size != 16:
        raise LayoutError("unsupported index record sizes")
    if abi not in (1, 2):
        raise LayoutError("unsupported index ABI")
    if stages == 0 or stages > MAX_STAGES or variants > 4096 or programs == 0:
        raise LayoutError("index record count out of range")
    if body_bytes != len(data) - INDEX_HEADER_SIZE:
        raise LayoutError("index body length mismatch")
    expected = stages * stage_size + variants * variant_size + programs * program_size
    if expected != body_bytes or expected > MAX_BYTES:
        raise LayoutError("index body size does not match record counts")
    body = data[INDEX_HEADER_SIZE:]
    if crc32_ieee(body) != body_crc:
        raise LayoutError("index body CRC mismatch")

    stage_records = []
    running_variants = 0
    for index in range(stages):
        record = body[index * stage_size:(index + 1) * stage_size]
        if len(record) != stage_size:
            raise LayoutError("short index stage record")
        stage_type = struct.unpack_from("<I", record, 0)[0]
        sha = record[4:36]
        variant_start, variant_count = struct.unpack_from("<HH", record, 36)
        source_flags = struct.unpack_from("<I", record, 40)[0]
        if stage_type not in (TYPE_VERTEX, TYPE_FRAGMENT):
            raise LayoutError("index stage type invalid")
        if source_flags & ~7:
            raise LayoutError("index stage source flags invalid")
        if variant_start != running_variants:
            raise LayoutError("index stage variant ranges not contiguous")
        if variant_start + variant_count > variants:
            raise LayoutError("index variant range invalid")
        running_variants += variant_count
        stage_records.append({
            "index": index,
            "type": stage_type,
            "sha256": sha.hex(),
            "variant_start": variant_start,
            "variant_count": variant_count,
            "source_flags": source_flags,
        })
    if running_variants != variants:
        raise LayoutError("index variant table is not fully owned")

    variant_records = []
    second_table = stages * stage_size
    for index in range(variants):
        offset = second_table + index * variant_size
        record = body[offset:offset + variant_size]
        if len(record) != variant_size:
            raise LayoutError("short index variant record")
        stage_index = struct.unpack_from("<I", record, 0)[0]
        viewport_mask, variant_abi = struct.unpack_from("<HH", record, 4)
        key = record[8:24]
        gxp_bytes = struct.unpack_from("<I", record, 24)[0]
        variant_flags = struct.unpack_from("<I", record, 28)[0]
        gxp_crc = struct.unpack_from("<I", record, 32)[0]
        input_signature = struct.unpack_from("<I", record, 36)[0]
        if stage_index >= stages:
            raise LayoutError("index variant stage index out of range")
        if variant_abi != abi:
            raise LayoutError("index variant ABI mismatch")
        if variant_flags & ~7 or not (variant_flags & 1):
            raise LayoutError("index variant flags invalid")
        if input_signature & ~0xffff:
            raise LayoutError("index variant input signature out of range")
        if abi == 1 and input_signature != 0:
            raise LayoutError("index ABI1 variant reserved word not zero")
        if viewport_mask == 0 or viewport_mask & ~3:
            raise LayoutError("index variant viewport mask invalid")
        if gxp_bytes < 32 or gxp_bytes > 16 * 1024 * 1024:
            raise LayoutError("index variant GXP size out of range")
        stage = stage_records[stage_index]
        position = index - stage["variant_start"]
        if position < 0 or position >= stage["variant_count"]:
            raise LayoutError("index variant outside its stage range")
        variant_records.append({
            "index": index,
            "stage_index": stage_index,
            "viewport_mask": viewport_mask,
            "gxp_bytes": gxp_bytes,
            "gxp_crc32": gxp_crc,
            "key": key.hex(),
            "input_signature": input_signature,
            "flags": variant_flags,
        })

    program_records = []
    third_table = second_table + variants * variant_size
    previous_token = None
    for index in range(programs):
        offset = third_table + index * program_size
        record = body[offset:offset + program_size]
        if len(record) != program_size:
            raise LayoutError("short index program record")
        token = struct.unpack_from("<I", record, 0)[0]
        vertex_stage, fragment_stage = struct.unpack_from("<HH", record, 4)
        program_flags = struct.unpack_from("<I", record, 8)[0]
        if previous_token is not None and token <= previous_token:
            raise LayoutError("index program tokens not strictly increasing")
        previous_token = token
        if program_flags & ~3:
            raise LayoutError("index program flags invalid")
        if vertex_stage == 0xffff and fragment_stage == 0xffff:
            raise LayoutError("index program has no stage")
        if vertex_stage != 0xffff:
            if vertex_stage >= stages or stage_records[vertex_stage]["type"] != TYPE_VERTEX:
                raise LayoutError("index program vertex stage invalid")
        if fragment_stage != 0xffff:
            if fragment_stage >= stages or stage_records[fragment_stage]["type"] != TYPE_FRAGMENT:
                raise LayoutError("index program fragment stage invalid")
        program_records.append({
            "index": index,
            "token": token,
            "vertex_stage": vertex_stage,
            "fragment_stage": fragment_stage,
            "flags": program_flags,
        })

    identities = {(entry["type"], entry["sha256"]) for entry in stage_records}
    if len(identities) != len(stage_records):
        raise LayoutError("index contains duplicate stage identities")

    return {
        "data": data,
        "sha256": digest_bytes(data),
        "crc32": crc32_ieee(data),
        "body_crc32": body_crc,
        "abi": abi,
        "variant_count": variants,
        "program_count": programs,
        "stages": stage_records,
        "variants": variant_records,
        "programs": program_records,
    }


def read_catalogue(path: Path):
    entries = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise LayoutError(f"catalogue line {line_number}: {error}") from error
            entries.append(record)
    return entries


def strip_glsl_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def matching_paren(text: str, open_pos: int) -> int:
    depth = 0
    for position in range(open_pos, len(text)):
        character = text[position]
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return position
    return -1


def swizzle_mask_at(text: str, position: int):
    match = re.match(r"\s*\.\s*([xyzwrgba]{1,4})\b", text[position:])
    if match is None:
        return None
    mask = 0
    for character in match.group(1):
        mask |= _COMPONENT_BITS[character]
    return mask


_SAMPLING_CALL = re.compile(
    r"\b(texture|textureLod|textureGrad|textureProj|textureOffset|"
    r"textureProjOffset|textureLodOffset|textureGradOffset|"
    r"texelFetch|texelFetchOffset)\s*\(\s*tex(\d+)\s*,")


def glsl_sampler_usage(text: str, slot: int):
    """Return (component mask, analyzed) for sampler `slot`.

    Only simple, direct generated-GLSL forms are understood.  A direct
    ``texture(tex, ...).xy`` access and a simple assignment to a scalar/vector
    variable followed by component accesses are tracked.  Any other escape
    (passing the sampled vector to a function, storing it in another carrier,
    using an unsupported intrinsic) is conservatively treated as reading all
    four components.  ``analyzed`` is false when the exact sampler uniform is
    absent, so an unknown descriptor never becomes a positive usage claim.
    """
    name = f"tex{slot}"
    if re.search(r"\buniform\s+sampler\w+\s+" + re.escape(name) + r"\b",
                 text) is None:
        return 0, False
    mask = 0
    unknown = False
    carriers = set()
    call_spans = []
    for match in _SAMPLING_CALL.finditer(text):
        if int(match.group(2)) != slot:
            continue
        open_pos = text.find("(", match.start())
        close_pos = matching_paren(text, open_pos)
        if close_pos < 0:
            unknown = True
            continue
        call_spans.append((match.start(), close_pos + 1))
        direct = swizzle_mask_at(text, close_pos + 1)
        if direct is not None:
            mask |= direct
            continue
        statement_start = max(
            text.rfind(";", 0, match.start()),
            text.rfind("{", 0, match.start()),
            text.rfind("}", 0, match.start())) + 1
        assignment = re.search(
            r"([A-Za-z_]\w*)\s*=\s*$", text[statement_start:match.start()])
        if assignment is not None:
            carriers.add(assignment.group(1))
        else:
            unknown = True
    # A sampler mention outside its declaration and supported sampling calls
    # is not part of the reviewed data flow.
    sanitized = text
    for start, end in reversed(call_spans):
        sanitized = sanitized[:start] + " " * (end - start) + sanitized[end:]
    sanitized = re.sub(
        r"\buniform\s+sampler\w+\s+" + re.escape(name) + r"\b", " ",
        sanitized)
    if re.search(r"\b" + re.escape(name) + r"\b", sanitized):
        unknown = True
    for carrier in sorted(carriers):
        # Ignore declarations of the carrier; only actual uses decide the mask.
        scan = re.sub(
            r"\b(?:const\s+)?(?:vec[234]|float|int|uint|ivec[234]|uvec[234]|"
            r"mat[234](?:x[234])?)\s+" + re.escape(carrier) + r"\b", " ", text)
        for use in re.finditer(r"\b" + re.escape(carrier) + r"\b", scan):
            direct = swizzle_mask_at(scan, use.end())
            if direct is not None:
                mask |= direct
                continue
            if re.match(r"\s*=", scan[use.end():use.end() + 8]):
                continue
            unknown = True
    if unknown:
        mask = USAGE_MASK
    return mask, True


_CG_SAMPLING_CALL = re.compile(
    r"\b(tex1D|tex2D|tex3D|texCUBE|tex1Dlod|tex2Dlod|tex3Dlod|tex1Dproj|"
    r"tex2Dproj|tex3Dproj|tex1Dbias|tex2Dbias|tex3Dbias|tex1Dgrad|"
    r"tex2Dgrad|tex3Dgrad)\s*\(\s*tex(\d+)\s*,")

_CG_CARRIER_DECLARATION = re.compile(
    r"\b(?:const\s+)?(?:float|half|fixed|double|int|uint|bool|"
    r"float[234]|half[234]|fixed[234]|int[234]|uint[234]|bool[234])\s+")


def cg_sampler_usage(text: str, slot: int):
    """Return (component mask, analyzed) for a Cg sampler uniform.

    This is the explicit emitter audit for the M6-B24 shadow stages: only direct
    ``tex2Dlod(texN, ...).x`` reads (or a direct swizzle on the call) are
    accepted as proven component usage.  Any other escape is conservatively
    treated as all four components, exactly like the GLSL analyzer.
    """
    name = f"tex{slot}"
    if re.search(r"\buniform\s+sampler\w+\s+" + re.escape(name) + r"\b",
                 text) is None:
        return 0, False
    mask = 0
    unknown = False
    carriers = set()
    call_spans = []
    for match in _CG_SAMPLING_CALL.finditer(text):
        if int(match.group(2)) != slot:
            continue
        open_pos = text.find("(", match.start())
        close_pos = matching_paren(text, open_pos)
        if close_pos < 0:
            unknown = True
            continue
        call_spans.append((match.start(), close_pos + 1))
        direct = swizzle_mask_at(text, close_pos + 1)
        if direct is not None:
            mask |= direct
            continue
        statement_start = max(
            text.rfind(";", 0, match.start()),
            text.rfind("{", 0, match.start()),
            text.rfind("}", 0, match.start())) + 1
        assignment = re.search(
            r"([A-Za-z_]\w*)\s*=\s*$", text[statement_start:match.start()])
        if assignment is not None:
            carriers.add(assignment.group(1))
        else:
            unknown = True
    sanitized = text
    for start, end in reversed(call_spans):
        sanitized = sanitized[:start] + " " * (end - start) + sanitized[end:]
    sanitized = re.sub(
        r"\buniform\s+sampler\w+\s+" + re.escape(name) + r"\b", " ",
        sanitized)
    if re.search(r"\b" + re.escape(name) + r"\b", sanitized):
        unknown = True
    for carrier in sorted(carriers):
        scan = _CG_CARRIER_DECLARATION.sub(" ", text)
        for use in re.finditer(r"\b" + re.escape(carrier) + r"\b", scan):
            direct = swizzle_mask_at(scan, use.end())
            if direct is not None:
                mask |= direct
                continue
            if re.match(r"\s*=", scan[use.end():use.end() + 8]):
                continue
            unknown = True
    if unknown:
        mask = USAGE_MASK
    return mask, True


def load_resolved_glsl(glsl_root: Path, record):
    shader_id = record.get("shader_id")
    if not isinstance(shader_id, str) or "/" in shader_id or "\\" in shader_id:
        raise LayoutError(f"invalid shader id for {shader_id!r}")
    stage_dir = glsl_root / shader_id
    resolution_path = stage_dir / "resolution.json"
    glsl_path = stage_dir / "shader.glsl"
    if not resolution_path.exists() or not glsl_path.exists():
        raise LayoutError(f"{shader_id}: resolved GLSL evidence missing")
    resolution = json.loads(resolution_path.read_text(encoding="utf-8"))
    if resolution.get("shader_id") != shader_id:
        raise LayoutError(f"{shader_id}: resolution shader identity mismatch")
    if resolution.get("maxwell_sha256") != record.get("maxwell_sha256"):
        raise LayoutError(f"{shader_id}: resolution Maxwell identity mismatch")
    resolved = resolution.get("resolved")
    if not isinstance(resolved, dict):
        raise LayoutError(f"{shader_id}: resolution has no resolved GLSL")
    data = glsl_path.read_bytes()
    if digest_bytes(data) != resolved.get("glsl_sha256"):
        raise LayoutError(f"{shader_id}: resolved GLSL hash mismatch")
    return strip_glsl_comments(data.decode("utf-8"))


def stage_textures(record, glsl_text: str):
    resources = record.get("resources")
    if not isinstance(resources, dict):
        raise LayoutError(f"{record.get('shader_id')}: missing resources")
    textures = resources.get("textures")
    if not isinstance(textures, list):
        raise LayoutError(f"{record.get('shader_id')}: missing resources.textures")
    if len(textures) > 16:
        raise LayoutError(f"{record.get('shader_id')}: too many texture descriptors")
    samplers = [m for m in record.get("cg_metadata", [])
                if isinstance(m, dict) and m.get("storage") == "sampler"]
    for slot, sampler in enumerate(samplers):
        if sampler.get("name") != f"tex{slot}" or sampler.get("binding") != slot:
            raise LayoutError(
                f"{record.get('shader_id')}: sampler order {slot} is "
                f"{sampler.get('name')}/{sampler.get('binding')}")
    if len(samplers) > len(textures):
        raise LayoutError(
            f"{record.get('shader_id')}: {len(samplers)} samplers but "
            f"{len(textures)} texture descriptors")
    result = []
    seen_offsets = set()
    for slot, texture in enumerate(textures):
        for field in ("type", "depth", "secondary", "bank", "offset", "count",
                      "shift", "size_shift"):
            if field not in texture:
                raise LayoutError(
                    f"{record.get('shader_id')}: descriptor {slot} missing {field}")
        texture_type = int(texture["type"])
        if texture_type not in SUPPORTED_TEXTURE_TYPES:
            raise LayoutError(
                f"{record.get('shader_id')}: unknown texture type {texture_type}")
        depth = int(texture["depth"])
        secondary = int(texture["secondary"])
        bank = int(texture["bank"])
        offset = int(texture["offset"])
        count = int(texture["count"])
        shift = int(texture["shift"])
        size_shift = int(texture["size_shift"])
        if depth not in (0, 1):
            raise LayoutError(f"{record.get('shader_id')}: depth flag {depth}")
        if secondary != 0:
            raise LayoutError(f"{record.get('shader_id')}: secondary descriptor")
        if bank < 0 or bank >= 18:
            raise LayoutError(f"{record.get('shader_id')}: bank {bank} out of range")
        if offset < 0 or offset > 4094 or offset % 8:
            raise LayoutError(f"{record.get('shader_id')}: offset {offset}")
        if offset in seen_offsets:
            raise LayoutError(f"{record.get('shader_id')}: duplicate offset {offset}")
        seen_offsets.add(offset)
        if count != 1 or shift != 0 or size_shift != 3:
            raise LayoutError(
                f"{record.get('shader_id')}: unsupported descriptor shape "
                f"count={count} shift={shift} size_shift={size_shift}")
        if slot < len(samplers):
            usage, analyzed = glsl_sampler_usage(glsl_text, slot)
        else:
            usage, analyzed = 0, False
        result.append({
            "slot": slot,
            "type": texture_type,
            "depth": depth,
            "secondary": secondary,
            "env_bank": bank,
            "offset": offset,
            "count": count,
            "shift": shift,
            "size_shift": size_shift,
            "usage": usage,
            "usage_analyzed": analyzed,
            "flags": usage | (USAGE_ANALYZED if analyzed else 0),
        })
    return result


def load_shadow_spec(path: Path):
    """Load shadow-stage source bindings and index them by shader id."""
    spec = json.loads(path.read_text(encoding="utf-8"))
    entries = spec.get("add")
    if not isinstance(entries, list) or not entries:
        raise LayoutError("shadow spec must declare additions")
    by_id = {}
    for entry in entries:
        shader_id = entry.get("shader_id")
        if not isinstance(shader_id, str) or shader_id in by_id:
            raise LayoutError("shadow spec has a missing/duplicate shader id")
        for field in ("stage_sha256", "cg_sha256", "cg_path", "gxp_sha256",
                      "environment_sha256", "policy_sha256", "emitter_sha256",
                      "stage_index"):
            if field not in entry:
                raise LayoutError(f"shadow spec entry {shader_id} lacks {field}")
        by_id[shader_id] = entry
    return spec, by_id


def shadow_stage_textures(record, glsl_text: str, glsl_root: Path,
                          spec_entry):
    """Audit one newly compiled shadow stage against its emitted Cg.

    Descriptor values still come from the locked catalogue/resources metadata;
    this function cross-checks the resolved reflection for exact equality, then
    derives component usage from the actual emitter Cg.  The depth slot must be
    an exact ``.r`` read, because the native depth view has no proven G/B/A
    mapping.
    """
    shader_id = record.get("shader_id")
    if spec_entry.get("shader_id") != shader_id:
        raise LayoutError("shadow spec shader identity mismatch")
    if spec_entry.get("stage_sha256") != record.get("maxwell_sha256"):
        raise LayoutError(f"{shader_id}: shadow spec stage SHA mismatch")
    if int(spec_entry.get("stage_index", -1)) < 0:
        raise LayoutError(f"{shader_id}: shadow spec stage index invalid")
    layout = stage_textures(record, glsl_text)
    stage_dir = glsl_root / shader_id
    reflection_path = stage_dir / "shader.glsl.resources.json"
    cg_path = Path(spec_entry["cg_path"])
    if not cg_path.is_absolute():
        cg_path = Path(__file__).resolve().parents[2] / cg_path
    reflection = json.loads(reflection_path.read_text(encoding="utf-8"))
    catalogue_textures = (record.get("resources") or {}).get("textures")
    if reflection.get("textures") != catalogue_textures:
        raise LayoutError(f"{shader_id}: reflection and catalogue descriptors differ")
    cg_data = cg_path.read_bytes()
    if digest_bytes(cg_data) != spec_entry["cg_sha256"]:
        raise LayoutError(f"{shader_id}: emitted Cg SHA differs from shadow spec")
    cg_text = cg_data.decode("utf-8")
    samplers = re.findall(r"uniform\s+sampler\w+\s+(tex\d+)\b", cg_text)
    expected = [f"tex{index}" for index in range(len(layout))]
    if samplers != expected:
        raise LayoutError(
            f"{shader_id}: Cg sampler declaration order {samplers} != {expected}")
    audited = []
    for texture in layout:
        slot = texture["slot"]
        usage, analyzed = cg_sampler_usage(cg_text, slot)
        if texture["depth"] == 1:
            if not analyzed or usage != USAGE_R:
                raise LayoutError(
                    f"{shader_id}: depth sampler tex{slot} usage 0x{usage:02x} "
                    f"analyzed={analyzed} is not the exact .r read")
            for suffix in ("_shadow_extent", "_shadow_state", "_shadow_border"):
                if not re.search(r"\buniform\s+float4\s+tex%d%s\b" % (slot, suffix),
                                 cg_text):
                    raise LayoutError(
                        f"{shader_id}: tex{slot}{suffix} uniform is missing")
        texture["usage"] = usage
        texture["usage_analyzed"] = analyzed
        texture["flags"] = usage | (USAGE_ANALYZED if analyzed else 0)
        audited.append({
            "slot": slot,
            "depth": texture["depth"],
            "usage": usage,
            "usage_analyzed": analyzed,
            "flags": texture["flags"],
        })
    return layout, {
        "shader_id": shader_id,
        "stage_index": int(spec_entry["stage_index"]),
        "stage_sha256": record["maxwell_sha256"],
        "cg_sha256": spec_entry["cg_sha256"],
        "policy_sha256": spec_entry["policy_sha256"],
        "emitter_sha256": spec_entry["emitter_sha256"],
        "reflection_equals_catalogue": True,
        "samplers": samplers,
        "textures": audited,
        "usage_rule": "exact Cg tex2Dlod(texN, ...).x for depth slots",
    }


def build(catalogue_path: Path, index_path: Path, glsl_root: Path,
          shadow_spec_path: Path | None = None):
    index = read_index(index_path)
    catalogue = read_catalogue(catalogue_path)
    shadow_spec = None
    shadow_by_id = {}
    if shadow_spec_path is not None:
        shadow_spec, shadow_by_id = load_shadow_spec(shadow_spec_path)
    by_identity = {}
    glsl_by_identity = {}
    for record in catalogue:
        stage = record.get("stage")
        if stage not in STAGE_NAMES:
            raise LayoutError(f"{record.get('shader_id')}: unknown stage {stage}")
        sha = record.get("maxwell_sha256")
        if not isinstance(sha, str) or len(sha) != 64:
            raise LayoutError(f"{record.get('shader_id')}: missing maxwell_sha256")
        key = (STAGE_NAMES[stage], sha)
        if key in by_identity:
            raise LayoutError(f"duplicate catalogue stage {key}")
        by_identity[key] = record
        glsl_by_identity[key] = load_resolved_glsl(glsl_root, record)
    index_identities = {(entry["type"], entry["sha256"]) for entry in index["stages"]}
    if len(index_identities) != len(index["stages"]):
        raise LayoutError("index contains duplicate stage identities")
    missing_catalogue = index_identities - set(by_identity)
    missing_index = set(by_identity) - index_identities
    if missing_catalogue or missing_index:
        raise LayoutError(
            f"catalogue/index stage sets differ: catalogue_missing={len(missing_catalogue)} "
            f"index_missing={len(missing_index)}")

    # Every compiled variant must point at a stage whose exact source metadata
    # is present in the catalogue. The stage-set comparison above already
    # implies this; walking the variant table makes an unknown/renumbered
    # variant an explicit package refusal rather than an inherited assumption.
    audited_variants = 0
    for variant in index["variants"]:
        stage = index["stages"][variant["stage_index"]]
        key = (stage["type"], stage["sha256"])
        if key not in by_identity:
            raise LayoutError(
                f"variant {variant['index']} references unknown stage {key}")
        audited_variants += 1
    if audited_variants != index["variant_count"]:
        raise LayoutError("variant audit count mismatch")

    stages = []
    textures = []
    type_histogram = Counter()
    offset_histogram = Counter()
    usage_histogram = Counter()
    depth_count = 0
    shadow_audits = []
    unaudited = []
    used_shadow = set()
    for entry in index["stages"]:
        key = (entry["type"], entry["sha256"])
        record = by_identity[key]
        shader_id = record.get("shader_id")
        if shader_id in shadow_by_id:
            layout, audit = shadow_stage_textures(
                record, glsl_by_identity[key], glsl_root, shadow_by_id[shader_id])
            if audit["stage_index"] != entry["index"]:
                raise LayoutError(
                    f"{shader_id}: shadow spec stage index differs from the index")
            shadow_audits.append(audit)
            used_shadow.add(shader_id)
        else:
            layout = stage_textures(record, glsl_by_identity[key])
            samplers = [m for m in (record.get("cg_metadata") or [])
                        if isinstance(m, dict) and m.get("storage") == "sampler"]
            if layout and not samplers:
                unaudited.append(shader_id)
        start = len(textures)
        textures.extend(layout)
        stages.append({
            "index": entry["index"],
            "type": entry["type"],
            "sha256": entry["sha256"],
            "texture_start": start,
            "texture_count": len(layout),
            "textures": layout,
            "shader_id": record.get("shader_id"),
        })
        type_histogram[entry["type"]] += 1
        for texture in layout:
            offset_histogram[texture["offset"]] += 1
            usage_histogram[texture["flags"]] += 1
            depth_count += texture["depth"]
    if shadow_spec is not None and used_shadow != set(shadow_by_id):
        missing = sorted(set(shadow_by_id) - used_shadow)
        raise LayoutError(f"shadow spec stages missing from the index: {missing}")

    # Sorted by (type, sha) so the runtime can binary-search without a second
    # table.  Texture ownership stays contiguous in stage order.
    order = sorted(range(len(stages)), key=lambda position: (
        stages[position]["type"], stages[position]["sha256"]))
    sorted_stages = []
    sorted_textures = []
    for position in order:
        stage = stages[position]
        start = len(sorted_textures)
        sorted_textures.extend(stage["textures"])
        stage = dict(stage)
        stage["texture_start"] = start
        sorted_stages.append(stage)

    stage_body = bytearray()
    for stage in sorted_stages:
        sha = bytes.fromhex(stage["sha256"])
        if len(sha) != 32:
            raise LayoutError("stage SHA length")
        stage_body += struct.pack(
            "<BBBBHH", stage["type"], 0, stage["texture_count"], 0,
            stage["texture_start"], stage["index"]) + sha
    texture_body = bytearray()
    for texture in sorted_textures:
        texture_body += struct.pack(
            "<BBBBHHBBBBI", texture["slot"], texture["type"], texture["depth"],
            texture["secondary"], texture["offset"], texture["count"],
            texture["shift"], texture["size_shift"], texture["env_bank"],
            texture["flags"], 0)
    body = bytes(stage_body + texture_body)
    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    struct.pack_into("<HHI", header, 8, VERSION, HEADER_SIZE, FLAGS)
    struct.pack_into("<IIIIII", header, 16, len(sorted_stages), len(sorted_textures),
                     STAGE_RECORD, TEXTURE_RECORD, len(body), crc32_ieee(body))
    header[40:72] = bytes.fromhex(index["sha256"])
    struct.pack_into("<I", header, 72, index["crc32"])
    image = bytes(header) + body
    if len(image) > MAX_BYTES:
        raise LayoutError("sidecar exceeds the size bound")
    return image, sorted_stages, sorted_textures, {
        "stage_types": dict(sorted(type_histogram.items())),
        "offsets": dict(sorted(offset_histogram.items())),
        "usage_flags": dict(sorted(usage_histogram.items())),
        "depth_descriptors": depth_count,
        "variants_audited": audited_variants,
        "catalogue_records": len(catalogue),
        "shadow_audits": sorted(shadow_audits, key=lambda row: row["stage_index"]),
        "usage_unaudited_stages": sorted(unaudited),
        "shadow_spec": None if shadow_spec_path is None else {
            "path": str(shadow_spec_path),
            "sha256": digest(shadow_spec_path),
            "stages": len(shadow_by_id),
        },
    }


def verify_image(image: bytes, stages, textures, index):
    if image[:8] != MAGIC:
        raise LayoutError("self-check magic")
    (version, header_size, flags) = struct.unpack_from("<HHI", image, 8)
    (stage_count, texture_count, stage_size, texture_size, body_bytes,
     body_crc) = struct.unpack_from("<IIIIII", image, 16)
    if version != VERSION or header_size != HEADER_SIZE or flags != FLAGS:
        raise LayoutError("self-check header")
    if stage_size != STAGE_RECORD or texture_size != TEXTURE_RECORD:
        raise LayoutError("self-check record sizes")
    if stage_count != len(stages) or texture_count != len(textures):
        raise LayoutError("self-check counts")
    body = image[HEADER_SIZE:]
    if len(body) != body_bytes or crc32_ieee(body) != body_crc:
        raise LayoutError("self-check body")
    if image[40:72].hex() != index["sha256"]:
        raise LayoutError("self-check parent SHA")
    if struct.unpack_from("<I", image, 72)[0] != index["crc32"]:
        raise LayoutError("self-check parent CRC")
    running = 0
    previous = None
    for position, stage in enumerate(stages):
        record = body[position * STAGE_RECORD:(position + 1) * STAGE_RECORD]
        stage_type, _flags, count, _reserved, start, stage_index = struct.unpack_from(
            "<BBBBHH", record, 0)
        sha = record[8:40]
        if stage_type != stage["type"] or count != stage["texture_count"]:
            raise LayoutError("self-check stage record")
        if start != running:
            raise LayoutError("self-check texture ownership")
        if stage_index != stage["index"]:
            raise LayoutError("self-check stage index")
        if sha.hex() != stage["sha256"]:
            raise LayoutError("self-check stage SHA")
        key = (stage_type, sha)
        if previous is not None and key <= previous:
            raise LayoutError("self-check stage ordering")
        previous = key
        running += count
    if running != texture_count:
        raise LayoutError("self-check texture table ownership")
    stage_record_bytes = stage_count * STAGE_RECORD
    for position, texture in enumerate(textures):
        record = body[stage_record_bytes + position * TEXTURE_RECORD:
                      stage_record_bytes + (position + 1) * TEXTURE_RECORD]
        if record[11] != texture["flags"]:
            raise LayoutError("self-check texture usage flags")


def read_sidecar_image(path: Path):
    """Decode a VPSRES1 sidecar into stage-SHA -> texture tuple records."""
    data = path.read_bytes()
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        raise LayoutError(f"baseline sidecar magic/header mismatch: {path}")
    (version, header_size, flags) = struct.unpack_from("<HHI", data, 8)
    (stage_count, texture_count, stage_size, texture_size, body_bytes,
     body_crc) = struct.unpack_from("<IIIIII", data, 16)
    if version != VERSION or header_size != HEADER_SIZE or flags != FLAGS:
        raise LayoutError("baseline sidecar header unsupported")
    if stage_size != STAGE_RECORD or texture_size != TEXTURE_RECORD:
        raise LayoutError("baseline sidecar record sizes unsupported")
    body = data[HEADER_SIZE:]
    if len(body) != body_bytes or crc32_ieee(body) != body_crc:
        raise LayoutError("baseline sidecar body mismatch")
    records = {}
    for position in range(stage_count):
        record = body[position * STAGE_RECORD:(position + 1) * STAGE_RECORD]
        stage_type, _reserved, count, _reserved2, start, _index = \
            struct.unpack_from("<BBBBHH", record, 0)
        sha = record[8:40].hex()
        key = (stage_type, sha)
        if key in records:
            raise LayoutError("baseline sidecar has duplicate stage identity")
        rows = []
        for offset in range(start, start + count):
            item = body[stage_count * STAGE_RECORD + offset * TEXTURE_RECORD:
                        stage_count * STAGE_RECORD + (offset + 1) * TEXTURE_RECORD]
            if len(item) != TEXTURE_RECORD:
                raise LayoutError("baseline sidecar short texture record")
            (slot, texture_type, depth, secondary, texture_offset, texture_count,
             shift, size_shift, env_bank, usage_flags, _reserved3) = \
                struct.unpack("<BBBBHHBBBBI", item)
            rows.append((slot, texture_type, depth, secondary, texture_offset,
                         texture_count, shift, size_shift, env_bank, usage_flags))
        records[key] = tuple(rows)
    return records


def compare_baseline_sidecar(baseline_path: Path, stages, shadow_by_id):
    old = read_sidecar_image(baseline_path)
    new = {(stage["type"], stage["sha256"]): tuple(
        (texture["slot"], texture["type"], texture["depth"],
         texture["secondary"], texture["offset"], texture["count"],
         texture["shift"], texture["size_shift"], texture["env_bank"],
         texture["flags"]) for texture in stage["textures"])
        for stage in stages}
    expected_changed = {(5, shadow_by_id[shader_id]["stage_sha256"])
                        for shader_id in shadow_by_id}
    changed = sorted(key[1] for key in set(old) | set(new)
                     if old.get(key) != new.get(key))
    unexpected = sorted(sha for sha in changed if (5, sha) not in expected_changed)
    missing = sorted((set(old) ^ set(new)) and
                     (key[1] for key in set(old) ^ set(new)))
    changed_expected = sorted(sha for sha in changed if (5, sha) in expected_changed)
    return {
        "baseline": str(baseline_path),
        "baseline_sha256": digest(baseline_path),
        "stages_baseline": len(old),
        "stages_candidate": len(new),
        "expected_changed_stages": len(expected_changed),
        "changed_stages": len(changed),
        "changed_stage_sha256": changed_expected,
        "unexpected_changed_stage_sha256": unexpected,
        "missing_or_extra_stage_sha256": missing,
        "only_shadow_stage_records_changed":
            not unexpected and not missing and len(changed_expected) == len(expected_changed),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalogue", required=True, type=Path)
    parser.add_argument("--index", required=True, type=Path)
    parser.add_argument("--glsl-root", required=True, type=Path,
                        help="directory of <shader_id>/resolution.json and shader.glsl")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--shadow-spec", type=Path, default=None,
                        help="validated M6-B24 shadow augmentation spec; nine "
                             "depth stages get an explicit emitted-Cg .r audit")
    parser.add_argument("--baseline-sidecar", type=Path, default=None,
                        help="previous VPSRES1 sidecar; only the nine shadow "
                             "stage records may differ")
    args = parser.parse_args()

    try:
        image, stages, textures, summary = build(
            args.catalogue, args.index, args.glsl_root, args.shadow_spec)
        index = read_index(args.index)
        verify_image(image, stages, textures, index)
        baseline = None
        if args.baseline_sidecar is not None:
            if args.shadow_spec is None:
                raise LayoutError("baseline comparison requires --shadow-spec")
            _, shadow_by_id = load_shadow_spec(args.shadow_spec)
            baseline = compare_baseline_sidecar(
                args.baseline_sidecar, stages, shadow_by_id)
            if not baseline["only_shadow_stage_records_changed"]:
                raise LayoutError(
                    "baseline sidecar differs outside the nine shadow stages")
    except LayoutError as error:
        print(json.dumps({"status": "rejected", "error": str(error)}))
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    stage_429 = next((s for s in stages if s["index"] == 429), None)
    stage_2377 = next((s for s in stages if s["index"] == 2377), None)
    manifest = {
        "schema": 1,
        "status": "built",
        "scope": "offline per-stage texture resource layout for the locked 1.4.0 bank",
        "tool_sha256": digest(Path(__file__)),
        "catalogue": {
            "path": str(args.catalogue),
            "sha256": digest(args.catalogue),
            "records": summary["catalogue_records"],
        },
        "glsl": {
            "root": str(args.glsl_root),
            "identity": "each stage resolution.json must name the same shader_id "
                        "and maxwell_sha256 and its resolved GLSL hash must match "
                        "the file read by this tool",
        },
        "index": {
            "path": str(args.index),
            "sha256": index["sha256"],
            "crc32": f"{index['crc32']:08x}",
            "crc32_scope": "entire shader-index.bin",
            "body_crc32": f"{index['body_crc32']:08x}",
            "body_crc32_scope": "index body only; not the sidecar parent identity",
            "stages": len(index["stages"]),
            "variants": index["variant_count"],
            "programs": index["program_count"],
            "abi": index["abi"],
            "variants_audited": summary["variants_audited"],
        },
        "sidecar": {
            "path": str(args.output),
            "sha256": digest_bytes(image),
            "bytes": len(image),
            "stage_records": len(stages),
            "texture_records": len(textures),
        },
        "summary": summary,
        "shadow_usage_audit": {
            "spec": summary["shadow_spec"],
            "stages": summary["shadow_audits"],
            "count": len(summary["shadow_audits"]),
            "rule": "descriptor values are exact catalogue/resolved-reflection "
                    "metadata; component usage is derived from the emitted Cg "
                    "and every depth slot must be an exact .r read",
        },
        "usage_unaudited": {
            "count": len(summary["usage_unaudited_stages"]),
            "stages": summary["usage_unaudited_stages"],
            "reason": "catalogue cg_metadata has no sampler entry and no M6-B24 "
                      "emitter override; usage stays 0/not-analyzed, so no "
                      "depth-view consumption is claimed for these stages",
        },
        "baseline_comparison": baseline,
        "witness_stages": {
            "429": stage_429,
            "2377": stage_2377,
        },
        "bank_semantics": (
            "env_bank is the offline compiler environment cbuf index (all zero). "
            "The runtime resolves the live texture-bound buffer from Maxwell "
            "state and never substitutes this constant."
        ),
        "limitations": [
            "Offsets and types are compiler metadata, not proof of linkage or pixels.",
            "The sidecar changes no index bytes, GXP bytes or flags.",
            "Runtime supports only the reviewed typed descriptor shape (count 1, "
            "shift 0, size_shift 3, secondary 0); other records are rejected.",
            "Usage flags are conservative GLSL/Cg data-flow facts. A depth view "
            "is accepted only when the exact stage reads no channel other than .r; "
            "the native depth views have no proven .g/.b/.a mapping.",
            "The nine M6-B24 shadow stages use emitted-Cg usage audits; other "
            "stages keep the historical catalogue/GLSL path. A successful "
            "sidecar build is not linkage, pixel or runtime evidence.",
        ],
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(
        json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n")
    print(json.dumps({
        "status": "built",
        "sidecar_sha256": manifest["sidecar"]["sha256"],
        "stages": len(stages),
        "textures": len(textures),
        "bytes": len(image),
        "offsets": summary["offsets"],
    }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
