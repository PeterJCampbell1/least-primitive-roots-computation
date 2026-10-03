TARGET_DEPTH = 26
import sympy
import time

OMEGA = 31
S = 28
VERBOSE_LOCAL = False
VERBOSE_GLOBAL = False
RUN_GLOBAL = False
STORE_PREFIXES = False

w = lambda x: (2**(OMEGA-S)) * (2 + (S-1)/(1-x))
t = lambda y: (y / 4220)**(1/16)

# Generate a pool of primes
PRIMES = [int(p) for p in sympy.primerange(2, 1000000)]
print("Generated primes")

def min_suffix(prefix):
    """Fills remaining slots up to OMEGA with the smallest consecutive primes."""
    current = list(prefix)
    last_p = current[-1]
    
    idx = 0
    while PRIMES[idx] <= last_p:
        idx += 1
        
    needed = OMEGA - len(current)
    return current + PRIMES[idx : idx + needed]

glob_min_pre = min_suffix([2])
acc = 0
for p in glob_min_pre[OMEGA-S:]:
    acc += 1/p
global_bound = 4220*((w(acc))**16)
print("Global bound is: ", global_bound)

def skip_subtree_local(prefix):
    full_support = min_suffix(prefix)
    
    prod = 1
    for p in full_support:
        prod *= p
        
    acc = 0.0
    for p in full_support[OMEGA-S:]:
        acc += 1.0 / p
        
    assert acc < 1
        
    lhs = w(acc)
    rhs = t(prod)
    
    return lhs*(1.01) < rhs # Added 1% error buffer for conservative estimates

def skip_subtree_global(prefix):
    full_support = min_suffix(prefix)
    prod = 1
    for p in full_support:
        prod *= p

    #return prod > 5.082 * (10**53)
    return prod > global_bound

def count_valid_prefixes(target_depth=8, mode="local"):
    nodes_visited = 0
    certified_prunes = 0
    active_prefixes = []
    num_active = 0

    # Start at mandatory root [2, 3, 5, 7]
    stack = [[2, 3, 5, 7]]

    while stack:
        prefix = stack.pop()
        nodes_visited += 1
        depth = len(prefix)

        # 1. Evaluate bulk certification for the subtree
        if mode == "local":
            can_skip = skip_subtree_local(prefix)
        else:
            can_skip = skip_subtree_global(prefix)

        if can_skip:
            certified_prunes += 1
            continue  # Subtree certified! Do not expand deeper.

        # 2. Reached target depth without certification
        if depth == target_depth:
            num_active += 1
            if num_active % (5*(10**6)) == 0:
                print(f"Completed {num_active} prefixes of length {target_depth}")
            if STORE_PREFIXES:
                active_prefixes.append(prefix)
            continue

        # 3. Branching: Push candidates in REVERSE order so smaller primes are popped first
        last_p = prefix[-1]
        p_idx = PRIMES.index(last_p) + 1
        
        # Look at consecutive prime candidates
        candidates = PRIMES[p_idx : p_idx + 15]
        
        for next_p in reversed(candidates):
            new_prefix = prefix + [next_p]
            
            # Prune invalid paths where the s-tail reciprocal sum >= 1
            s_tail = new_prefix[OMEGA-S:] if len(new_prefix) > (OMEGA-S) else new_prefix[3:]
            if sum(1.0/p for p in s_tail) >= 1.0:
                continue
                
            stack.append(new_prefix)

    return nodes_visited, certified_prunes, num_active, active_prefixes


if __name__ == "__main__":
    print(f"--- Running Prefix Search at Depth {TARGET_DEPTH} ---")

    # Local Bounding
    nodes_loc, prunes_loc, num_active_loc, active_loc = count_valid_prefixes(TARGET_DEPTH, mode="local")

    print(f"\n[ LOCAL BOUNDING (Depth {TARGET_DEPTH}) ]")
    print(f"Active Prefixes Remaining ({num_active_loc})")
    if VERBOSE_LOCAL:
        for p in active_loc:
            print(" ", p)

    # Global Bounding
    if RUN_GLOBAL:  
        nodes_glo, prunes_glo, num_active_glo, active_glo = count_valid_prefixes(TARGET_DEPTH, mode="global")
        print(f"\n[ GLOBAL BOUNDING (Depth {TARGET_DEPTH}) ]")
        print(f"Active Prefixes Remaining ({num_active_glo}):")
        if VERBOSE_GLOBAL:
            for p in active_glo:
                print(" ", p)
    else:
        print("Elected not to run global bound check")
