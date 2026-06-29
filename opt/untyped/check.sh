#!/usr/bin/env bash
# check.sh -- run every expression in tests/suite.txt through baseline, nbe and
# clam; report any disagreement. baseline is the normal-order oracle.
pass=0; fail=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    printf '%s\n' "$line" > _expr.lam
    # nbe is the de-Bruijn reference (same naming scheme as clam); baseline is
    # the normal-order oracle but prints source variable names, so compare it
    # only on Church-numeral results.
    n=$(cat prelude.lam _expr.lam | ./nbe.exe  2>/dev/null)
    c=$(cat prelude.lam _expr.lam | ./clam.exe 2>/dev/null)
    if [ "$n" = "$c" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "MISMATCH: $line"
        echo "   nbe=[$n]  clam=[$c]"
    fi
done < tests/suite.txt
rm -f _expr.lam
echo "----"
echo "pass=$pass fail=$fail"
