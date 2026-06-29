-- examples.tl — run with:  typed.exe examples.tl

-- Polymorphic identity: inferred, no annotations.
let id = \x. x
let const = \x y. x

-- let-polymorphism: id is used at two different types in one expression.
let selfapp = let f = \x. x in f f

-- Church-style higher-order: twice applies a function two times.
let twice = \f x. f (f x)
twice (\n. n + 1) 0

-- Recursion is Turing-complete (factorial).
let rec fact = \n. if n == 0 then 1 else n * fact (n - 1)
fact 10

-- Mutual-ish recursion via one recursive function (Fibonacci).
let rec fib = \n. if n < 2 then n else fib (n - 1) + fib (n - 2)
fib 20

-- Optional type annotation; checked against the inferred type.
let inc = \x:Int. x + 1
inc 41

-- Comparisons return Bool.
fact 5 == 120

-- A polymorphic combinator applied at Int then Bool.
let apply = \f x. f x
apply (\b. if b then 1 else 0) true
