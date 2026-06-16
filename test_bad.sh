#!/bin/bash
# Negative tests: every program in examples/bad/ MUST be rejected by the
# compiler (semantic analysis or codegen), producing no output. Reports the
# first error line for each, and fails loudly if any bad program is accepted.
set +e
pass=0
fail=0
for f in examples/bad/*.sim; do
    name=$(basename "$f")
    out=$(./build/esimc "$f" -o /tmp/bad_check.ll 2>&1)
    rc=$?
    err=$(echo "$out" | grep -m1 -E '^Error')
    if [ $rc -ne 0 ]; then
        echo "ok   $name -> ${err:-(rejected)}"
        pass=$((pass+1))
    else
        echo "FAIL $name -> ACCEPTED (should have been rejected)"
        fail=$((fail+1))
    fi
done
echo "----"
echo "rejected: $pass   wrongly accepted: $fail"
[ $fail -eq 0 ]
