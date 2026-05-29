#!/bin/bash
# Run PDB pipeline on a fixture and tail key metrics.
# Usage:
#   scripts/smoke.sh <fixture>     -- one fixture, tail 8
#   scripts/smoke.sh all           -- regression sweep (all fixtures,
#                                     filter to modules/globals/wrote)
# fixture short-names map to examples/0N_*/Win64/Debug/:
#   locals, types, iface, records, inherit_props, cross_unit
set -e
ROOT=/c/Dev/Src/rsm2pdb
cd "$ROOT"

resolve() {
  case "$1" in
    locals)         echo "examples/04_locals/Win64/Debug locals" ;;
    types)          echo "examples/05_types/Win64/Debug types" ;;
    iface)          echo "examples/06_interface/Win64/Debug iface" ;;
    records)        echo "examples/07_records/Win64/Debug records" ;;
    inherit_props)  echo "examples/08_inherit_props/Win64/Debug inherit_props" ;;
    cross_unit)     echo "examples/09_cross_unit/Win64/Debug cross_unit" ;;
    *) echo "" ;;
  esac
}

if [ "$1" = "all" ]; then
  for fix in locals types iface records inherit_props cross_unit; do
    pair=$(resolve "$fix")
    [ -z "$pair" ] && continue
    dir=$(echo "$pair" | cut -d' ' -f1)
    base=$(echo "$pair" | cut -d' ' -f2)
    [ -f "$dir/$base.map" ] || continue
    cp "$dir/$base.exe" "$dir/${base}_test.exe"
    echo "=== $fix ==="
    ./build/src/rsm2pdb.exe pdb "$dir/$base.map" "$dir/$base.exe" \
      "$dir/${base}_test.exe" 2>&1 \
      | grep -E "modules:|globals typed via RSM|wrote PDB|adjuster"
  done
  exit 0
fi

pair=$(resolve "$1")
if [ -z "$pair" ]; then
  echo "usage: smoke.sh <locals|types|iface|records|inherit_props|cross_unit|all>" >&2
  exit 2
fi
dir=$(echo "$pair" | cut -d' ' -f1)
base=$(echo "$pair" | cut -d' ' -f2)
cp "$dir/$base.exe" "$dir/${base}_test.exe"
./build/src/rsm2pdb.exe pdb "$dir/$base.map" "$dir/$base.exe" \
  "$dir/${base}_test.exe" 2>&1 | tail -8
