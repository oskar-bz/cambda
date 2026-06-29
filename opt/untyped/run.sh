#!/usr/bin/env bash
# run.sh EVAL BODYFILE  -- prepend prelude, evaluate, print result
set -e
EVAL="$1"; BODY="$2"
cat prelude.lam "$BODY" | "$EVAL"
