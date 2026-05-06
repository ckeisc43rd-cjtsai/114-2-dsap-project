#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

echo
echo "Listing modules from examples/hem_sample.xmp"
build/filtrox_xmp --list-modules examples/hem_sample.xmp

echo
echo "Patching examples/hem_sample.xmp -> build/hem_output.xmp"
build/filtrox_xmp examples/hem_sample.xmp examples/prototype_filter.json build/hem_output.xmp

echo
echo "Benchmarking prototype on examples/hem_sample.xmp"
build/filtrox_xmp --benchmark examples/hem_sample.xmp examples/prototype_filter.json 10000

echo
echo "Benchmarking Python baseline on examples/hem_sample.xmp"
python3 scripts/benchmark_python.py examples/hem_sample.xmp examples/prototype_filter.json 10000

echo
DT_CLI="${DARKTABLE_CLI_PATH:-darktable-cli}"
if ! command -v "$DT_CLI" >/dev/null 2>&1; then
  for candidate in \
    "/Applications/darktable.app/Contents/MacOS/darktable-cli" \
    "/Applications/Darktable.app/Contents/MacOS/darktable-cli" \
    "/opt/homebrew/bin/darktable-cli" \
    "/usr/local/bin/darktable-cli" \
    "/opt/local/bin/darktable-cli"
  do
    if [[ -x "$candidate" ]]; then
      DT_CLI="$candidate"
      export DARKTABLE_CLI_PATH="$DT_CLI"
      break
    fi
  done
fi

if command -v "$DT_CLI" >/dev/null 2>&1; then
  echo "Using darktable-cli: $DT_CLI"
  echo "Rendering 3 low-res previews with C++ parallel renderer"
  build/filtrox_xmp --render-preview \
    examples/hem_sample.jpeg \
    examples/hem_sample.xmp \
    build/render_cpp_preview \
    3 \
    examples/variations/variation_1.json \
    examples/variations/variation_2.json \
    examples/variations/variation_3.json

  echo
  echo "Rendering 3 low-res previews with original Python/filtrox sequential renderer"
  python3 scripts/benchmark_render_python.py \
    examples/hem_sample.jpeg \
    examples/hem_sample.xmp \
    build/render_python_preview \
    examples/variations/variation_1.json \
    examples/variations/variation_2.json \
    examples/variations/variation_3.json
else
  echo "Skipping render benchmark: darktable-cli is not available."
  echo "If it works in your terminal, run: DARKTABLE_CLI_PATH=\$(command -v darktable-cli) ./run.sh"
fi
