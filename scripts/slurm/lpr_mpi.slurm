#!/bin/bash
# Slurm job for one search across several nodes with lpr_mpi (docs/mpi.md): one MPI rank
# per node, one worker thread per CPU, plus each rank's communication thread (mostly asleep).
#
# Submit through scripts/submit.sh, which exports the commit into a run directory:
#   LPR_JOB=$HOME/lpr/scripts/slurm/lpr_mpi.slurm scripts/submit.sh -n mpi_s29 \
#       -a "--config config/omega33.conf --s 29" -- --nodes=2 --time=00:15:00
# or run sbatch from the repository root (or any directory with src/, Makefile, config/):
#   sbatch --nodes=2 scripts/slurm/lpr_mpi.slurm
#
# The #SBATCH values are Rangpur's (largecpu: r730 nodes, 24 cores; the nodes advertise
# 1 MB of memory, so no --mem line). MPI on Rangpur (hello test, 26/10/26): Slurm offers
# PMI-2 only. MPICH starts under `srun --mpi=pmi2`, which delivers Slurm's signals to
# every rank; Open MPI 4.1.1 was not built with Slurm's PMI and needs mpirun.
#
# No checkpoints under MPI yet: give --time enough to finish; the job does not requeue.
# Exit codes as lpr, plus 5: an MPI fatal error (a rank aborted the job, e.g. the
# watchdog found a rank silent for 120 s).
#
# Environment (all optional):
#   MPI_MODULE    mpi/mpich-x86_64 (default) or mpi/openmpi-x86_64
#   LPR_ARGS      lpr arguments (default "--config config/omega33.conf"); --parallel is
#                 added unless they give --parallel or --dry-run. --mpi is always added.
#   LPR_CXX       compiler (default clang++: 25-30% faster Montgomery code than GCC 8.5)
#   LPR_CXXFLAGS  extra flags (default -march=native; every largecpu node is Haswell)
#   GMP_PREFIX    GMP installation (default $HOME/opt/gmp-6.3.0)
#   LPR_COMMIT    commit compiled into the run header (set by submit.sh)
#   LPR_LAUNCH    launch command instead of the module's default

#SBATCH --job-name=lpr_mpi
#SBATCH --partition=largecpu
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=24
#SBATCH --exclusive
#SBATCH --time=00:30:00
#SBATCH --output=lpr_mpi_%j.out
#SBATCH --error=lpr_mpi_%j.err

set -u
mpi_module=${MPI_MODULE:-mpi/mpich-x86_64}
module purge 2>/dev/null || true
module load "$mpi_module" || { echo "module load $mpi_module failed"; exit 2; }
threads=${SLURM_CPUS_PER_TASK:-24}
case "$mpi_module" in
    *mpich*)   mpi_inc=/usr/include/mpich-x86_64;   mpi_lib=/usr/lib64/mpich/lib
               launch=${LPR_LAUNCH:-srun --mpi=pmi2 -N ${SLURM_NNODES} -n ${SLURM_NNODES} -c ${threads}} ;;
    *openmpi*) mpi_inc=/usr/include/openmpi-x86_64; mpi_lib=/usr/lib64/openmpi/lib
               launch=${LPR_LAUNCH:-mpirun -np ${SLURM_NNODES} --map-by ppr:1:node --bind-to none} ;;
    *) echo "unknown MPI module $mpi_module (set LPR_LAUNCH and edit the paths in this script)"; exit 2 ;;
esac

echo "Job ${SLURM_JOB_ID}: ${SLURM_NNODES} nodes (${SLURM_JOB_NODELIST}), ${threads} threads per rank, module $mpi_module, $(date '+%Y-%m-%d %H:%M:%S')"
lscpu 2>/dev/null | grep -E "^Model name|^Thread\(s\) per core" || true

# Build on the first node with the compiler directly, not through mpicxx: MPICH's wrapper
# adds Red Hat's hardening flags (-D_GLIBCXX_ASSERTIONS, -specs=...), which slow lpr and
# which clang does not accept. LTO only if the linker supports it (Rangpur has no lld).
gmp=${GMP_PREFIX:-$HOME/opt/gmp-6.3.0}
cxx=${LPR_CXX:-clang++}
flags=${LPR_CXXFLAGS:--march=native}
mpi_libs="-lgmpxx -lgmp -pthread -L$mpi_lib -Wl,-rpath,$mpi_lib -lmpi"
build() { make lpr_mpi MPICXX="$cxx" GMP_PREFIX="$gmp" GIT_COMMIT="${LPR_COMMIT:-unknown}" LDLIBS="$mpi_libs" "$@"; }
rm -f lpr_mpi
build EXTRA_CXXFLAGS="$flags -I$mpi_inc" || build EXTRA_CXXFLAGS="$flags -I$mpi_inc" LTOFLAGS= \
    || { echo "build failed"; exit 2; }
export LD_LIBRARY_PATH="$gmp/lib:${LD_LIBRARY_PATH:-}"

lpr_args=${LPR_ARGS:---config config/omega33.conf}
case " $lpr_args " in
    *" --parallel "*|*" --dry-run "*) lpr_mode="" ;;
    *) lpr_mode="--parallel" ;;
esac
# Progress log: rank 0 writes lpr_mpi_<jobid>.log (its lines cover every rank: mpi_pool,
# all_ranks_processed, all_ranks_candidates); rank r writes lpr_mpi_<jobid>.log.rank<r>
cmd=(./lpr_mpi $lpr_mode --mpi --threads "$threads" --log-interval "${LPR_LOG_INTERVAL:-60}"
     --log-file "lpr_mpi_${SLURM_JOB_ID}.log" $lpr_args)
echo "Launch: $launch"
echo "Command: ${cmd[*]}"
$launch "${cmd[@]}"
status=$?
echo "lpr_mpi exited with status ${status} at $(date '+%Y-%m-%d %H:%M:%S')"
exit ${status}
