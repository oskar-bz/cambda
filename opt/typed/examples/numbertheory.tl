-- numbertheory.tl — integer algorithms on native Int + let rec.
--
-- Note: this language has + - * and comparisons, but NO division/modulo
-- operator, so we build integer division and remainder from subtraction.
-- Everything here is ordinary recursion over Int, which HM types with no
-- trouble (it is the language's comfort zone).
--
-- Run:  typed.exe examples/numbertheory.tl

let rec idiv = \a b. if a < b then 0 else 1 + idiv (a - b) b      -- a `div` b  (a,b >= 0)
let rec imod = \a b. if a < b then a else imod (a - b) b          -- a `mod` b

-- greatest common divisor (Euclid) and least common multiple
let rec gcd = \a b. if b == 0 then a else gcd b (imod a b)
let lcm = \a b. (idiv a (gcd a b)) * b

-- primality by trial division up to sqrt (d*d > n)
let rec hasDivFrom = \n d. if d * d > n then false else if imod n d == 0 then true else hasDivFrom n (d + 1)
let isPrime = \n. if n < 2 then false else if hasDivFrom n 2 then false else true

-- count primes below a bound, and the n-th prime (1-indexed)
let rec primesBelow = \n hi. if n >= hi then 0 else (if isPrime n then 1 else 0) + primesBelow (n + 1) hi
let rec nthPrimeFrom = \k cand. if isPrime cand then (if k == 1 then cand else nthPrimeFrom (k - 1) (cand + 1)) else nthPrimeFrom k (cand + 1)
let nthPrime = \k. nthPrimeFrom k 2

-- integer square root (floor)
let rec isqrtFrom = \n i. if i * i > n then i - 1 else isqrtFrom n (i + 1)
let isqrt = \n. isqrtFrom n 0

-- Collatz: number of steps to reach 1
let rec collatz = \n. if n == 1 then 0 else if imod n 2 == 0 then 1 + collatz (idiv n 2) else 1 + collatz (3 * n + 1)

-- Ackermann-Péter (small arguments only — it grows astronomically)
let rec ack = \m n. if m == 0 then n + 1 else if n == 0 then ack (m - 1) 1 else ack (m - 1) (ack m (n - 1))

-- digit utilities: digit sum, reverse, palindrome test
let rec sumDigits = \n. if n == 0 then 0 else imod n 10 + sumDigits (idiv n 10)
let rec revFrom = \n acc. if n == 0 then acc else revFrom (idiv n 10) (acc * 10 + imod n 10)
let reverseNum = \n. revFrom n 0
let isPalindrome = \n. reverseNum n == n

-- fast modular exponentiation (square-and-multiply): base^exp mod m
let rec modpow = \b e m. if e == 0 then 1 else (let h = modpow b (idiv e 2) m in (let sq = imod (h * h) m in if imod e 2 == 0 then sq else imod (sq * imod b m) m))

-- ---- demonstrations (value : type) ------------------------------------
gcd 1071 462          -- 21
lcm 4 6               -- 12
isPrime 97            -- true
isPrime 91            -- false  (7 * 13)
primesBelow 2 100     -- 25  (primes under 100)
nthPrime 10           -- 29
isqrt 144             -- 12
isqrt 150             -- 12
collatz 27            -- 111
ack 2 3              -- 9
ack 3 3              -- 61
sumDigits 123456     -- 21
reverseNum 1230      -- 321
isPalindrome 12321   -- true
modpow 7 128 13      -- 3   (7^128 mod 13)
