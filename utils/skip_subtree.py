import sympy

OMEGA = 31
S = 28

w = lambda x: (2**(OMEGA-S)) * (2 + (S-1)/(1-x))
t = lambda y: (y / 4220)**(1/16)

def min_suffix(prefix):
    while len(prefix) < OMEGA:
        prefix.append(sympy.nextprime(prefix[-1]))
    return prefix

# Note - this is a floating point approximation and doesn't hold everywhere
# It does tell us when we can/should try an exact test
def skip_subtree(prefix, verbose=False):
    min_support = min_suffix(prefix)
    prod, acc = 1, 0
    for p in prefix:
        prod *= p
    for p in prefix[OMEGA-S:]:
        acc += 1/p
    assert acc < 1
    lhs = w(acc)
    rhs = t(prod)
    
    if verbose:
        print("Minimum suffix: ")
        print(min_support)
        print("Product: ", prod)
        print("Delta: ", 1 - acc)
        print("lhs: ", lhs)
        print("rhs: ", rhs)
        print("Corollary 4 says subtree can be skipped is: ", lhs < rhs)
        print("Global corollary 4 bound says subtree can be skipped is: ", prod > 5.082*(10**53))
        print("")

    return lhs < rhs

prefix1 = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 113, 131, 137, 199]
prefix2 = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 113, 131, 137, 193]
prefix3 = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 113, 131, 137, 191]
prefix4 = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 101, 127, 151, 223]
skip_subtree(prefix1, True)
skip_subtree(prefix2, True)
skip_subtree(prefix3, True)
skip_subtree(prefix4, True)
