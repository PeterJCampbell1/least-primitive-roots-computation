# Quick Python verification script
primes = [
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
    59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131
]

min_delta = float('inf')
best_s = 0

for s in range(1, 32):
    # Sum inverses of the largest s primes among the first 32
    sum_inv = sum(1.0 / q for q in primes[32 - s:32])
    delta_prime = 1.0 - sum_inv
    
    if delta_prime > 0:
        W = ((2.0 * delta_prime + s - 1.0) / delta_prime) * (2 ** (32 - s))
        val = 4220.0 * (W ** 16)
        if val < min_delta:
            min_delta = val
            best_s = s

print(f"Optimal s = {best_s}")
print(f"Delta_32 = {min_delta:.6e}")
