#!/bin/bash
# Test all examples non-interactively
# Usage: ./test_all.sh
#
# Each example is given empty stdin and a 2-second timeout.
# Reports the first line of output for each example.

set +e
for f in examples/*.sim; do
    name=$(basename $f)
    ./build/esimc $f -o /tmp/esimc_x.ll 2>/dev/null > /dev/null || { echo "$name: (compile failed)"; continue; }
    llc -filetype=obj /tmp/esimc_x.ll -o /tmp/esimc_x.o 2>/dev/null || { echo "$name: (llc failed)"; continue; }
    clang /tmp/esimc_x.o build/simula_rt.o -o /tmp/esimc_x -lm 2>/dev/null || { echo "$name: (link failed)"; continue; }

    output=$(echo "1" | /tmp/esimc_x 2>&1 & P=$!
        sleep 2
        if kill -0 $P 2>/dev/null; then
            kill -9 $P 2>/dev/null
            echo "(timeout)"
        fi
        wait $P 2>/dev/null)
    line1=$(echo "$output" | head -1)
    if [ -z "$line1" ]; then line1="(no output)"; fi
    echo "$name: $line1"
done
