#!/bin/bash
# sanitizer_check.sh — Check if a process exited cleanly (no sanitizer errors)
#
# Usage: source sanitizer_check.sh
#        stop_and_check_sanitizer PID "description"
#
# Sends SIGTERM, waits, checks exit code. ASan/UBSan cause non-zero exit.
# Exit code 1 = sanitizer error detected.

stop_and_check_sanitizer() {
    local pid="$1"
    local desc="${2:-process}"
    local rc=0

    if [ -z "$pid" ]; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
        # Already dead — check if it died from sanitizer
        wait "$pid" 2>/dev/null
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "SANITIZER FAIL: $desc (PID $pid) exited with code $rc"
            return 1
        fi
        return 0
    fi

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null
    rc=$?

    # SIGTERM causes exit code 143 (128+15) which is normal
    if [ $rc -ne 0 ] && [ $rc -ne 143 ]; then
        echo "SANITIZER FAIL: $desc (PID $pid) exited with code $rc"
        return 1
    fi
    return 0
}
