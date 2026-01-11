@echo off
clang++ main.cpp -gfull -o m.exe -fsanitize=address
REM clang++ main.cpp -gfull -o m.exe
@echo on
