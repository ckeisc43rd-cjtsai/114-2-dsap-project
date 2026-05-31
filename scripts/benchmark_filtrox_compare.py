#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


PROJECT_ROOT = Path(__file__).resolve().parents[1]
WORKSPACE_ROOT = PROJECT_ROOT.parent


def load_python_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load Python module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def module_configs(config: Dict[str, Any]) -> Dict[str, Any]:
    modules = config.get("modules", config.get("params", {}).get("modules", config))
    if not isinstance(modules, dict):
        raise ValueError("config does not contain a module dictionary")
    return modules


def patch_xmp_with_original_python(xmp_gen, source_xmp: str, config: Dict[str, Any]) -> str:
    document = source_xmp
    for operation, module_config in module_configs(config).items():
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


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def parse_key_value_output(output: str) -> Dict[str, str]:
    parsed: Dict[str, str] = {}
    for line in output.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        parsed[key.strip()] = value.strip()
    return parsed


def executable_exists(path: str) -> bool:
    candidate = Path(path).expanduser()
    if candidate.is_absolute() or "/" in path:
        return candidate.is_file() and os.access(candidate, os.X_OK)
    return shutil.which(path) is not None


def resolve_from_project(path: Path) -> Path:
    return path if path.is_absolute() else (PROJECT_ROOT / path).resolve()


def run_checked(command: List[str], *, cwd: Path, env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=True,
        capture_output=True,
        text=True,
    )


def benchmark_original_python_xmp(
    original_root: Path,
    base_xmp: Path,
    config_path: Path,
    iterations: int,
) -> Dict[str, Any]:
    xmp_gen = load_python_module(original_root / "src" / "xmp_gen.py", "original_filtrox_xmp_gen")
    source = base_xmp.read_text(encoding="utf-8")
    config = read_json(config_path)

    output = ""
    start = time.perf_counter_ns()
    for _ in range(iterations):
        output = patch_xmp_with_original_python(xmp_gen, source, config)
    elapsed_us = (time.perf_counter_ns() - start) / 1000.0
    return {
        "name": "original_python_xmp",
        "iterations": iterations,
        "elapsed_us": int(elapsed_us),
        "avg_us": elapsed_us / iterations,
        "output_bytes": len(output.encode("utf-8")),
        "output_sha256": sha256_text(output),
    }


def benchmark_current_cpp_xmp(
    cpp_binary: Path,
    base_xmp: Path,
    config_path: Path,
    iterations: int,
) -> Dict[str, Any]:
    result = run_checked(
        [
            cpp_binary.as_posix(),
            "--benchmark",
            base_xmp.as_posix(),
            config_path.as_posix(),
            str(iterations),
        ],
        cwd=PROJECT_ROOT,
    )
    parsed = parse_key_value_output(result.stdout)
    return {
        "name": "current_cpp_xmp",
        "iterations": int(parsed.get("iterations", iterations)),
        "elapsed_us": int(float(parsed["elapsed_us"])),
        "avg_us": float(parsed["avg_us"]),
        "output_bytes_checksum": int(parsed.get("output_bytes_checksum", "0")),
        "stdout": result.stdout.strip(),
    }


def compare_single_xmp_output(
    original_root: Path,
    cpp_binary: Path,
    base_xmp: Path,
    config_path: Path,
) -> Dict[str, Any]:
    xmp_gen = load_python_module(original_root / "src" / "xmp_gen.py", "original_filtrox_xmp_compare")
    source = base_xmp.read_text(encoding="utf-8")
    config = read_json(config_path)
    python_output = patch_xmp_with_original_python(xmp_gen, source, config)

    with tempfile.TemporaryDirectory(prefix="filtrox_compare_xmp_") as tmp:
        cpp_output_path = Path(tmp) / "cpp_output.xmp"
        run_checked(
            [
                cpp_binary.as_posix(),
                base_xmp.as_posix(),
                config_path.as_posix(),
                cpp_output_path.as_posix(),
            ],
            cwd=PROJECT_ROOT,
        )
        cpp_output = cpp_output_path.read_text(encoding="utf-8")

    return {
        "byte_identical": python_output == cpp_output,
        "python_sha256": sha256_text(python_output),
        "cpp_sha256": sha256_text(cpp_output),
        "python_bytes": len(python_output.encode("utf-8")),
        "cpp_bytes": len(cpp_output.encode("utf-8")),
    }


def render_command(
    darktable_cli: str,
    image_path: Path,
    xmp_path: Path,
    output_path: Path,
    width: int,
    height: int,
) -> tuple[List[str], Path]:
    config_dir = Path(tempfile.mkdtemp(prefix="filtrox_dt_py_"))
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
        config_dir.as_posix(),
        "--disable-opencl",
    ], config_dir


def benchmark_original_python_render(
    original_root: Path,
    darktable_cli: str,
    image_path: Path,
    base_xmp: Path,
    config_paths: Iterable[Path],
    out_dir: Path,
    width: int,
    height: int,
) -> Dict[str, Any]:
    xmp_gen = load_python_module(original_root / "src" / "xmp_gen.py", "original_filtrox_xmp_render")
    source = base_xmp.read_text(encoding="utf-8")
    out_dir.mkdir(parents=True, exist_ok=True)

    per_job: List[Dict[str, Any]] = []
    total_start = time.perf_counter()
    for index, config_path in enumerate(config_paths, start=1):
        job_start = time.perf_counter()
        config = read_json(config_path)
        xmp_path = out_dir / f"python_variation_{index}.xmp"
        output_path = out_dir / f"python_variation_{index}.jpg"
        xmp_path.write_text(patch_xmp_with_original_python(xmp_gen, source, config), encoding="utf-8")
        command, config_dir = render_command(darktable_cli, image_path, xmp_path, output_path, width, height)
        try:
            run_checked(command, cwd=PROJECT_ROOT)
        finally:
            shutil.rmtree(config_dir, ignore_errors=True)
        per_job.append({
            "output": output_path.as_posix(),
            "elapsed_ms": int((time.perf_counter() - job_start) * 1000),
            "exists": output_path.exists(),
        })

    return {
        "name": "original_python_render_sequential_preview",
        "workers": 1,
        "jobs": len(per_job),
        "elapsed_ms": int((time.perf_counter() - total_start) * 1000),
        "per_job": per_job,
    }


def benchmark_current_cpp_render(
    cpp_binary: Path,
    darktable_cli: str,
    image_path: Path,
    base_xmp: Path,
    config_paths: List[Path],
    out_dir: Path,
    workers: int,
    width: int,
    height: int,
    timing: bool,
) -> Dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["DARKTABLE_CLI_PATH"] = darktable_cli
    env["FILTROX_PREVIEW_WIDTH"] = str(width)
    env["FILTROX_PREVIEW_HEIGHT"] = str(height)
    if timing:
        env["FILTROX_TIMING"] = "1"

    start = time.perf_counter()
    result = run_checked(
        [
            cpp_binary.as_posix(),
            "--render-preview",
            image_path.as_posix(),
            base_xmp.as_posix(),
            out_dir.as_posix(),
            str(workers),
            *[path.as_posix() for path in config_paths],
        ],
        cwd=PROJECT_ROOT,
        env=env,
    )
    parsed = parse_key_value_output(result.stdout)
    return {
        "name": "current_cpp_render_parallel_preview",
        "workers": workers,
        "jobs": len(config_paths),
        "elapsed_ms": int((time.perf_counter() - start) * 1000),
        "reported_total_elapsed_ms": int(parsed.get("total_elapsed_ms", "0")),
        "stdout": result.stdout.strip(),
        "stderr": result.stderr.strip(),
    }


def print_xmp_summary(python_result: Dict[str, Any], cpp_result: Dict[str, Any], output_compare: Dict[str, Any]) -> None:
    speedup = python_result["avg_us"] / cpp_result["avg_us"] if cpp_result["avg_us"] else 0.0
    print("XMP-only benchmark")
    print(f"  original Python avg: {python_result['avg_us']:.3f} us")
    print(f"  current C++ avg:     {cpp_result['avg_us']:.3f} us")
    print(f"  speedup:             {speedup:.2f}x")
    print(f"  byte-identical:      {output_compare['byte_identical']}")


def print_render_summary(python_result: Dict[str, Any], cpp_results: List[Dict[str, Any]]) -> None:
    print("Render benchmark")
    print(f"  original Python sequential preview: {python_result['elapsed_ms']} ms")
    for cpp_result in cpp_results:
        speedup = python_result["elapsed_ms"] / cpp_result["elapsed_ms"] if cpp_result["elapsed_ms"] else 0.0
        print(
            f"  current C++ preview workers={cpp_result['workers']}: "
            f"{cpp_result['elapsed_ms']} ms ({speedup:.2f}x)"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare original filtrox Python XMP/render path with the current C++ implementation."
    )
    parser.add_argument("--original-root", type=Path, default=WORKSPACE_ROOT / "filtrox")
    parser.add_argument("--cpp-binary", type=Path, default=PROJECT_ROOT / "build" / "filtrox_xmp")
    parser.add_argument("--image", type=Path, default=PROJECT_ROOT / "examples" / "hem_sample.jpeg")
    parser.add_argument("--base-xmp", type=Path, default=PROJECT_ROOT / "examples" / "hem_sample.xmp")
    parser.add_argument("--config", type=Path, default=PROJECT_ROOT / "examples" / "prototype_filter.json")
    parser.add_argument(
        "--render-configs",
        type=Path,
        nargs="+",
        default=[
            PROJECT_ROOT / "examples" / "variations" / "variation_1.json",
            PROJECT_ROOT / "examples" / "variations" / "variation_2.json",
            PROJECT_ROOT / "examples" / "variations" / "variation_3.json",
        ],
    )
    parser.add_argument("--iterations", type=int, default=10000)
    parser.add_argument("--render", action="store_true", help="also benchmark darktable preview rendering")
    parser.add_argument("--workers", type=int, nargs="+", default=[2])
    parser.add_argument("--preview-width", type=int, default=int(os.getenv("FILTROX_PREVIEW_WIDTH", "900")))
    parser.add_argument("--preview-height", type=int, default=int(os.getenv("FILTROX_PREVIEW_HEIGHT", "900")))
    parser.add_argument("--darktable-cli", default=os.getenv("DARKTABLE_CLI_PATH", "darktable-cli"))
    parser.add_argument("--out-dir", type=Path, default=PROJECT_ROOT / "build" / "benchmark_compare")
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--timing", action="store_true", help="enable FILTROX_TIMING for the C++ render path")
    args = parser.parse_args()

    args.original_root = resolve_from_project(args.original_root)
    args.cpp_binary = resolve_from_project(args.cpp_binary)
    args.image = resolve_from_project(args.image)
    args.base_xmp = resolve_from_project(args.base_xmp)
    args.config = resolve_from_project(args.config)
    args.render_configs = [resolve_from_project(path) for path in args.render_configs]
    args.out_dir = resolve_from_project(args.out_dir)
    if args.json_out:
        args.json_out = resolve_from_project(args.json_out)

    if args.iterations <= 0:
        raise SystemExit("--iterations must be positive")
    if not args.original_root.exists():
        raise SystemExit(f"original filtrox root not found: {args.original_root}")
    if not executable_exists(args.cpp_binary.as_posix()):
        raise SystemExit(f"C++ benchmark binary not found or not executable: {args.cpp_binary}")

    report: Dict[str, Any] = {
        "inputs": {
            "original_root": args.original_root.resolve().as_posix(),
            "cpp_binary": args.cpp_binary.resolve().as_posix(),
            "image": args.image.resolve().as_posix(),
            "base_xmp": args.base_xmp.resolve().as_posix(),
            "config": args.config.resolve().as_posix(),
            "iterations": args.iterations,
        }
    }

    python_xmp = benchmark_original_python_xmp(args.original_root, args.base_xmp, args.config, args.iterations)
    cpp_xmp = benchmark_current_cpp_xmp(args.cpp_binary, args.base_xmp, args.config, args.iterations)
    output_compare = compare_single_xmp_output(args.original_root, args.cpp_binary, args.base_xmp, args.config)
    report["xmp"] = {
        "original_python": python_xmp,
        "current_cpp": cpp_xmp,
        "output_compare": output_compare,
        "speedup": python_xmp["avg_us"] / cpp_xmp["avg_us"] if cpp_xmp["avg_us"] else 0.0,
    }
    print_xmp_summary(python_xmp, cpp_xmp, output_compare)

    if args.render:
        if not executable_exists(args.darktable_cli):
            raise SystemExit(f"darktable-cli not found or not executable: {args.darktable_cli}")
        render_root = args.out_dir / "render"
        python_render = benchmark_original_python_render(
            args.original_root,
            args.darktable_cli,
            args.image,
            args.base_xmp,
            args.render_configs,
            render_root / "python",
            args.preview_width,
            args.preview_height,
        )
        cpp_renders = [
            benchmark_current_cpp_render(
                args.cpp_binary,
                args.darktable_cli,
                args.image,
                args.base_xmp,
                list(args.render_configs),
                render_root / f"cpp_workers_{workers}",
                workers,
                args.preview_width,
                args.preview_height,
                args.timing,
            )
            for workers in args.workers
        ]
        best_cpp = min(cpp_renders, key=lambda item: item["elapsed_ms"])
        report["render"] = {
            "original_python": python_render,
            "current_cpp": cpp_renders,
            "best_cpp": best_cpp,
            "best_speedup": python_render["elapsed_ms"] / best_cpp["elapsed_ms"] if best_cpp["elapsed_ms"] else 0.0,
        }
        print()
        print_render_summary(python_render, cpp_renders)

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print()
        print(f"wrote JSON report: {args.json_out}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"command failed: {' '.join(exc.cmd)}", file=sys.stderr)
        if exc.stdout:
            print(exc.stdout, file=sys.stderr)
        if exc.stderr:
            print(exc.stderr, file=sys.stderr)
        raise SystemExit(exc.returncode)
