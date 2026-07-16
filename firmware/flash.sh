#!/usr/bin/env bash
# Upload an app image over HTTP and verify the device actually runs it.
# Works against the loader or a running RackOTA app.
#
#   usage: flash.sh <host> <firmware.bin> [timeout_s]
#
# exit 0: the uploaded build is running (validated identity via its embedded
#         per-build SHA), 1: device came back with a different image (rollback),
#         2: device never came back, 3: bad arguments/image.
set -euo pipefail

host=${1:?usage: flash.sh <host> <firmware.bin> [timeout_s]}
bin=${2:?usage: flash.sh <host> <firmware.bin> [timeout_s]}
timeout=${3:-90}

# Every ESP32 app image embeds esp_app_desc_t at file offset 32 (after the
# 24-byte image header and the first 8-byte segment header): magic 0xABCD5432,
# and the toolchain's per-build ELF SHA-256 at struct offset 144 -> file 176.
# The device reports the same bytes in /status as "elf_sha256".
if [ "$(xxd -p -s 32 -l 4 "$bin")" != "3254cdab" ]; then
    echo "error: $bin does not look like an ESP32 app image" >&2
    exit 3
fi
want_sha=$(xxd -p -s 176 -l 32 "$bin" | tr -d '\n')

get()   { curl -fsS -m 3 "http://$host/$1" 2>/dev/null; }
field() { grep -o "\"$2\":\"[^\"]*\"" <<<"$1" | head -1 | cut -d'"' -f4; }

# Explain *why* the upload was rolled back, from the survivor's /status
# diagnostics: the reset reason survives the rollback reboot, the rejected
# image stays in its slot marked "aborted", and the panic handler left a
# core dump (task, PC, backtrace) in the coredump partition.
diag() {
    local st=$1 reset abslot absha celf bt
    reset=$(field "$st" last_reset)
    [ -n "$reset" ] && echo "  last reset: $reset" >&2
    abslot=$(field "$st" aborted_slot)
    absha=$(field "$st" aborted_sha)
    if [ -n "$abslot" ] && [ "$absha" = "$want_sha" ]; then
        echo "  your image booted and was rejected; it sits in $abslot marked aborted" >&2
    fi
    celf=$(field "$st" crash_elf)
    if [ -n "$celf" ] && [ "${want_sha:0:${#celf}}" = "$celf" ]; then
        bt=$(field "$st" crash_bt)
        echo "  crash: task '$(field "$st" crash_task)' at $(field "$st" crash_pc)," \
             "cause $(grep -o '"crash_cause":[0-9]*' <<<"$st" | cut -d: -f2)" >&2
        echo "  backtrace: $bt" >&2
        echo "  decode:    xtensa-esp32-elf-addr2line -e .pio/build/<env>/firmware.elf $bt" >&2
    fi
}

echo "uploading $(stat -c %s "$bin") bytes (${want_sha:0:12}...) to http://$host/update"
curl -fsS --data-binary @"$bin" "http://$host/update"

start=$SECONDS
sleep 3
while (( SECONDS - start < timeout )); do
    if status=$(get status); then
        elapsed=$((SECONDS - start))
        sha=$(field "$status" elf_sha256)
        part=$(field "$status" partition)
        if [ "$sha" = "$want_sha" ]; then
            echo "OK: new image is running from $part (after ${elapsed}s)"
            exit 0
        fi
        if [ "$(field "$status" role)" = "loader" ]; then
            echo "FAILED: device fell back to the loader — the app crashed before validating" >&2
            diag "$status"
            exit 1
        fi
        # A different app answered. Freshly rebooted -> it's the rollback
        # fallback; long uptime -> the old app hasn't rebooted yet, keep waiting.
        up=$(grep -o '"uptime_s":[0-9]*' <<<"$status" | cut -d: -f2)
        if [ -n "$up" ] && (( up < elapsed + 3 )); then
            echo "FAILED: device rolled back to previous image (\"$(field "$status" info)\" in $part)" >&2
            diag "$status"
            exit 1
        fi
    elif info=$(get ""); then
        # loaders flashed before /status existed: identify by the info page
        if grep -q "minirack loader" <<<"$info"; then
            echo "FAILED: device fell back to the loader — the app crashed before validating" >&2
            exit 1
        fi
    fi
    sleep 1
done
echo "FAILED: device did not come back within ${timeout}s" >&2
exit 2
