-- lists.tl — a Church-encoded list library, and the boundary of what HM allows.
--
-- A list is its own right-fold:  [x,y,z]  =  \c n. c x (c y (c z n)).
-- Operations that CONSUME a list to a scalar (sum, length, any, ...) or that
-- transform it in a single pass (map, filter) type fine in Hindley-Milner.
--
-- What does NOT work: list -> list operations defined by folding a *list-valued
-- accumulator* (insertion sort, nub, reverse-by-insert). They need the list
-- type to be used polymorphically inside its own fold, which is first-class
-- polymorphism HM lacks — the checker rejects them with an occurs check
-- ("infinite type"). Sorting therefore lives in sorting.tl, done a different way.
--
-- Run:  typed.exe examples/lists.tl

let nil  = \c n. n
let cons = \h t. \c n. c h (t c n)

-- build the list [lo, lo+1, ..., hi]
let rec range = \lo hi. if lo > hi then nil else cons lo (range (lo + 1) hi)

-- single-pass transformers (return a fresh \c n. list)
let map    = \f l. \c n. l (\h acc. c (f h) acc) n
let filter = \p l. \c n. l (\h acc. if p h then c h acc else acc) n

-- folds to a scalar
let sum   = \l. l (\h a. h + a) 0
let len   = \l. l (\h a. 1 + a) 0
let prod  = \l. l (\h a. h * a) 1
let maxOf = \l. l (\h a. if h > a then h else a) 0
let count = \p l. l (\h a. if p h then 1 + a else a) 0
let any   = \p l. l (\h a. if p h then true else a) false
let all   = \p l. l (\h a. if p h then a else false) true
let member = \x l. l (\h a. if h == x then true else a) false

-- a predicate to play with: primality (trial division, no '/' operator)
let rec imod = \a b. if a < b then a else imod (a - b) b
let rec hasDiv = \n d. if d * d > n then false else if imod n d == 0 then true else hasDiv n (d + 1)
let isPrime = \n. if n < 2 then false else if hasDiv n 2 then false else true

-- ---- demonstrations ----------------------------------------------------
sum (range 1 100)                            -- 5050
len (range 1 100)                            -- 100
prod (range 1 6)                             -- 720  (6!)
maxOf (cons 3 (cons 9 (cons 4 nil)))         -- 9
count isPrime (range 1 100)                  -- 25   (primes below/at 100)
member 42 (range 1 100)                      -- true
any isPrime (range 90 96)                    -- false (90..96 has none)
sum (map (\x. x * x) (filter isPrime (range 1 30)))   -- 2397 (sum of squared primes < 30)
