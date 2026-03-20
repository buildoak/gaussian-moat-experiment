"""Gaussian prime generation via sieve + vectorized lookup."""

import numpy as np
from math import isqrt


def sieve_of_eratosthenes(limit: int) -> np.ndarray:
    """Return boolean array where is_prime[n] = True iff n is prime, for n <= limit."""
    if limit < 2:
        return np.zeros(limit + 1, dtype=bool)
    is_prime = np.ones(limit + 1, dtype=bool)
    is_prime[0] = is_prime[1] = False
    for i in range(2, isqrt(limit) + 1):
        if is_prime[i]:
            is_prime[i * i :: i] = False
    return is_prime


def gaussian_primes_in_rect(
    a_min: int, a_max: int, b_min: int, b_max: int
) -> np.ndarray:
    """Generate all Gaussian primes (a, b) with a_min <= a <= a_max, b_min <= b <= b_max.

    A Gaussian integer a + bi is a Gaussian prime iff:
      - If b == 0: |a| is a rational prime AND |a| ≡ 3 (mod 4)
      - If a == 0: |b| is a rational prime AND |b| ≡ 3 (mod 4)
      - Otherwise: a² + b² is a rational prime

    Returns:
        np.ndarray of shape (N, 2) with dtype int32, each row is (a, b).
    """
    # Build coordinate arrays
    a_vals = np.arange(a_min, a_max + 1, dtype=np.int64)
    b_vals = np.arange(b_min, b_max + 1, dtype=np.int64)

    # Max possible norm for sieve
    max_a = max(abs(a_min), abs(a_max))
    max_b = max(abs(b_min), abs(b_max))
    max_norm = int(max_a**2 + max_b**2)
    max_axis = max(max_a, max_b)

    # Sieve: we need primes up to max_norm (for a²+b² case)
    # and also up to max_axis (for axis cases)
    sieve_limit = max(max_norm, max_axis, 2)
    is_prime = sieve_of_eratosthenes(sieve_limit)

    primes = []

    # Case 1: b == 0 axis
    if b_min <= 0 <= b_max:
        for a in a_vals:
            aa = abs(int(a))
            if aa >= 2 and aa < len(is_prime) and is_prime[aa] and aa % 4 == 3:
                primes.append((int(a), 0))

    # Case 2: a == 0 axis
    if a_min <= 0 <= a_max:
        for b in b_vals:
            if b == 0:
                continue  # already handled
            bb = abs(int(b))
            if bb >= 2 and bb < len(is_prime) and is_prime[bb] and bb % 4 == 3:
                primes.append((0, int(b)))

    # Case 3: off-axis (a != 0 and b != 0) — vectorized
    # Filter to non-zero a and b
    a_nz = a_vals[a_vals != 0]
    b_nz = b_vals[b_vals != 0]

    if len(a_nz) > 0 and len(b_nz) > 0:
        aa, bb = np.meshgrid(a_nz, b_nz, indexing="ij")
        norms = aa**2 + bb**2

        # Vectorized prime lookup — norms within sieve range
        flat_norms = norms.ravel()
        mask = flat_norms <= sieve_limit
        is_gp = np.zeros(len(flat_norms), dtype=bool)
        is_gp[mask] = is_prime[flat_norms[mask].astype(int)]

        is_gp = is_gp.reshape(norms.shape)

        # Extract coordinates
        idx = np.argwhere(is_gp)
        for i, j in idx:
            primes.append((int(aa[i, j]), int(bb[i, j])))

    if len(primes) == 0:
        return np.empty((0, 2), dtype=np.int32)

    return np.array(primes, dtype=np.int32)
