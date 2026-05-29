#!/bin/bash
# Build + show only errors / FAILED lines. Use this when a build is
# failing and you want to read ALL errors at once (per frugality
# directive #4: batch fixes -- one edit pass per all errors, then
# rebuild).
"$(dirname "$0")/b.sh" 2>&1 | grep -E "error|FAILED" | head -25
