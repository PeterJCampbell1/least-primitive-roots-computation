import math

# --- Constants ---
MAX_PRIME = 4000
OMEGA = 33
BOUND = 1.8e54
FIXED_PREFIX = [2]            # Mandatory base primes in every set

def sieve_primes(limit):
    sieve = [True] * (limit + 1)
    sieve[0] = sieve[1] = False
    for p in range(2, int(math.sqrt(limit)) + 1):
        if sieve[p]:
            for i in range(p * p, limit + 1, p):
                sieve[i] = False
    return [p for p, is_p in enumerate(sieve) if is_p]

def main():
    primes = sieve_primes(MAX_PRIME)
    log_primes = [math.log(p) for p in primes]
    log_bound = math.log(BOUND)
    
    fixed_indices = [primes.index(p) for p in FIXED_PREFIX]
    fixed_log_sum = sum(math.log(p) for p in FIXED_PREFIX)
    
    if fixed_log_sum > log_bound:
        print("Error: Fixed prefix already exceeds the bound.")
        return

    start_idx = fixed_indices[-1] + 1

    # --- PASS 1: Count total valid subsets (zero memory overhead) ---
    print("Pass 1: Scanning search space and counting valid subsets...")
    total_count = 0
    
    def count_dfs(curr_idx, depth, current_log_sum):
        nonlocal total_count
        if depth == OMEGA:
            if current_log_sum <= log_bound:
                total_count += 1
            return

        if current_log_sum > log_bound:
            return

        for i in range(curr_idx, len(primes)):
            next_log_sum = current_log_sum + log_primes[i]
            if next_log_sum > log_bound:
                break
            count_dfs(i + 1, depth + 1, next_log_sum)

    count_dfs(start_idx, len(FIXED_PREFIX), fixed_log_sum)
    print(f"Total valid subsets found: {total_count:,}\n")
    
    if total_count == 0:
        print("No valid subsets found under these constraints.")
        return

    # --- Prompt user for N ---
    try:
        user_input = input(f"Enter interval N (subsets per task chunk): ")
        N = int(user_input)
        if N <= 0:
            print("N must be greater than 0.")
            return
    except ValueError:
        print("Invalid number entered. Exiting.")
        return

    # --- PASS 2: Re-run DFS and save subsets every N valid leaves ---
    print(f"\nPass 2: Re-running DFS to capture milestones every {N} subsets...")
    milestones = []
    current_subset = list(FIXED_PREFIX)
    current_count = 0
    
    def collect_dfs(curr_idx, current_log_sum):
        nonlocal current_count
        if len(current_subset) == OMEGA:
            if current_log_sum <= log_bound:
                current_count += 1
                # Save snapshot on the first subset and every Nth subset thereafter
                if current_count == 1 or current_count % N == 0:
                    milestones.append((current_count, list(current_subset)))
            return

        if current_log_sum > log_bound:
            return

        for i in range(curr_idx, len(primes)):
            p = primes[i]
            next_log_sum = current_log_sum + log_primes[i]
            if next_log_sum > log_bound:
                break
            
            current_subset.append(p)
            collect_dfs(i + 1, next_log_sum)
            current_subset.pop()

    collect_dfs(start_idx, fixed_log_sum)

    # Print out the collected milestone boundaries
    print(f"\nCaptured {len(milestones)} milestone boundaries:")
    for idx, (subset_num, sub) in enumerate(milestones):
        print(f"  Task Marker {idx} (Subset #{subset_num:,}):")
        print(f"    Prefix (First 5): {sub[:5]}")
        print(f"    Full Vector     : {sub}\n")

if __name__ == '__main__':
    main()
