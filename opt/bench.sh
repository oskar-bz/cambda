#!/usr/bin/env bash
# bench.sh [evaluator] -- run the benchmark suite, report beta/s
EVAL="${1:-./nbe.exe}"
BENCHES="collapse25 pow25 collapse10 pow20"
echo "== $EVAL =="
for b in $BENCHES; do
    printf '%-12s ' "$b"
    cat prelude.lam bench/$b.lam | "$EVAL" 2>&1 | tr '\n' ' '
    echo
done
