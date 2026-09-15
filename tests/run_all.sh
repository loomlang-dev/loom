#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOOM_EXEC="${LOOM_EXEC:-$ROOT/build/loom}"

TEST_DIR="$ROOT/tests"

if [ ! -d "$TEST_DIR" ]; then
  echo "No $TEST_DIR directory found; create tests/*.loom test files."
  exit 2
fi

if [ ! -x "$LOOM_EXEC" ]; then
  echo "Error: loom executable not found at $LOOM_EXEC"
  echo "Build the project (cmake --build build) or set LOOM_EXEC to the loom binary path."
  exit 2
fi

OUTDIR="$ROOT/tests/out"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

failures=0
shopt -s nullglob
for f in "$TEST_DIR"/*.loom; do
  name=$(basename "$f" .loom)
  echo "=== Running test: $name ==="
  od="$OUTDIR/$name"
  rm -rf "$od"
  mkdir -p "$od"

  echo "Compiling $f -> $od"
  "$LOOM_EXEC" "$f" -o "$od"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "FAIL: $name (compiler exited $rc)"
    failures=$((failures+1))
    continue
  fi

  if [ ! -f "$od/pack.mcmeta" ]; then
    echo "FAIL: $name missing pack.mcmeta"
    failures=$((failures+1))
    continue
  fi

  if ! find "$od/data" -type f -name '*.mcfunction' | grep -q .; then
    echo "FAIL: $name produced no mcfunction files"
    failures=$((failures+1))
    continue
  fi

  if [ "$name" = "test_func_ref" ] && ! grep -rq "internal_call_ref_int" "$od/data"; then
    echo "FAIL: $name did not emit the indirect-call helper (internal_call_ref_int)"
    failures=$((failures+1))
    continue
  fi

  if [ "$name" = "test_lambda" ]; then
    if ! find "$od/data" -name '__lambda_*.mcfunction' | grep -q .; then
      echo "FAIL: $name did not emit any synthesized lambda functions"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "internal_call_ref_int" "$od/data"; then
      echo "FAIL: $name did not emit the indirect-call helper (internal_call_ref_int)"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "closureEnv_" "$od/data"; then
      echo "FAIL: $name did not emit a capturing-closure environment (closureEnv_*)"
      failures=$((failures+1))
      continue
    fi
  fi

  echo "PASS: $name"
done

PROJECTS_DIR="$TEST_DIR/projects"
for d in "$PROJECTS_DIR"/*/; do
  [ -d "$d" ] || continue
  name=$(basename "$d")
  echo "=== Running project test: $name ==="
  od="$OUTDIR/$name"
  rm -rf "$od"
  mkdir -p "$od"

  echo "Compiling $d/main.loom -> $od"
  "$LOOM_EXEC" "$d/main.loom" -b "$d" -o "$od"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "FAIL: $name (compiler exited $rc)"
    failures=$((failures+1))
    continue
  fi

  if [ ! -f "$od/pack.mcmeta" ]; then
    echo "FAIL: $name missing pack.mcmeta"
    failures=$((failures+1))
    continue
  fi

  if ! find "$od/data" -type f -name '*.mcfunction' | grep -q .; then
    echo "FAIL: $name produced no mcfunction files"
    failures=$((failures+1))
    continue
  fi

  if [ "$name" = "dep_normal" ] && [ -d "$od/data/example_lib" ]; then
    echo "FAIL: $name should not emit a data/example_lib folder (dependency wasn't embedded)"
    failures=$((failures+1))
    continue
  fi

  if [ "$name" = "dep_embed" ] && [ ! -d "$od/data/example_lib" ]; then
    echo "FAIL: $name should emit a data/example_lib folder (dependency was embedded)"
    failures=$((failures+1))
    continue
  fi

  if [ "$name" = "func_ref_dep" ]; then
    if ! grep -rq "internal_call_ref_int" "$od/data"; then
      echo "FAIL: $name did not emit the indirect-call helper (internal_call_ref_int)"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq '"example_lib:add"' "$od/data"; then
      echo "FAIL: $name did not capture example_lib:add as the reference target (cross-package function reference)"
      failures=$((failures+1))
      continue
    fi
  fi

  echo "PASS: $name"
done

if [ $failures -ne 0 ]; then
  echo "$failures tests failed"
  exit 1
fi

echo "All tests passed"
exit 0
