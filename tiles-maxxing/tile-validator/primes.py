"""Gaussian primality testing.

A Gaussian integer a+bi is a Gaussian prime iff:
  - a,b both nonzero: a^2 + b^2 is a rational prime
  - a=0: |b| is a rational prime AND |b| ≡ 3 (mod 4)
  - b=0: |a| is a rational prime AND |a| ≡ 3 (mod 4)

The validator prefers external libraries when available, but also carries a
deterministic Miller-Rabin fallback so the spec-faithful pipeline can run in a
plain Python environment.
"""

TRIAL_DIVISORS = (
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29,
    31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
    73, 79, 83, 89, 97,
)


def _is_rational_prime_mr(n: int) -> bool:
    if n < 2:
        return False
    for p in TRIAL_DIVISORS:
        if n == p:
            return True
        if n % p == 0:
            return False

    d = n - 1
    s = 0
    while d % 2 == 0:
        d //= 2
        s += 1

    if n < 25_326_001:
        witnesses = (2, 3, 5)
    elif n < 3_215_031_751:
        witnesses = (2, 3, 5, 7)
    else:
        witnesses = (2, 3, 5, 7, 11)

    for a in witnesses:
        if a >= n:
            continue
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        witness_passed = False
        for _ in range(s - 1):
            x = (x * x) % n
            if x == n - 1:
                witness_passed = True
                break
        if not witness_passed:
            return False
    return True


try:
    from gmpy2 import is_prime as _is_rational_prime, mpz

    def is_rational_prime(n: int) -> bool:
        return bool(_is_rational_prime(mpz(n)))

    BACKEND = "gmpy2"
except ImportError:
    try:
        from sympy import isprime as is_rational_prime  # type: ignore

        BACKEND = "sympy"
    except ImportError:
        is_rational_prime = _is_rational_prime_mr
        BACKEND = "deterministic-mr"


def is_gaussian_prime(a: int, b: int) -> bool:
    """Test if a+bi is a Gaussian prime."""
    if a == 0 and b == 0:
        return False
    if a == 0:
        ab = abs(b)
        return ab % 4 == 3 and is_rational_prime(ab)
    if b == 0:
        ab = abs(a)
        return ab % 4 == 3 and is_rational_prime(ab)
    return is_rational_prime(a * a + b * b)
