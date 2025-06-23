@echo off
set files=main.c cambda.c lib\ol.c
set flags=-fsanitize=address --debug -gfull -g3 -glldb -Wno-macro-redefined
clang %files% %flags% -o out/main.exe
@echo on
