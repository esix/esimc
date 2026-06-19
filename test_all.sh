#!/bin/bash
# Golden-output test harness.
#
# For each examples/*.sim: compile -> llc -> link, run it with the matching
# examples/input/<name>.in as stdin (empty if none), and compare the FULL
# output to examples/expected/<name>.out.
#
# A few examples are interactive and loop on EOF (no stable full output); they
# are listed in LOOSE and checked only for a matching first output line.
#
#   ./test_all.sh           run all checks (exit 1 if any fail)
#   ./test_all.sh --bless   regenerate expected outputs from current behavior
#
# `timeout` isn't available on macOS, so a sleep+kill watchdog is used.

set +e
LOOSE="15_puzzle_game 24_game"
STRICT_TIMEOUT=20      # generous: Anagrams scans the 25k-word dictionary
LOOSE_TIMEOUT=2
EXP=examples/expected
bless=0; [ "$1" = "--bless" ] && bless=1

is_loose() { case " $LOOSE " in *" $1 "*) return 0;; *) return 1;; esac; }

# run_one <simfile> <name> <timeout> -> full combined output on stdout
run_one() {
    local f=$1 name=$2 to=$3
    ./build/esimc "$f" -o /tmp/g_$name.ll >/dev/null 2>&1 || { echo "__COMPILE_FAIL__"; return; }
    llc -filetype=obj /tmp/g_$name.ll -o /tmp/g_$name.o 2>/dev/null || { echo "__LLC_FAIL__"; return; }
    clang /tmp/g_$name.o build/simula_rt.o -o /tmp/g_$name.exe -lm 2>/dev/null || { echo "__LINK_FAIL__"; return; }
    local instream=/dev/null
    [ -f "examples/input/$name.in" ] && instream="examples/input/$name.in"
    # The watchdog must NOT inherit the captured stdout fd, or command
    # substitution blocks for the full timeout on every example.
    /tmp/g_$name.exe < "$instream" 2>&1 & P=$!
    ( sleep "$to"; kill -9 $P 2>/dev/null ) >/dev/null 2>&1 & W=$!
    wait $P 2>/dev/null
    kill $W 2>/dev/null; wait $W 2>/dev/null
}

pass=0; fail=0; failed=""
for f in examples/*.sim; do
    name=$(basename "$f" .sim)
    if is_loose "$name"; then
        out=$(run_one "$f" "$name" "$LOOSE_TIMEOUT")
        got=$(printf '%s\n' "$out" | head -1)
        if [ $bless -eq 1 ]; then printf '%s\n' "$got" > "$EXP/$name.firstline"; echo "blessed $name (loose)"; continue; fi
        want=$(cat "$EXP/$name.firstline" 2>/dev/null)
        if [ "$got" = "$want" ]; then echo "ok    $name (loose)"; pass=$((pass+1));
        else echo "FAIL  $name (loose): [$got] != [$want]"; fail=$((fail+1)); failed="$failed $name"; fi
        continue
    fi
    out=$(run_one "$f" "$name" "$STRICT_TIMEOUT")
    if [ $bless -eq 1 ]; then printf '%s' "$out" > "$EXP/$name.out"; echo "blessed $name"; continue; fi
    if printf '%s' "$out" | diff -q - "$EXP/$name.out" >/dev/null 2>&1; then
        echo "ok    $name"; pass=$((pass+1))
    else
        echo "FAIL  $name"; fail=$((fail+1)); failed="$failed $name"
    fi
done

echo "----"
if [ $bless -eq 1 ]; then echo "blessed all expected outputs"; exit 0; fi
echo "passed: $pass   failed: $fail"
[ -n "$failed" ] && echo "failing:$failed"
[ $fail -eq 0 ]
