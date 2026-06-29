-- sorting.tl — sorting that works in HM-typed pure lambda calculus.
--
-- We cannot fold a list into a sorted list (see lists.tl: it needs first-class
-- polymorphism HM lacks). But a *sorting network* — a fixed sequence of
-- compare-exchange (min/max) steps — needs no recursion and no list type at
-- all, so it types perfectly. Sorting networks are real hardware/SIMD
-- algorithms. We pack the sorted outputs into one Int (two decimal digits per
-- slot) so the result is readable.
--
-- Run:  typed.exe examples/sorting.tl

let min = \a b. if a < b then a else b
let max = \a b. if a < b then b else a

-- 3-input network (3 comparators), output a<=b<=c packed as aabbcc
let sort3 = \a b c. (let x = max a b in (let lo = min (min a b) c in (let hi = max x c in (let mid = (a + b + c) - lo - hi in lo * 10000 + mid * 100 + hi))))

-- 4-input network (5 comparators: (0,1)(2,3)(0,2)(1,3)(1,2)), packed aabbccdd
let sort4 = \a b c d. (let a1 = min a b in (let b1 = max a b in (let c1 = min c d in (let d1 = max c d in (let lo = min a1 c1 in (let m1 = max a1 c1 in (let hi = max b1 d1 in (let m2 = min b1 d1 in (let p1 = min m1 m2 in (let p2 = max m1 m2 in lo * 1000000 + p1 * 10000 + p2 * 100 + hi))))))))))

-- median of three, and clamp(value, lo, hi) — handy primitives built on min/max
let median3 = \a b c. max (min a b) (min (max a b) c)
let clamp = \lo hi x. max lo (min hi x)

-- ---- demonstrations ----------------------------------------------------
sort3 3 1 2          -- 10203   (1, 02, 03)
sort3 9 9 1          -- 10909   (1, 09, 09)
sort3 5 4 7          -- 40507   (4, 05, 07)
sort4 4 2 3 1        -- 1020304 (1,02,03,04)
sort4 9 1 5 5        -- 1050509 (1,05,05,09)
median3 7 2 9        -- 7
clamp 0 100 250      -- 100
clamp 0 100 (0 - 5)  -- 0
