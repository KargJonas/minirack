#!/usr/bin/env bash
# Upload an app image over HTTP and verify the device actually runs it.
#
#   usage: flash.sh [host] [firmware.bin] [timeout_s]
#
# Without a host, the board is discovered via mDNS (_easyota._tcp; needs
# avahi-browse). Without a .bin, the platformio environment is chosen
# interactively (enter = default_envs) and built via pio - or, when an
# image already exists, optionally re-flashed as-is. So a plain
# ./flash.sh builds, finds the board, and uploads.
#
# exit 0: the uploaded build is running (validated identity via its embedded
#         per-build SHA), 1: device came back with a different image (rollback),
#         2: device never came back, 3: bad arguments/image.
set -euo pipefail

host=${1:-}
bin=${2:-}
timeout=${3:-90}

# With host omitted the other args shift left: a purely numeric arg is the
# timeout, a *.bin (or existing file path) is the image.
if [[ "$host" =~ ^[0-9]+$ ]]; then
    timeout=$host
    host=
elif [[ "$host" == *.bin || ( "$host" == */* && -f "$host" ) ]]; then
    [ -n "$bin" ] && timeout=$bin
    bin=$host
    host=
fi
if [[ "$bin" =~ ^[0-9]+$ ]]; then
    timeout=$bin
    bin=
fi

# No .bin given: pick a platformio environment and (re)build it. Never guess
# between environments: a wt32-eth01 image uploaded onto the lolin32 dev
# board bricks its way into a rollback.
if [ -z "$bin" ]; then
    dir=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
    command -v pio >/dev/null || {
        echo "error: pio not found - build yourself and pass a .bin" >&2
        exit 3
    }
    mapfile -t envs < <(sed -n 's/^\[env:\([^]]*\)\].*/\1/p' "$dir/platformio.ini")
    if [ ${#envs[@]} -eq 0 ]; then
        echo "error: no [env:...] sections in $dir/platformio.ini" >&2
        exit 3
    elif [ ${#envs[@]} -eq 1 ]; then
        env=${envs[0]}
    else
        defenv=$(sed -n 's/^default_envs *= *//p' "$dir/platformio.ini" | head -1)
        echo "environments in platformio.ini:" >&2
        defidx=1
        for i in "${!envs[@]}"; do
            [ "${envs[$i]}" = "$defenv" ] && defidx=$((i + 1))
            printf '  [%d] %s%s\n' "$((i + 1))" "${envs[$i]}" \
                   "$([ "${envs[$i]}" = "$defenv" ] && echo ' (default)')" >&2
        done
        read -rp "build which environment? [1-${#envs[@]}, enter=$defidx] " n </dev/tty || exit 3
        n=${n:-$defidx}
        [[ "$n" =~ ^[0-9]+$ ]] && (( n >= 1 && n <= ${#envs[@]} )) || {
            echo "error: invalid selection" >&2
            exit 3
        }
        env=${envs[$((n - 1))]}
    fi
    bin=$dir/.pio/build/$env/firmware.bin
    build=1
    if [ -f "$bin" ]; then
        printf '  [1] rebuild %s\n' "$env" >&2
        printf '  [2] use the existing image (built %s)\n' "$(date -r "$bin" '+%F %T')" >&2
        read -rp "which image? [1-2, enter=1] " a </dev/tty || exit 3
        case ${a:-1} in
        1) ;;
        2) build=0 ;;
        *)
            echo "error: invalid selection" >&2
            exit 3
            ;;
        esac
    fi
    if (( build )); then
        echo "building $env..."
        # progress to /dev/null; compiler errors come via stderr and stay visible
        (cd "$dir" && pio run -e "$env" >/dev/null) || {
            echo "error: build failed - for the full log: pio run -e $env" >&2
            exit 3
        }
    fi
    echo "using build: $bin ($(date -r "$bin" '+%F %T'))"
fi

if [ -z "$host" ]; then
    command -v avahi-browse >/dev/null || {
        echo "error: mDNS discovery needs avahi-browse (avahi-utils) - or pass <host>" >&2
        exit 3
    }
    # Parseable resolved records: =;if;proto;name;type;domain;host;addr;port;txt
    # One line per address (devices show up once per interface), IPv4 only.
    mapfile -t found < <(avahi-browse -rtp _easyota._tcp 2>/dev/null \
        | awk -F';' '$1=="=" && $3=="IPv4" && !seen[$8]++ {print $8";"$7";"$10}')
    if [ ${#found[@]} -eq 0 ]; then
        echo "error: no EasyOTA device found via mDNS (is the board up and avahi-daemon running?)" >&2
        exit 3
    fi
    for i in "${!found[@]}"; do
        IFS=';' read -r addr mdnshost txt <<<"${found[$i]}"
        printf '  [%d] %-15s %s  %s\n' "$((i + 1))" "$addr" "$mdnshost" "$txt" >&2
    done
    # Always ask, even for a single device: silently uploading to "the one
    # board that answered" is wrong when a second expected board just
    # dropped off the network.
    defsel=
    [ ${#found[@]} -eq 1 ] && defsel=", enter=1"
    read -rp "flash which device? [1-${#found[@]}$defsel] " n </dev/tty || exit 3
    [ ${#found[@]} -eq 1 ] && n=${n:-1}
    [[ "$n" =~ ^[0-9]+$ ]] && (( n >= 1 && n <= ${#found[@]} )) || {
        echo "error: invalid selection" >&2
        exit 3
    }
    host=${found[$((n - 1))]%%;*}
fi

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
    tl=$(field "$st" crash_timeline)
    [ -n "$tl" ] && echo "  crashed boot's events: $tl" >&2
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
# explicit content type: curl's default (x-www-form-urlencoded) would make the
# async server parse the image as a form. The device flashes each chunk
# before ACKing more (TCP backpressure), so curl's upload bar tracks the
# actual flash progress - but curl mutes the bar if the response body goes
# to the terminal, hence the capture-then-echo.
resp=$(curl -f --progress-bar -H 'Content-Type: application/octet-stream' \
       --data-binary @"$bin" "http://$host/update")
echo "$resp"

start=$SECONDS
printf 'waiting for the board to come back ' >&2
sleep 3
while (( SECONDS - start < timeout )); do
    if status=$(get status); then
        elapsed=$((SECONDS - start))
        sha=$(field "$status" elf_sha256)
        part=$(field "$status" partition)
        if [ "$sha" = "$want_sha" ]; then
            printf '\n' >&2
            echo "OK: new image is running from $part (after ${elapsed}s)"
            exit 0
        fi
        # A different image answered. Freshly rebooted -> it's the rollback
        # fallback; long uptime -> the old app hasn't rebooted yet, keep waiting.
        up=$(grep -o '"uptime_s":[0-9]*' <<<"$status" | cut -d: -f2)
        if [ -n "$up" ] && (( up < elapsed + 3 )); then
            printf '\n' >&2
            echo "FAILED: device rolled back to previous image (\"$(field "$status" info)\" in $part)" >&2
            diag "$status"
            exit 1
        fi
    fi
    printf '.' >&2
    sleep 1
done
printf '\n' >&2
echo "FAILED: device did not come back within ${timeout}s" >&2
exit 2
