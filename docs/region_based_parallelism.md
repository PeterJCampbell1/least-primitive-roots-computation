# Parallelism via Work-Stealing of Regions
Author: Mittun Sudhahar
Date: 22/10/2026

This plan documents the new simplified parallelism architecture based on dividing the support tree into *regions* and implementing work-stealing for dynamic task balancing across cores.

### Region Definitions:

Consider the tree upon which we perform DFS. The tree is the (connected) poset of the powerset of primes (with poset ordering defined by subset), where we limit the size of subsets to OMEGA (note, by not restricting we can redefine the problem across all possible primes, and then talk about specific subproblems). Next, we define a natural ordering on the children of any node simply by the ordering of primes within the natural numbers. This extends to a lexicographic ordering on the elements of this poset (random sidequest - the thing we are doing is partitioning this space based on clopen sets within an induced topology related to the associated bitopological space to this dual ordering of subset and lexicographic order).   

We prune subtrees based on the extended version of corollary 4. This can be stated as follows:  

Corollary 4 provides a filter for leaf nodes of this tree. If the first node in an in-order traversal of a subtree may be pruned, then the entire subtree may be pruned (see other docs for mathematical definition and proof of this result). This can be stated more concisely as Corollary 4 is monotonic upon the domination ordering where corollary 4 defines a function from the poset to the true/false lattice.  

We now define a *region*. Let P be a prefix (an ordered tuple of primes). Each prefix is associated with a particular subtree of our poset. Let X be the set of all immediate children of P (where we interchange P between the subtree and the prefix) ordered by the prime they are associated with. Then we say a region is a triple (P, lo, hi) where hi > lo + 1 (and the region as a set is defined by taking all nodes in subtree P with prefix's between P:lo and P:hi-1 where hi-1 is the previous prime before hi).   

Our parallelism architecture is defined by associating to each thread an unexplored region of the tree.

### Splitting Regions Across Threads

The initial thread is provided the entire tree (or each thread is seeded with a top level partition of the tree) and then the imbalanced structure of the tree is handled by *work-stealing*.  

Suppose thread 1 has region (P, lo, hi). To steal work, thread 1 computes some value lo < mid < hi. Thread 1 then is given the region (P, lo, mid) and thread 2 is given the region (P, mid, hi). In the event that lo+1 = mid or mid+1 = hi we assign to thread 1 (P:lo, 2, \inf) and to thread 2 (P:mid, 2, \inf) respectively (and we can restrict 2, \inf based on how much each thread has currently computed).  

A thread may apply this procedure whenever it finishes it's own region and becomes idle - this is an implementation of the general work-stealing concept.

### Practical Aspects: (TODO - clean up) 

We can ascribe to one thread the upper bound of infinity, and work-steal from there. Since corollary 4 is eventually true if we increase the size of the prime sufficiently in any index of a subset, this procedure must terminate. To actually choose mid when work-stealing, we can be more efficient by using the logarithmic version of corollary 4 to estimate the midpoint of admissible values in lo/hi (and perhaps split by thirds rather than halves to account for the imbalanced nature of the tree). When doing work-stealing, rather than using locks the thief should request the victim thread for work-stealing to occur. The thief then sleeps until it recieves a refusal, a region or a time threshold passes (in the event a time threshold passes, it must withdraw it's request with atomic operations - a threshold passing would generally mean that the victim is either stuck doing a very complex support, or has been corrupted - we may want to create some sort of structure to account for this and recover from such a state). The thief if given refusals should ask other threads for work with increasing (perhaps exponential backoff) waiting periods in between each ask until some threshold value (perhaps 5ms) for which it must wait before requesting again. The victim can determine whether to partition it's region by checking for requests at each iteration of the DFS (and race conditions can be safely ignored since only one additional iteration of the DFS would be checked) and then splitting it's region. It should refuse in the event it's region is "small". What "small" refers to needs to be tuned in practise - this may mean a particular depth level (perhaps the last 2 levels will never be split).

Deadlocks must also be cleanly handled in the event that an idle thread asks another idle thread for work (we may also maintain some shared structure indicating whether a thread is idle or not). Accesses to shared states do not require locks necessarily, but must be implemented with atomic operations when writing to shared data (but not necessarily reading) to avoid deadlocks.

### Saving Work:

We can save the remaining work by simply saving the region ascribed to each thread. Note that in practise, rather than just lo, we would save the entire state of the stack (and either keep whether we are currently moving down or up the stack or just assume we are moving down when resuming work). This enables us to restart with minimal amounts of work being redone.

In addition, we can save and output an additive checksum based on hashing each support that is emitted and summing this modulo word size. If two runs to completion do not produce the same checksum, then they did not compute the same supports and something has gone wrong. This allows us to check correctness between more naive but more obviously correct versions of the code, and more sophisticated versions. In addition, previous counters such as DFS nodes visited may be double counted by work-stealing (although we should check whether this can actually happen), but the actual supports emitted can never do so.

### Termination:

We should maintain actively the total number of threads which own a region. When this reaches 0, the computation is over.

