#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import json
import re
import shlex
import struct
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class ModulePatch:
    operation: str
    attributes: list[tuple[str, str]] = field(default_factory=list)


HEX_OPS = {
    "exposure",
    "sigmoid",
    "toneequal",
    "temperature",
    "diffuse",
    "hazeremoval",
    "vignette",
    "grain",
}

MODULE_SPECS: dict[str, list[tuple[str, str, int]]] = {
    "exposure": [
        ("mode", "i", 1), ("black", "f", 1), ("exposure", "f", 1),
        ("deflicker_percentile", "f", 1), ("deflicker_target_level", "f", 1),
        ("compensate_exposure_bias", "i", 1), ("compensate_hilite_pres", "i", 1),
    ],
    "sigmoid": [
        ("middle_grey_contrast", "f", 1), ("contrast_skewness", "f", 1),
        ("display_white_target", "f", 1), ("display_black_target", "f", 1),
        ("color_processing", "i", 1), ("hue_preservation", "f", 1),
        ("red_inset", "f", 1), ("red_rotation", "f", 1),
        ("green_inset", "f", 1), ("green_rotation", "f", 1),
        ("blue_inset", "f", 1), ("blue_rotation", "f", 1),
        ("purity", "f", 1), ("base_primaries", "i", 1),
    ],
    "toneequal": [
        ("noise", "f", 1), ("ultra_deep_blacks", "f", 1), ("deep_blacks", "f", 1),
        ("blacks", "f", 1), ("shadows", "f", 1), ("midtones", "f", 1),
        ("highlights", "f", 1), ("whites", "f", 1), ("speculars", "f", 1),
        ("blending", "f", 1), ("smoothing", "f", 1), ("feathering", "f", 1),
        ("quantization", "f", 1), ("contrast_boost", "f", 1),
        ("exposure_boost", "f", 1), ("details", "i", 1),
        ("method", "i", 1), ("iterations", "i", 1),
    ],
    "temperature": [
        ("red", "f", 1), ("green", "f", 1), ("blue", "f", 1),
        ("various", "f", 1), ("preset", "i", 1),
    ],
    "diffuse": [
        ("iterations", "i", 1), ("sharpness", "f", 1), ("radius", "i", 1),
        ("regularization", "f", 1), ("variance_threshold", "f", 1),
        ("anisotropy_first", "f", 1), ("anisotropy_second", "f", 1),
        ("anisotropy_third", "f", 1), ("anisotropy_fourth", "f", 1),
        ("threshold", "f", 1), ("first", "f", 1), ("second", "f", 1),
        ("third", "f", 1), ("fourth", "f", 1), ("radius_center", "i", 1),
    ],
    "hazeremoval": [
        ("strength", "f", 1), ("distance", "f", 1), ("slope", "f", 1),
        ("saturation", "f", 1), ("unbound", "i", 1), ("iterations", "i", 1),
    ],
    "vignette": [
        ("scale", "f", 1), ("falloff_scale", "f", 1), ("brightness", "f", 1),
        ("saturation", "f", 1), ("center", "f", 2), ("autoratio", "i", 1),
        ("whratio", "f", 1), ("shape", "f", 1), ("dithering", "i", 1),
        ("unbound", "i", 1),
    ],
    "grain": [
        ("channel", "i", 1), ("scale", "f", 1), ("strength", "f", 1),
        ("midtones", "f", 1),
    ],
    "colorbalancergb": [
        ("shadows_Y", "f", 1), ("shadows_C", "f", 1), ("shadows_H", "f", 1),
        ("midtones_Y", "f", 1), ("midtones_C", "f", 1), ("midtones_H", "f", 1),
        ("highlights_Y", "f", 1), ("highlights_C", "f", 1), ("highlights_H", "f", 1),
        ("global_Y", "f", 1), ("global_C", "f", 1), ("global_H", "f", 1),
        ("shadows_weight", "f", 1), ("white_fulcrum", "f", 1), ("highlights_weight", "f", 1),
        ("chroma_shadows", "f", 1), ("chroma_highlights", "f", 1),
        ("chroma_global", "f", 1), ("chroma_midtones", "f", 1),
        ("saturation_global", "f", 1), ("saturation_highlights", "f", 1),
        ("saturation_midtones", "f", 1), ("saturation_shadows", "f", 1),
        ("hue_angle", "f", 1), ("brilliance_global", "f", 1),
        ("brilliance_highlights", "f", 1), ("brilliance_midtones", "f", 1),
        ("brilliance_shadows", "f", 1), ("mask_grey_fulcrum", "f", 1),
        ("vibrance", "f", 1), ("grey_fulcrum", "f", 1), ("contrast", "f", 1),
        ("saturation_formula", "i", 1),
    ],
    "colorequal": [
        ("threshold", "f", 1), ("smoothing_hue", "f", 1), ("contrast", "f", 1),
        ("white_level", "f", 1), ("chroma_size", "f", 1), ("param_size", "f", 1),
        ("use_filter", "i", 1), ("sat_red", "f", 1), ("sat_orange", "f", 1),
        ("sat_yellow", "f", 1), ("sat_green", "f", 1), ("sat_cyan", "f", 1),
        ("sat_blue", "f", 1), ("sat_lavender", "f", 1), ("sat_magenta", "f", 1),
        ("hue_red", "f", 1), ("hue_orange", "f", 1), ("hue_yellow", "f", 1),
        ("hue_green", "f", 1), ("hue_cyan", "f", 1), ("hue_blue", "f", 1),
        ("hue_lavender", "f", 1), ("hue_magenta", "f", 1),
        ("bright_red", "f", 1), ("bright_orange", "f", 1),
        ("bright_yellow", "f", 1), ("bright_green", "f", 1),
        ("bright_cyan", "f", 1), ("bright_blue", "f", 1),
        ("bright_lavender", "f", 1), ("bright_magenta", "f", 1),
        ("hue_shift", "f", 1),
    ],
}


def normalize_darktable_attr(name: str) -> str:
    if ":" in name:
        return name
    return f"darktable:{name}"


def dt_encode_gz(raw: bytes) -> str:
    compressed = zlib.compress(raw, level=9)
    factor = min((len(raw) // len(compressed)) + 1, 99)
    return f"gz{factor:02d}" + base64.b64encode(compressed).decode("ascii")


def build_raw_params(operation: str, params: dict) -> bytes | None:
    spec = MODULE_SPECS.get(operation)
    if spec is None:
        return None
    chunks: list[bytes] = []
    for field, field_type, count in spec:
        value = params.get(field)
        values = value if isinstance(value, list) else [value] * count
        for index in range(count):
            item = values[index] if index < len(values) else None
            if field_type == "f":
                chunks.append(struct.pack("<f", float(item or 0.0)))
            else:
                chunks.append(struct.pack("<i", int(item or 0)))
    return b"".join(chunks)


def build_params_string(operation: str, module_config: dict) -> str | None:
    if isinstance(module_config.get("params_gz"), str) and module_config["params_gz"].startswith("gz"):
        return module_config["params_gz"]
    if isinstance(module_config.get("params_hex"), str):
        return module_config["params_hex"].strip().lower()

    params = module_config.get("params")
    if not isinstance(params, dict):
        return None
    raw = build_raw_params(operation, params)
    if raw is None:
        return None
    expected = module_config.get("_expected_bytes")
    if isinstance(expected, int) and expected > 0 and len(raw) != expected:
        return None

    force_format = module_config.get("format")
    if force_format == "hex" or (force_format is None and operation in HEX_OPS):
        return raw.hex()
    return dt_encode_gz(raw)


def patch_from_json_module(operation: str, module_config: dict) -> ModulePatch:
    patch = ModulePatch(operation=operation)
    for key, value in module_config.items():
        if key in {"params", "params_gz", "params_hex", "format", "_expected_bytes"}:
            continue
        if isinstance(value, bool):
            patch.attributes.append((normalize_darktable_attr(key), "1" if value else "0"))
        elif isinstance(value, (int, float, str)):
            patch.attributes.append((normalize_darktable_attr(key), str(value)))

    params = build_params_string(operation, module_config)
    if params is not None:
        patch.attributes.append(("darktable:params", params))
    return patch


def parse_json_config(path: Path) -> list[ModulePatch]:
    config = json.loads(path.read_text(encoding="utf-8"))
    modules = config.get("modules", config.get("params", {}).get("modules", config))
    return [
        patch_from_json_module(operation, module_config)
        for operation, module_config in modules.items()
        if isinstance(module_config, dict)
    ]


def parse_config(path: Path) -> list[ModulePatch]:
    if path.suffix == ".json":
        return parse_json_config(path)

    patches: list[ModulePatch] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        parts = shlex.split(line)
        if not parts:
            continue

        patch = ModulePatch(operation=parts[0])
        for token in parts[1:]:
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            attr = normalize_darktable_attr(key)
            if attr == "darktable:operation":
                patch.operation = value
            else:
                patch.attributes.append((attr, value))
        patches.append(patch)
    return patches


def replace_or_insert_attr(tag: str, attr: str, value: str) -> str:
    pattern = re.compile(rf"(\s{re.escape(attr)}=)([\"'])(.*?)(\2)", flags=re.DOTALL)
    if pattern.search(tag):
        return pattern.sub(lambda match: match.group(1) + match.group(2) + value + match.group(4), tag, count=1)
    if tag.endswith("/>"):
        return tag[:-2] + f' {attr}="{value}"' + "/>"
    if tag.endswith(">"):
        return tag[:-1] + f' {attr}="{value}"' + ">"
    return tag


def patch_operation(document: str, patch: ModulePatch) -> tuple[str, bool]:
    operation = re.escape(patch.operation)
    pattern = re.compile(
        rf'(<rdf:li\b[^>]*\bdarktable:operation=(["\']){operation}\2[^>]*>)',
        flags=re.DOTALL,
    )
    match = pattern.search(document)
    if not match:
        return document, False

    tag = match.group(1)
    for attr, value in patch.attributes:
        tag = replace_or_insert_attr(tag, attr, value)
    return document[: match.start(1)] + tag + document[match.end(1) :], True


def patch_document(source: str, patches: list[ModulePatch]) -> str:
    document = source
    for patch in patches:
        document, _ = patch_operation(document, patch)
    return document


def benchmark(source: str, patches: list[ModulePatch], iterations: int) -> tuple[int, float, int]:
    output_size = 0
    start = time.perf_counter_ns()
    for _ in range(iterations):
        output = patch_document(source, patches)
        output_size += len(output)
    elapsed_us = (time.perf_counter_ns() - start) / 1000.0
    return int(elapsed_us), elapsed_us / iterations, output_size


def main() -> int:
    parser = argparse.ArgumentParser(description="Python baseline benchmark for Filtrox JSON-to-XMP generation.")
    parser.add_argument("input_xmp", type=Path)
    parser.add_argument("config", type=Path)
    parser.add_argument("iterations", type=int)
    args = parser.parse_args()

    if args.iterations <= 0:
        raise SystemExit("iterations must be positive")

    source = args.input_xmp.read_text(encoding="utf-8")
    patches = parse_config(args.config)
    elapsed_us, avg_us, output_size = benchmark(source, patches, args.iterations)

    print(f"iterations: {args.iterations}")
    print(f"elapsed_us: {elapsed_us}")
    print(f"avg_us: {avg_us:.3f}")
    print(f"output_bytes_checksum: {output_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
