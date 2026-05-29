#!/bin/bash
# Run rsm2pdb_tests, tail to summary lines.
# Usage:   scripts/t.sh           -> last 3 lines (pass/fail summary)
#          scripts/t.sh full      -> full output
#          scripts/t.sh -tc=X     -> pass-through to doctest (filter etc.)
case "${1:-}" in
  full) /c/Dev/Src/rsm2pdb/build/test/rsm2pdb_tests.exe ;;
  ""|-*) /c/Dev/Src/rsm2pdb/build/test/rsm2pdb_tests.exe "$@" 2>&1 | tail -3 ;;
  *)    /c/Dev/Src/rsm2pdb/build/test/rsm2pdb_tests.exe "$@" ;;
esac
