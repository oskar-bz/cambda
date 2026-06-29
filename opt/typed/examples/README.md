# Example programs

Run any with `typed.exe examples/<name>.tl`.

| file | what it shows |
|---|---|
| `numbertheory.tl` | Integer algorithms on native `Int` + `let rec`: `div`/`mod` from subtraction, `gcd`/`lcm`, primality & n-th prime, integer `sqrt`, Collatz length, Ackermann, digit tricks, fast modular exponentiation. The language's comfort zone. |
| `lists.tl` | A Church-encoded list library (`range`, `map`, `filter`, folds). Also documents the boundary: list→list operations like sorting need first-class polymorphism HM lacks, and the checker rejects them with an occurs check. |
| `sorting.tl` | Sorting that *does* type-check: **sorting networks** (fixed min/max compare-exchange), which need no recursion or list type. Outputs are packed into one `Int` (two digits per slot). |
| `rsa.tl` | A real program: working **RSA** public-key crypto (textbook key), deriving the private exponent and showing `decrypt(encrypt(m)) = m`. Pure integer arithmetic. |

### Why no binary search tree?

A recursive tree (or a Scott-encoded list with structural recursion) has a type
that refers to itself — `T = ... T ...` — which Hindley–Milner rejects via the
occurs check, because it has no equirecursive/iso-recursive types. That is a
real property of the type system, not a gap in the interpreter; it is exactly
why `sorting.tl` uses a network and `lists.tl` sticks to single-pass folds.
Lifting it would need recursive types or a System-F core.
