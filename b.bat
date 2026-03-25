@echo off
clang main.c -gfull -fsanitize=address -o m.exe -lreplxx-static
@echo on
