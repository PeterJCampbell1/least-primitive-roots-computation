#!/bin/bash
# Slurm job template for one search across several nodes with lpr_mpi (docs/mpi.md):
# one MPI rank per node, one worker thread per CPU, plus each rank's communication
# thread (mostly asleep).
#
# Fill in the <...> placeholders for your cluster (or override them with sbatch options),
# run the MPI hello test there first (utils/mpi_hello), then submit through submit.sh:
#
#   LPR_JOB=$HOME/lpr/scripts/slurm/lpr_mpi.slurm scripts/submit.sh -n mpi_s29 \
#       -a "--config config/omega33.conf --s 29" -- --nodes=2 --time=00:15:00
#
# or with sbatch from the repository root (LPR_BUILD=1 to build first).
#
# Build: Slurm runs this script on the first node only, but every node runs lpr_mpi, so
# with LPR_BUILD=1 (submit.sh sets it) lpr_mpi is built once on EACH node, in
# build_<host>/, by build_lpr.sh (LPR_TARGET=lpr_mpi): each node's binary is tuned for its
# own CPU (-march=native), which is safe on clusters whose nodes differ. Each rank then
# runs its own node's binary. Without LPR_BUILD=1 an existing ./lpr_mpi is used on every
# node (build it for the oldest CPU in the partition).
#
# MPI flags: the -I/-L/-l/-Wl,/-pthread flags of the MPI compiler wrapper (mpicxx
# --showme for Open MPI, mpicxx -show for MPICH and Intel MPI), added to the compiler
# build_lpr.sh chooses. The wrapper's other flags are dropped (e.g. Red Hat's MPICH
# wrapper adds -D_GLIBCXX_ASSERTIONS and -specs=..., which slow lpr and clang rejects).
#
# Launch: Open MPI with `srun --mpi=pmix` if Slurm offers PMIx, else mpirun; MPICH and
# Intel MPI with `srun --mpi=pmi2` if Slurm offers PMI-2, else mpirun. srun is preferred:
# it delivers Slurm's signals to every rank. Set LPR_LAUNCH if the hello test shows that
# another launcher works.
#
# Checkpoints (docs/checkpointing.md, docs/mpi.md): rank 0 saves the whole search to
# lpr.ckpt in the run directory every LPR_CHECKPOINT_INTERVAL seconds (default 600) and
# when the job's time limit approaches. Slurm signals this script (--signal=B:USR1@300),
# which forwards SIGUSR1 to the launcher; srun and mpirun both pass it to every rank
# without killing anything, the ranks checkpoint together and exit with code 4. If
# lpr.ckpt exists when the job starts, the run continues from it, on any number of nodes.
# After an exit with code 4 the job requeues itself (LPR_REQUEUE=1, the default) until the
# search is complete, at most LPR_MAX_RUNS runs (default 20); a requeued job rebuilds on
# its new nodes. A node failure ends the whole job (plain MPI cannot lose a rank); with
# --requeue Slurm may requeue it, and it resumes from the last periodic checkpoint.
# Dynamic allocations (docs/elastic_pool.md): without --cpus-per-task, each rank uses
# every CPU of its own node (srun --whole, and lpr's --threads 0: the CPUs the process
# may use), so a job may mix node sizes; with --nodes=MIN-MAX Slurm starts the job with
# as many nodes as it can find. Larger nodes can be preferred with --prefer=<feature>
# (Slurm 22.05+) and small ones excluded with --mincpus=N, e.g.
#   sbatch --nodes=2-4 --mincpus=28 --prefer=<feature of the big nodes> lpr_mpi.slurm
#
# Shared pool (LPR_POOL=<directory on shared storage>): the job joins (or creates) a
# pool shared with any number of other jobs, e.g. a job array
#   LPR_POOL=$HOME/pools/k31 sbatch --array=1-4 --nodes=1-2 lpr_mpi.slurm
# Jobs may start, leave and die at any time: a job killed by a node failure loses at most
# its last LPR_CHECKPOINT_INTERVAL seconds, released by another job after LPR_POOL_STALE
# seconds. A job leaves with exit 0 when the pool has nothing left for it; at the time
# limit it returns its work to the pool, exits with 4 and requeues itself to rejoin. The
# last job to leave prints the whole search's merged summary; any time later,
# `lpr --pool-status <directory>` (with the same --omega/--s) reports on it and verifies it.
# Exit codes as lpr, plus 5: an MPI fatal error (a rank aborted the job, e.g. the
# watchdog found a rank silent for 120 s).
#
# Environment (all optional):
#   MPI_MODULE      MPI environment module to load first (default: the MPI_MODULE line
#                   below, empty in the template), e.g. mpi/mpich-x86_64
#   LPR_ARGS        lpr arguments (default "--config config/omega33.conf"); --parallel is
#                   added unless they give --parallel or --dry-run. --mpi is always added.
#   LPR_BUILD       1: build lpr_mpi on every node first (set by submit.sh)
#   LPR_CXX, LPR_CXXFLAGS, LPR_NATIVE, GMP_PREFIX, LPR_COMMIT: as for build_lpr.sh
#   LPR_MPI_CFLAGS, LPR_MPI_LIBS: MPI compile and link flags instead of the wrapper's
#   LPR_LAUNCH      launch command (e.g. "srun --mpi=pmi2"), instead of the choice above
#   LPR_LOG_INTERVAL seconds between progress lines (default 60)
#   LPR_CHECKPOINT_INTERVAL seconds between checkpoints (default 600)
#   LPR_REQUEUE     0: do not requeue after a checkpoint (continue with resubmit.sh)
#   LPR_MAX_RUNS    most runs of one job when requeueing (default 20)
#   LPR_POOL        a pool directory on shared storage: share the search with other jobs
#   LPR_POOL_STALE  seconds without a heartbeat before another job releases a lease (600)
#   LPR_POOL_POLL   seconds between rank 0's looks at the pool (10)

#SBATCH --job-name=lpr_mpi
#SBATCH --account=<account>
#SBATCH --partition=<partition>
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=<cpus>
#SBATCH --mem=4G
#SBATCH --time=<HH:MM:SS>
#SBATCH --exclusive
#SBATCH --signal=B:USR1@300
#SBATCH --requeue
#SBATCH --open-mode=append
#SBATCH --output=lpr_mpi_%j.out
#SBATCH --error=lpr_mpi_%j.err

set -u

# Load a compiler and GMP if your cluster uses environment modules, e.g.:
# module load gcc gmp

# The MPI library: set your cluster's MPI module here (see `module avail mpi`; the MPI hello
# test, utils/mpi_hello, shows which one works), or pass MPI_MODULE when submitting. Empty:
# no module is loaded, and mpicxx and mpirun must already be on PATH.
MPI_MODULE=${MPI_MODULE:-}                  # e.g. mpi/mpich-x86_64 (Rangpur)
if [ -n "$MPI_MODULE" ]; then
    module load "$MPI_MODULE" || { echo "Error: module load $MPI_MODULE failed (see module avail mpi)"; exit 2; }
fi

nodes=${SLURM_NNODES:-1}
# Threads per rank: --cpus-per-task if given; otherwise every CPU of each rank's own node
# (the launch gives each task its whole node, and lpr counts the CPUs it may use)
if [ -n "${SLURM_CPUS_PER_TASK:-}" ]; then
    threads=$SLURM_CPUS_PER_TASK; lpr_threads=$threads; per_node="$threads threads per rank"
else
    # Smallest CPU count over the nodes (SLURM_JOB_CPUS_PER_NODE is e.g. "28(x2),36"),
    # for launchers that cannot give each task its whole node
    threads=$(echo "${SLURM_JOB_CPUS_PER_NODE:-1}" | tr ',' '\n' | sed 's/(x[0-9]*)//' | sort -n | head -1)
    lpr_threads=0; per_node="each rank uses its node's CPUs (${SLURM_JOB_CPUS_PER_NODE:-?})"
fi
echo "Job ${SLURM_JOB_ID}: ${nodes} nodes (${SLURM_JOB_NODELIST}), ${per_node} (run $(( ${SLURM_RESTART_COUNT:-0} + 1 )) of this job, $(date '+%Y-%m-%d %H:%M:%S'))"

# An MPI library must be available: its compiler wrapper (for the flags) and launcher,
# unless both are given explicitly
have_flags=0; [ -n "${LPR_MPI_CFLAGS:-}${LPR_MPI_LIBS:-}" ] && have_flags=1
have_launch=0; [ -n "${LPR_LAUNCH:-}" ] && have_launch=1
if { [ $have_flags = 0 ] && ! command -v mpicxx > /dev/null 2>&1; } \
   || { [ $have_launch = 0 ] && ! command -v mpirun > /dev/null 2>&1; }; then
    echo "Error: no MPI library found (mpicxx and mpirun are not on PATH)."
    echo "Load one: set MPI_MODULE in this script, or submit with MPI_MODULE=<module> (see module avail mpi)."
    exit 2
fi
# The MPI library's version: Open MPI and Intel MPI print it on the first line of
# `mpirun --version`; MPICH's Hydra launcher prints "HYDRA build details:" there and the
# version on a later "Version:" line
mpi_version() {
    local text; text=$(mpirun --version 2>&1)
    case "$text" in
        *"HYDRA build details"*) echo "MPICH (Hydra) $(echo "$text" | sed -n 's/^ *Version: *//p' | head -1)" ;;
        *) echo "$text" | head -1 ;;
    esac
}
echo "MPI: ${MPI_MODULE:-no module}; $(mpi_version)"

# MPI compile and link flags (unless given)
mpi_cflags=${LPR_MPI_CFLAGS:-}
mpi_libs=${LPR_MPI_LIBS:-}
if [ -z "$mpi_cflags$mpi_libs" ]; then
    wrapper=$(mpicxx --showme 2>/dev/null || mpicxx -show 2>/dev/null || true)
    [ -n "$wrapper" ] || { echo "Error: mpicxx did not report its flags (--showme or -show): set LPR_MPI_CFLAGS and LPR_MPI_LIBS"; exit 2; }
    for flag in $wrapper; do
        case "$flag" in
            -I*) mpi_cflags="$mpi_cflags $flag" ;;
            -L*|-l*|-Wl,*|-pthread) mpi_libs="$mpi_libs $flag" ;;
        esac
    done
fi
echo "MPI compile flags:${mpi_cflags}"
echo "MPI link flags:${mpi_libs}"

# How to start one rank per node
launch=${LPR_LAUNCH:-}
if [ -z "$launch" ]; then
    plugins=$(srun --mpi=list 2>&1 || true)
    if [ "$lpr_threads" = 0 ] && srun --help 2>&1 | grep -q -- "--whole"; then
        srun_step="-N ${nodes} -n ${nodes} --ntasks-per-node=1 --whole" # Each task gets its whole node
    else
        srun_step="-N ${nodes} -n ${nodes} --ntasks-per-node=1 -c ${threads}"
    fi
    if mpirun --version 2>&1 | grep -q "Open MPI"; then
        export PMIX_MCA_psec=native # PMIx's optional munge security plugin is often missing: no warnings
        if echo "$plugins" | grep -qw pmix; then launch="srun --mpi=pmix $srun_step"
        else launch="mpirun -np ${nodes} --map-by ppr:1:node --bind-to none"; fi
    else # MPICH, Intel MPI and their derivatives speak PMI-2 under Slurm
        if echo "$plugins" | grep -qw pmi2; then launch="srun --mpi=pmi2 $srun_step"
        else launch="mpirun -np ${nodes} -ppn 1"; fi
    fi
fi

# Build lpr_mpi on every node, in parallel, each in its own directory
if [ "${LPR_BUILD:-0}" = 1 ]; then
    build_script=./build_lpr.sh                       # copied in by submit.sh
    [ -f "$build_script" ] || build_script=scripts/slurm/build_lpr.sh
    [ -f "$build_script" ] || { echo "build_lpr.sh not found"; exit 2; }
    export LPR_TARGET=lpr_mpi LPR_MPI_CFLAGS="$mpi_cflags" LPR_MPI_LIBS="$mpi_libs"
    build_cpus="-c ${threads}"
    [ "$lpr_threads" = 0 ] && srun --help 2>&1 | grep -q -- "--whole" && build_cpus="--whole"
    srun --mpi=none -N "${nodes}" -n "${nodes}" --ntasks-per-node=1 $build_cpus bash -c '
        host=$(hostname -s); dir=build_$host
        rm -rf "$dir" && mkdir -p "$dir" && cp -R src Makefile "$dir"/ && cp "$1" "$dir"/build_lpr.sh
        cd "$dir" && bash ./build_lpr.sh > build.log 2>&1
        status=$?
        echo "$host: $(grep -h "^\[build_lpr\] using" build.log | tail -1) (exit $status; log in $dir/build.log)"
        exit $status' build "$build_script" || { echo "Build failed on at least one node (see build_*/build.log)"; exit 2; }
    # Each rank starts its own node's binary. A script, not an inline bash -c string:
    # some launchers (Open MPI's mpirun) split arguments containing spaces and quotes.
    printf '#!/bin/bash\nexec "./build_$(hostname -s)/lpr_mpi" "$@"\n' > run_rank.sh
    chmod +x run_rank.sh
    binary=./run_rank.sh
else
    [ -x ./lpr_mpi ] || { echo "./lpr_mpi not found: build it first, or submit with LPR_BUILD=1"; exit 2; }
    binary=./lpr_mpi
fi

lpr_args=${LPR_ARGS:---config config/omega33.conf}
case " $lpr_args " in
    *" --parallel "*|*" --dry-run "*) lpr_mode="" ;;
    *) lpr_mode="--parallel" ;;
esac
# Checkpoints: a requeued or resubmitted job finds lpr.ckpt and continues from it
checkpoint_args="--checkpoint lpr.ckpt --checkpoint-interval ${LPR_CHECKPOINT_INTERVAL:-600}"
if [ -n "${LPR_POOL:-}" ]; then
    # The pool holds the state (no lpr.ckpt); a requeued job simply rejoins it
    checkpoint_args="--pool ${LPR_POOL} --checkpoint-interval ${LPR_CHECKPOINT_INTERVAL:-600} --pool-stale ${LPR_POOL_STALE:-600} --pool-poll ${LPR_POOL_POLL:-10}"
    echo "Pool: ${LPR_POOL}"
elif [ -f lpr.ckpt ]; then
    checkpoint_args="$checkpoint_args --resume lpr.ckpt"
    echo "Continuing from lpr.ckpt: $(grep -E '^(segments|elapsed_seconds|complete) ' lpr.ckpt | tr '\n' ' ')"
fi
# Progress log: rank 0 writes lpr_mpi_<jobid>.log (its all_ranks_* fields cover every
# rank); rank r writes lpr_mpi_<jobid>.log.rank<r>
args="$lpr_mode --mpi --threads $lpr_threads --log-interval ${LPR_LOG_INTERVAL:-60} --log-file lpr_mpi_${SLURM_JOB_ID}.log $checkpoint_args $lpr_args"
echo "Launch: $launch"
echo "Running: $binary $args"
# The launcher runs in the background so that this script can forward Slurm's warning
# (SIGUSR1, --signal=B:USR1@300) to it; it passes the signal on to every rank
$launch $binary $args &
launcher=$!
trap 'echo "Time limit approaching: asking lpr_mpi to checkpoint ($(date "+%H:%M:%S"))"; kill -USR1 $launcher 2>/dev/null' USR1
status=0
while true; do
    wait $launcher
    status=$?
    # wait returns >128 when interrupted by the trapped signal; wait again for the real status
    kill -0 $launcher 2>/dev/null || break
done
trap - USR1

case ${status} in
    0) if [ -n "${LPR_POOL:-}" ]; then
           echo "lpr_mpi left the pool: no work left for this job (see the end of the output: the whole search's summary appears there once the pool is complete)"
           exit 0
       fi
       case " ${lpr_args} " in
           *" --dry-run "*) echo "lpr_mpi finished: dry run (producer only), nothing verified" ;;
           *) echo "lpr_mpi finished: verification complete" ;;
       esac ;;
    1) echo "lpr_mpi finished: Grosswald failure or unresolved candidate (see lpr_mpi_${SLURM_JOB_ID}.err)" ;;
    2) echo "lpr_mpi did not start: usage error (see the Running: line above and lpr_mpi_${SLURM_JOB_ID}.err)" ;;
    3) echo "lpr_mpi was interrupted: partial run, nothing verified, no checkpoint written" ;;
    4|143) # 143: SIGTERM before lpr_mpi had started (lpr.ckpt, if any, is untouched)
       if [ -n "${LPR_POOL:-}" ]; then
           # Pool: the job returned its work to the pool; requeue to rejoin it
           runs=$(( ${SLURM_RESTART_COUNT:-0} + 1 ))
           echo "lpr_mpi left the pool at the time limit, returning its work (${runs} runs of this job so far)"
           if [ "${LPR_REQUEUE:-1}" = 1 ] && [ "${runs}" -lt "${LPR_MAX_RUNS:-20}" ]; then
               echo "Requeueing job ${SLURM_JOB_ID} to rejoin the pool"
               scontrol requeue "${SLURM_JOB_ID}" && exit 0
           fi
           exit ${status}
       fi
       if [ ${status} = 143 ] && [ ! -f lpr.ckpt ]; then
           echo "lpr_mpi was killed by SIGTERM before it started; no checkpoint exists"
           exit ${status}
       fi
       runs=$(grep '^segments ' lpr.ckpt 2>/dev/null | awk '{print $2}')
       echo "lpr_mpi was interrupted and saved its progress to lpr.ckpt (${runs:-?} runs so far)"
       if [ "${LPR_REQUEUE:-1}" = 1 ] && [ "${runs:-0}" -lt "${LPR_MAX_RUNS:-20}" ]; then
           echo "Requeueing job ${SLURM_JOB_ID} to continue"
           scontrol requeue "${SLURM_JOB_ID}" && exit 0
           echo "scontrol requeue failed: continue with resubmit.sh in $(pwd)"
       else
           echo "Not requeueing (LPR_REQUEUE=${LPR_REQUEUE:-1}, limit LPR_MAX_RUNS=${LPR_MAX_RUNS:-20}): continue with resubmit.sh in $(pwd)"
       fi ;;
    5) echo "lpr_mpi aborted: an MPI fatal error (see lpr_mpi_${SLURM_JOB_ID}.err)" ;;
    *) echo "lpr_mpi exited with status ${status}" ;;
esac
exit ${status}
