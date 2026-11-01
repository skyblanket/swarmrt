#!/usr/bin/env bash
# Windows cross-compile check (#51). Run from the repo root: `make win-xcheck`.
#
#  1. Compiles every translation unit of libswarmrt, swc and sws with
#     MinGW-w64. Units in OK_UNITS must compile. Units in BLOCKED_UNITS are
#     known not to compile yet (the reason is printed); if one starts
#     compiling, the script says so and it should move to OK_UNITS.
#  2. Links sws.exe (search CLI). If wine is installed, runs it: index a
#     temp dir, search it, and check the hit.
#
# Needs x86_64-w64-mingw32-gcc and MinGW zlib (Ubuntu: gcc-mingw-w64-x86-64
# libz-mingw-w64-dev; wine64 for step 2). Without the compiler this prints
# SKIP and exits 0, unless WIN_XCHECK_REQUIRED=1 (CI) makes that a failure.
set -u
cd "$(dirname "$0")/.."

CC=""
for c in x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc; do
    if command -v "$c" >/dev/null 2>&1; then CC="$c"; break; fi
done
if [ -z "$CC" ]; then
    if [ "${WIN_XCHECK_REQUIRED:-0}" = 1 ]; then
        echo "win-xcheck: FAIL: x86_64-w64-mingw32-gcc not found"; exit 1
    fi
    echo "win-xcheck: SKIP (x86_64-w64-mingw32-gcc not installed)"; exit 0
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CFLAGS="-O1 -Isrc -DSWARMRT_VERSION=\"xcheck\" -DSWARMRT_GITDESC=\"xcheck\""

OK_UNITS="swarmrt_asm.S swarmrt_otp.c swarmrt_task.c swarmrt_ets.c swarmrt_phase4.c
  swarmrt_phase5.c swarmrt_hotload.c swarmrt_io.c swarmrt_gc.c swarmrt_pdf.c
  swarmrt_varena.c swc.c swarmrt_codegen.c swarmrt_obfusc.c swarmrt_repl.c
  swarmrt_test.c swarmrt_lsp.c swarmrt_battery_interp.c swarmrt_new.c swc_pkg.c
  swc_imports.c swarmrt_desktop.c swarmrt_clipboard.c swarmrt_fsindex.c
  swarmrt_search.c"

# unit|reason (first blockers; see docs/notes/PLATFORM_PARITY.md)
BLOCKED_UNITS="
swarmrt_native.c|POSIX signals (sigaction, sigaltstack, siginfo_t, stack_t), execinfo.h backtrace, sysconf, usleep
swarmrt_node.c|BSD socket headers (arpa/inet.h) and OpenSSL, which has no MinGW package here
swarmrt_lang.c|sys/wait.h and fork/waitpid process control; SQLite headers, which have no MinGW package here
swarmrt_http.c|OpenSSL (SHA1 and TLS), which has no MinGW package here
"

compile() {   # unit -> 0 if it compiles
    local u="$1" extra=""
    [ "$u" = swarmrt_search.c ] || [ "$u" = swarmrt_fsindex.c ] && extra="-march=x86-64-v2"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $extra -c "src/$u" -o "$OUT/${u%.*}.o" 2> "$OUT/${u%.*}.err"
}

fail=0
echo "win-xcheck: compiling with $CC"
for u in $OK_UNITS; do
    if compile "$u"; then
        echo "  ok       $u"
    else
        echo "  FAIL     $u"
        grep -m5 -E "error" "$OUT/${u%.*}.err" | sed 's/^/           /'
        fail=1
    fi
done
while IFS='|' read -r u why; do
    [ -z "$u" ] && continue
    if compile "$u"; then
        echo "  NOW OK   $u (was blocked: move it to OK_UNITS in scripts/win_xcheck.sh)"
    else
        echo "  blocked  $u: $why"
    fi
done <<< "$BLOCKED_UNITS"

# Step 2: sws.exe end to end.
if "$CC" -O2 -march=x86-64-v2 -Isrc src/swarmrt_fsindex.c src/swarmrt_search.c \
        -o "$OUT/sws.exe" -lz -lpthread -static 2> "$OUT/sws.err"; then
    echo "  ok       link sws.exe"
    WINE=""
    for w in wine64 wine /usr/lib/wine/wine64; do
        if command -v "$w" >/dev/null 2>&1; then WINE="$w"; break; fi
    done
    if [ -n "$WINE" ]; then
        mkdir -p "$OUT/run/data/sub"
        echo "hello swarm xcheckneedle" > "$OUT/run/data/sub/a.txt"
        echo "other" > "$OUT/run/data/b.md"
        export WINEPREFIX="$OUT/wineprefix" WINEDEBUG=-all
        res="$(cd "$OUT/run" && timeout 180 "$WINE" "$OUT/sws.exe" index data 2>&1 &&
               timeout 60 "$WINE" "$OUT/sws.exe" search xcheckneedle 2>&1)"
        if printf '%s' "$res" | grep -q "data/sub/a.txt"; then
            echo "  ok       sws.exe index + search under $WINE"
        else
            echo "  FAIL     sws.exe under $WINE:"; printf '%s\n' "$res" | tail -8 | sed 's/^/           /'
            fail=1
        fi
    else
        echo "  skip     running sws.exe (wine not installed)"
    fi
else
    echo "  FAIL     link sws.exe"; grep -m5 -E "error|undefined" "$OUT/sws.err" | sed 's/^/           /'
    fail=1
fi

if [ "$fail" = 0 ]; then echo "win-xcheck: OK"; else echo "win-xcheck: FAIL"; fi
exit "$fail"
