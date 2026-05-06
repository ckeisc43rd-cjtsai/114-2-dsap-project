#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


def load_xmp_gen():
    path = Path(__file__).resolve().parents[2] / "filtrox" / "src" / "xmp_gen.py"
    spec = importlib.util.spec_from_file_location("xmp_gen", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def patch_xmp_with_original_logic(xmp_gen, source_xmp: str, config: dict) -> str:
    modules = config.get("modules", config.get("params", {}).get("modules", config))
    document = source_xmp
    for operation, module_config in modules.items():
        if not isinstance(module_config, dict):
            continue
        enabled = module_config.get("enabled")
        modversion = module_config.get("modversion")
        enabled_val = int(enabled) if isinstance(enabled, (int, float, bool)) else None
        modversion_val = int(modversion) if isinstance(modversion, (int, float)) else None
        params = xmp_gen.build_params_string(operation, module_config)
        document, ok = xmp_gen.patch_operation(
            document,
            operation,
            enabled_val,
            modversion_val,
            params,
        )
        if not ok:
            document, _ = xmp_gen.insert_operation(
                document,
                operation,
                enabled_val,
                modversion_val,
                params,
            )
    return document


def render_command(
    darktable_cli: str,
    image_path: Path,
    xmp_path: Path,
    output_path: Path,
    width: int,
    height: int,
) -> tuple[list[str], str]:
    tmp_config = tempfile.mkdtemp(prefix="filtrox_dt_py_")
    return [
        darktable_cli,
        image_path.as_posix(),
        xmp_path.as_posix(),
        output_path.as_posix(),
        "--width",
        str(width),
        "--height",
        str(height),
        "--core",
        "--configdir",
        tmp_config,
        "--disable-opencl",
    ], tmp_config


def main() -> int:
    parser = argparse.ArgumentParser(description="Sequential Python render benchmark using filtrox xmp_gen.py logic.")
    parser.add_argument("image")
    parser.add_argument("base_xmp")
    parser.add_argument("out_dir")
    parser.add_argument("configs", nargs="+")
    parser.add_argument("--darktable-cli", default=os.environ.get("DARKTABLE_CLI_PATH", "darktable-cli"))
    parser.add_argument("--preview-width", type=int, default=int(os.environ.get("FILTROX_PREVIEW_WIDTH", "1200")))
    parser.add_argument("--preview-height", type=int, default=int(os.environ.get("FILTROX_PREVIEW_HEIGHT", "1200")))
    args = parser.parse_args()

    if shutil.which(args.darktable_cli) is None:
        raise SystemExit(f"darktable-cli not found: {args.darktable_cli}")

    xmp_gen = load_xmp_gen()
    image = Path(args.image)
    base_xmp = Path(args.base_xmp).read_text(encoding="utf-8")
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    start = time.perf_counter()
    for index, config_path in enumerate(args.configs, start=1):
        config = json.loads(Path(config_path).read_text(encoding="utf-8"))
        xmp_path = out_dir / f"variation_{index}_preview.xmp"
        output_path = out_dir / f"variation_{index}_preview.jpg"
        xmp_path.write_text(patch_xmp_with_original_logic(xmp_gen, base_xmp, config), encoding="utf-8")
        command, tmp_config = render_command(
            args.darktable_cli,
            image,
            xmp_path,
            output_path,
            args.preview_width,
            args.preview_height,
        )
        try:
            subprocess.run(command, check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp_config, ignore_errors=True)
    elapsed_ms = int((time.perf_counter() - start) * 1000)
    print(f"variations: {len(args.configs)}")
    print(f"workers: 1")
    print(f"total_elapsed_ms: {elapsed_ms}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
