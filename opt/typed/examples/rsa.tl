-- rsa.tl — a real program: working RSA public-key cryptography.
--
-- RSA is the math behind a lot of TLS/SSH. It is pure integer arithmetic, so a
-- typed lambda calculus can run it for real. Here we use the textbook key
-- (p=61, q=53 -> n=3233), derive the private exponent ourselves, and show that
-- decrypt(encrypt(m)) = m for several messages.
--
-- This language has no '/' or 'mod' operator, so we build integer division and
-- remainder from subtraction, then square-and-multiply modular exponentiation.
--
-- Run:  typed.exe examples/rsa.tl

let rec idiv = \a b. if a < b then 0 else 1 + idiv (a - b) b
let rec imod = \a b. if a < b then a else imod (a - b) b

-- base^exp mod m, in O(log exp) multiplications
let rec modpow = \b e m. if e == 0 then 1 else (let h = modpow b (idiv e 2) m in (let sq = imod (h * h) m in if imod e 2 == 0 then sq else imod (sq * imod b m) m))

-- modular inverse of e mod phi, by search (small phi): the private exponent d
let rec invFrom = \e phi d. if imod (e * d) phi == 1 then d else invFrom e phi (d + 1)
let modinv = \e phi. invFrom e phi 1

-- ---- key setup: public (e, n), private (d, n) --------------------------
let n = 3233                 -- = p*q, p=61 q=53
let phi = 3120               -- = (p-1)*(q-1)
let e = 17                   -- public exponent (coprime to phi)
let d = modinv e phi         -- private exponent (derived)
d                            -- 2753

let encrypt = \m. modpow m e n
let decrypt = \c. modpow c d n

-- ---- demonstrations: c = encrypt(m), and decrypt(c) recovers m ----------
encrypt 65                   -- 2790
decrypt (encrypt 65)         -- 65
decrypt (encrypt 42)         -- 42
decrypt (encrypt 123)        -- 123
decrypt (encrypt 2025)       -- 2025
encrypt 2025                 -- the ciphertext for 2025 (looks scrambled)
