#!/usr/bin/env bash
# Build the typed lambda-calculus interpreter.
set -e
CC="${CC:-clang}"
$CC -O2 -Wall -o typed.exe typed.c
echo "built typed.exe"
