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

  if [ "$name" = "test_float_math" ]; then
    if ! grep -rq "set compute default float" "$od/data"; then
      echo "FAIL: $name did not emit any collapsed float number-provider commands"
      failures=$((failures+1))
      continue
    fi
    if grep -rq "item modify block\|set_custom_model_data\|transformation\|_temp_div\|_temp_trans" "$od/data"; then
      echo "FAIL: $name still emits the old entity-transformation-matrix/item-modify float math hacks"
      failures=$((failures+1))
      continue
    fi
  fi

  if [ "$name" = "test_classes" ]; then
    if ! grep -rq "__vtbl_speak" "$od/data"; then
      echo "FAIL: $name did not emit a vtable field for the virtual method"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "internal_call_ref" "$od/data"; then
      echo "FAIL: $name did not emit an indirect call for the virtual method dispatch"
      failures=$((failures+1))
      continue
    fi
  fi

  if [ "$name" = "test_operator_overload" ]; then
    if ! grep -rq "operator_add" "$od/data"; then
      echo "FAIL: $name did not emit an operator+ overload function"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "__vtbl_operator_add" "$od/data"; then
      echo "FAIL: $name did not emit a vtable field for the virtual operator overload"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "internal_call_ref" "$od/data"; then
      echo "FAIL: $name did not emit an indirect call for the virtual operator dispatch"
      failures=$((failures+1))
      continue
    fi
  fi

  if [ "$name" = "test_generics" ]; then
    if ! grep -rq "identity<int>" "$od/data"; then
      echo "FAIL: $name did not emit a monomorphized identity<int> instantiation"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "identity<string>" "$od/data"; then
      echo "FAIL: $name did not emit a separate monomorphized identity<string> instantiation"
      failures=$((failures+1))
      continue
    fi
    box_ctor_count=$(find "$od/data" -name "Box_*.mcfunction" | wc -l)
    if [ "$box_ctor_count" -lt 2 ]; then
      echo "FAIL: $name did not emit distinct Box<int>/Box<string> constructor instantiations (found $box_ctor_count)"
      failures=$((failures+1))
      continue
    fi
  fi

  if [ "$name" = "test_data_access" ]; then
    if ! grep -rq "set from entity @s Health" "$od/data"; then
      echo "FAIL: $name did not emit a direct entity NBT read"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "run data get block ~ ~-1 ~ Level" "$od/data"; then
      echo "FAIL: $name did not emit a direct block NBT read via 'as int'"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "data modify storage loom_test:scratch some.path set value 5" "$od/data"; then
      echo "FAIL: $name did not emit a direct storage NBT write"
      failures=$((failures+1))
      continue
    fi
  fi

  if [ "$name" = "test_type_alias" ]; then
    if ! grep -rq "internal_call_ref_int" "$od/data"; then
      echo "FAIL: $name did not emit the indirect-call helper for the function-type alias call"
      failures=$((failures+1))
      continue
    fi
  fi

  if [ "$name" = "test_force_cast" ]; then
    if ! grep -rq "internal_call_ref_int" "$od/data"; then
      echo "FAIL: $name did not emit the indirect-call helper for the force-cast-to-function-type call"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq 'set value "loom_test:internal/add"' "$od/data"; then
      echo "FAIL: $name did not compile the force-cast string literal through unchanged"
      failures=$((failures+1))
      continue
    fi
  fi

  if [ "$name" = "test_number_provider" ]; then
    if ! grep -rq "run compute default integer" "$od/data"; then
      echo "FAIL: $name did not emit a collapsed integer number-provider /compute"
      failures=$((failures+1))
      continue
    fi
    if ! grep -rq "set compute default float" "$od/data"; then
      echo "FAIL: $name did not emit a collapsed float number-provider /data modify ... compute"
      failures=$((failures+1))
      continue
    fi
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

  if [ "$name" = "dep_normal" ]; then
    if ! grep -rq '"example_lib:add' "$od/data"; then
      echo "FAIL: $name did not resolve the dependency's extern type alias (example_lib::IntOp) to a working function reference"
      failures=$((failures+1))
      continue
    fi
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
