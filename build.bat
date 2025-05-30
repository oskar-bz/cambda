@echo off
set files=main.c cambda.c ..\lib\ol.c
clang %files% -fsanitize=address --debug -gfull -g3 -glldb -o out/main.exe
@echo on
