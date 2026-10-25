# Plan: one search across several nodes with MPI

Status: **plan, written 25/10/26; not implemented.** It is the chosen first approach to multi-node runs; the shared-task-pool design in `multinode_plan.md` §2 remains the fallback if MPI proves impractical on the clusters. It builds on work stealing (`work_stealing.md`) and checkpoints (`checkpointing.md`).

## 1. MPI in one page (what we need from it)
- **Launch:** `srun ./lpr …` (or `mpirun`) starts the same program as several separate processes, called **ranks** and numbered 0 … size−1, typically one per node. They share no memory: everything they exchange is an explicit message. Slurm starts the ranks and tells them how to find each other (PMI/PMIx); the program does the rest.
- **Setup:** `MPI_Init_thread(…, MPI_THREAD_FUNNELED, …)` at start, `MPI_Finalize()` at the end. `MPI_Comm_rank` and `MPI_Comm_size` give the rank's number and the total. "Funneled" means only one thread per rank calls MPI. We give that job to a dedicated communication thread, so the worker threads never touch MPI.
- **Point-to-point messages:**
  - `MPI_Send` / `MPI_Recv` (blocking) and `MPI_Isend` / `MPI_Irecv` / `MPI_Test` (non-blocking).
  - `MPI_Iprobe` checks whether a message is waiting without receiving it.
  - Messages carry a **tag** (we use one per message type: work request, task, refusal, done, …).
  - Messages between one pair of ranks arrive in the order sent; messages from different ranks have no common order. Termination detection must cope with that.
- **Collectives:** every rank calls them together. `MPI_Bcast` sends from one rank to all, `MPI_Reduce` combines values into one rank (sum, max, …), and `MPI_Barrier` waits for everyone. We need them only at the start and the end, e.g. summing every `Counters` field into rank 0 for the final summary.
- **Failure:** if one rank dies, the whole MPI job normally ends. Checkpoints (§3.5) cover that.

## 2. Resources for learning the basics
- **Start here:** the MPI tutorials at https://mpitutorial.com (Wes Kendall). Short, practical lessons: hello world, send/receive, dynamic receiving with `MPI_Probe`, collectives, groups and communicators. Do the first three lessons with the local Open MPI (step 0 below).
- **A broader tutorial:** LLNL's "Message Passing Interface (MPI)", https://hpc-tutorials.llnl.gov/mpi/. Covers the same ground, plus non-blocking communication, derived datatypes and the standard's terminology.
- **Running under Slurm:** the Slurm MPI guide, https://slurm.schedmd.com/mpi_guide.html (`srun --mpi=pmix`, `--nodes`, `--ntasks-per-node`), and the Open MPI documentation, https://docs.open-mpi.org (launching, binding, "running with Slurm").
- **The book:** Gropp, Lusk and Skjellum, *Using MPI* (3rd ed., MIT Press, 2014), plus *Using Advanced MPI* (Gropp, Hoefler, Thakur and Lusk, 2014) for hybrid MPI and threads.
- **The standard,** for exact semantics: https://www.mpi-forum.org/docs/ (MPI 4.1).
- **Directly relevant research:**
  - **Work stealing across MPI ranks:** J. Dinan et al., "Scalable Work Stealing", SC 2009. Also the MPI version of the Unbalanced Tree Search benchmark (Olivier et al., LCPC 2006), whose tree search matches ours.
  - **Termination detection:** F. Mattern, "Global quiescence detection based on credit distribution and recovery", Information Processing Letters 30 (1989), the credit method used in §3.4; and Dijkstra and Safra's token-ring algorithm (Dijkstra, EWD 998) as an alternative.
  - **Hybrid MPI and threads:** R. Rabenseifner, G. Hager and G. Jost, "Hybrid MPI/OpenMP Parallel Programming on Clusters of Multi-Core SMP Nodes", PDP 2009. On why one rank per node with threads inside beats one rank per core here.

## 3. Design
### 3.1 Shape: one rank per node, threads inside
- Each rank runs the existing `Scheduler` (work stealing between its threads) plus one **communication thread**, the only thread that calls MPI. Within a node nothing changes. That keeps the verified in-node code, and avoids 24× more ranks with their messages and memory.
- **Slurm:** `#SBATCH --nodes=N --ntasks-per-node=1 --cpus-per-task=C`, then `srun --mpi=pmix ./lpr --mpi --threads C …`.

### 3.2 Moving work between ranks
- **Tasks are the unit of work.** A `Task` (region plus index path, about 30 integers) is exactly what a checkpoint stores and `DfsStack::restore` rebuilds, so it serialises in a few hundred bytes. Moving one between ranks reuses the checkpoint code: send the indices, then replay and verify on arrival.
- **The bridge between a rank's comm thread and its workers:**
  - **Receiving:** an incoming task goes into the scheduler's `pending_` list. That list is new with checkpointing, so idle local workers already take from it.
  - **Giving:** to give work away, the comm thread posts a steal request on one of its own workers, as an idle thread would. The victim splits on its own thread (`split_region`), and the comm thread sends the resulting `Region` as a `Task`.
  - So the comm thread is just one more thief or receiver for the existing protocol, plus a few new scheduler hooks: "post a request as an external thief", "add a pending task", and "is this rank idle?".

### 3.3 First version (M2): master and workers, with rank 0 holding the pool
Simplest correct design, closest to the shared pool:
- **Rank 0 builds a pool** of many tasks at startup (e.g. 20 × the total thread count), by repeatedly splitting the root with `split_region` and no processing.
- **A rank whose workers are all idle** (no region, no pending task) sends `REQUEST` to rank 0, which replies `TASK` (one task from the pool) or `WAIT`.
- **When the pool runs low,** below about the number of ranks, rank 0 sends `DONATE` to the busy rank holding the most work. That rank splits one region and sends it to rank 0 as `TASK_IN`. This mirrors the in-node protocol at node level; each rank reports its active count in its requests.
- **Termination is simple, because rank 0 sees every task move.** It counts tasks issued (handed out, including donated ones it re-issues) and tasks finished (each rank reports `FINISHED k` when its local regions all complete). A donated task enters the count only when it reaches rank 0. When the pool is empty and outstanding is 0, no work exists anywhere: rank 0 broadcasts `STOP`.
- **A race to avoid:** a rank must not report finished while a donation it sent is still in flight. Count a donation as finished only once rank 0 has received it and re-issued it as its own task. The donor's remaining region stays outstanding until the donor finishes it.
- **Rank 0 also computes:** its comm thread serves requests, and its workers compute like everyone else's.

### 3.4 Later (M3, only if rank 0 becomes a bottleneck): peer-to-peer stealing
- **Stealing:** idle ranks ask random ranks directly (receiver-initiated, as Dinan et al. do).
- **Termination:** now needs a distributed algorithm. Mattern's credit recovery fits best: the root task carries credit 1, a split divides a task's credit between the halves, finished credit is returned to rank 0, and all credit back means done. With credits as powers of two, keep an exact sum, e.g. one count per exponent with carries.
- **Expect to skip this:** with long tasks, rank 0 sees only a few messages per second even at hundreds of ranks.

### 3.5 Statistics, signals and checkpoints
- **Statistics:** each rank publishes locally as now. At the end, `MPI_Reduce` combines the `Counters` from every rank into rank 0: sum for most fields, max for `largest_least_primitive_root` and `d_max_max`, wrapping sum for `supports_checksum`. `for_each_field` already lists them. Rank 0 prints the usual summary, which must equal `--single`'s exactly.
- **Progress lines:** rank 0 gathers counts every log interval; small messages.
- **Signals:** Slurm signals every rank. Each rank tells rank 0, and rank 0 coordinates one checkpoint.
- **Checkpoint protocol:**
  1. Rank 0 broadcasts `PAUSE`.
  2. Every rank pauses its workers (the existing `Scheduler::pause`) and stops sending tasks.
  3. Tasks already in flight are received and counted: rank 0's issued and finished counts say exactly how many tasks must be accounted for.
  4. Each rank sends rank 0 its remaining tasks (verified by replay, as now) and its statistics.
  5. Rank 0 writes one checkpoint file in today's format, with the extra tasks. The file format doesn't change.
- **Resume:** rank 0 reads the file and puts all its tasks in the pool. The number of ranks may differ from the run that wrote it.
- **Node failure:** kills the job. The requeue workflow resumes from the last periodic checkpoint.

### 3.6 Build and run
- **Build:** a `make mpi` target building `lpr_mpi` with `mpicxx`, defining `LPR_MPI`, so the plain `lpr` stays MPI-free for single nodes.
- **On Rangpur:** look for MPI modules (`module avail mpi`, `module avail openmpi`). The MPI library must work with the GCC used for `lpr`, so check with the hello-world from the tutorial first. Slurm's PMIx support decides between `srun --mpi=pmix` and `mpirun`.
- **Locally:** Open MPI, with `mpirun -np 4 --oversubscribe ./lpr_mpi --mpi --threads 2` on the laptop simulating 4 nodes.

## 4. Steps (iterate, verifying each)
0. **Setup:**
   - install an arm64 Open MPI locally (`/opt/homebrew/bin/brew install open-mpi`; the existing `/usr/local` Open MPI 5.0.3 is x86_64 and cannot link with arm64 `lpr`);
   - work through the tutorial basics;
   - run an MPI hello world on 2 Rangpur nodes via Slurm.
1. **M1, static split:**
   - rank 0 splits the root into many tasks and hands them out round-robin;
   - each rank runs its tasks with its threads;
   - `MPI_Reduce` of the statistics, with rank 0 printing the summary.

   The summary must equal `--single` exactly, for 1–8 ranks locally and 2 nodes on Rangpur. Expect poor balance; this is plumbing only.
2. **M2, master and workers** (§3.3): dynamic pool, donation, termination by counts. Verification:
   - exact summaries across rank and thread counts, with stress options (tiny pool, constant donation);
   - Open MPI's `--oversubscribe` with many ranks on the laptop;
   - ThreadSanitizer on a single rank;
   - an OMEGA=32 run on 2–4 Rangpur nodes, compared with the reference and with the single-node run's time.
3. **M4, checkpoints:** the coordinated checkpoint and resume (§3.5), with the interrupt-and-resume chains from `checkpointing.md` repeated with varying rank counts.
4. **Measure, then decide:**
   - idle time at the end of the run and rank 0's message load at increasing node counts on the research cluster;
   - M3 (peer-to-peer stealing) only if rank 0 limits scaling.
