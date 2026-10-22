#!/bin/bash
# Slurm job template for a single-node (shared memory) run of lpr.
#
# Fill in the <...> placeholders for your cluster, build lpr on the cluster
# (make release, after loading a compiler and GMP), then submit from the
# repository root with:
#
#   sbatch scripts/slurm/lpr.slurm
#
# lpr takes its thread count from SLURM_CPUS_PER_TASK (1 producer + N-1 consumers).
# --signal makes Slurm send SIGTERM 5 minutes before the time limit, so lpr can
# log its statistics so far before exiting (exit code 3 = interrupted, partial run).

#SBATCH --job-name=lpr
#SBATCH --account=<account>
#SBATCH --partition=<partition>
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=<cpus>
#SBATCH --mem=4G
#SBATCH --time=<HH:MM:SS>
#SBATCH --signal=TERM@300
#SBATCH --output=lpr_%j.out
#SBATCH --error=lpr_%j.err

set -u

# Load a compiler and GMP if your cluster uses environment modules, e.g.:
# module load gcc
# module load gmp

echo "Job ${SLURM_JOB_ID} on $(hostname) with ${SLURM_CPUS_PER_TASK} CPUs"
# Node hardware, for comparing timings across node types
lscpu 2>/dev/null | grep -E "^Model name|^CPU\(s\)|^Thread\(s\) per core|^Core\(s\) per socket|^Socket\(s\)" || true
echo "SMT active: $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo unknown)"
echo "CPU features: $(grep -o -w -E 'bmi2|adx|avx2|avx512f|avx512ifma' /proc/cpuinfo 2>/dev/null | sort -u | tr '\n' ' ')"

# Optional build on the compute node (scripts/submit.sh sets LPR_BUILD=1 and runs
# the job in a fresh per-run copy of the source, which has no .git), after the
# module loads above. build_lpr.sh picks a working compiler, builds GMP into
# $HOME/opt if no usable GMP is found, and compiles LPR_COMMIT into the run header.
if [ "${LPR_BUILD:-0}" = 1 ]; then
    bash ./build_lpr.sh || { echo "Build failed on $(hostname)"; exit 2; }
fi

# Search parameters. LPR_ARGS (set by scripts/submit.sh --lpr-args) replaces the default
# "--config config/omega33.conf", e.g. "--omega 32 --s 29" or "--config config/x.conf --s 29".
# The mode is --parallel unless LPR_ARGS gives one (--single, --parallel or --dry-run;
# lpr accepts only one).
lpr_args=${LPR_ARGS:---config config/omega33.conf}
case " $lpr_args " in
    *" --single "*|*" --parallel "*|*" --dry-run "*) lpr_mode="" ;;
    *) lpr_mode="--parallel" ;;
esac
srun ./lpr ${lpr_mode} ${lpr_args} --log-interval 120 --log-file "lpr_${SLURM_JOB_ID}.log"
status=$?

case ${status} in
    0) case " ${lpr_args} " in
           *" --dry-run "*) echo "lpr finished: dry run (producer only), nothing verified" ;;
           *) echo "lpr finished: verification complete" ;;
       esac ;;
    1) echo "lpr finished: Grosswald failure or unresolved candidate (see lpr_${SLURM_JOB_ID}.err)" ;;
    2) echo "lpr did not start: usage error" ;;
    3) echo "lpr was interrupted: partial run, nothing verified" ;;
    *) echo "lpr exited with unexpected status ${status}" ;;
esac
exit ${status}
