#!/bin/bash
# Slurm job template for a single-node (shared memory) run of lpr.
#
# Fill in the <...> placeholders for your cluster, build lpr on the cluster
# (make release, after loading a compiler and GMP), then submit from the
# repository root with:
#
#   sbatch scripts/slurm/lpr.slurm
#
# lpr takes its thread count from SLURM_CPUS_PER_TASK.
#
# Checkpoints (docs/checkpointing.md): lpr saves its progress to lpr.ckpt in the run
# directory every LPR_CHECKPOINT_INTERVAL seconds (default 600) and when Slurm sends
# SIGTERM 5 minutes before the time limit (--signal), then exits with code 4. If
# lpr.ckpt exists when the job starts, lpr continues from it (--resume). After an
# exit with code 4 the job requeues itself (LPR_REQUEUE=1, the default) until the
# search is complete, at most LPR_MAX_RUNS runs (default 20) in total, so a search
# longer than the time limit finishes by itself. With LPR_REQUEUE=0, continue it by
# running resubmit.sh in the run directory. Output from every run is appended to
# lpr_<jobid>.out (a requeued job keeps its job id).

#SBATCH --job-name=lpr
#SBATCH --account=<account>
#SBATCH --partition=<partition>
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=<cpus>
#SBATCH --mem=4G
#SBATCH --time=<HH:MM:SS>
#SBATCH --signal=TERM@300
#SBATCH --requeue
#SBATCH --open-mode=append
#SBATCH --output=lpr_%j.out
#SBATCH --error=lpr_%j.err

set -u

# Load a compiler and GMP if your cluster uses environment modules, e.g.:
# module load gcc
# module load gmp

echo "Job ${SLURM_JOB_ID} on $(hostname) with ${SLURM_CPUS_PER_TASK} CPUs (run $(( ${SLURM_RESTART_COUNT:-0} + 1 )) of this job, $(date '+%Y-%m-%d %H:%M:%S'))"
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
# The mode is --parallel (work stealing) unless LPR_ARGS gives one (--single,
# --parallel, --parallel-queue or --dry-run; lpr accepts only one).
lpr_args=${LPR_ARGS:---config config/omega33.conf}
case " $lpr_args " in
    *" --single "*|*" --parallel "*|*" --parallel-queue "*|*" --dry-run "*) lpr_mode="" ;;
    *) lpr_mode="--parallel" ;;
esac
# Checkpoints (not available with --parallel-queue). A requeued or resubmitted job
# finds lpr.ckpt and continues from it.
checkpoint_args=""
case " $lpr_args " in
    *" --parallel-queue "*) ;;
    *) checkpoint_args="--checkpoint lpr.ckpt --checkpoint-interval ${LPR_CHECKPOINT_INTERVAL:-600}"
       if [ -f lpr.ckpt ]; then
           checkpoint_args="$checkpoint_args --resume lpr.ckpt"
           echo "Continuing from lpr.ckpt: $(grep -E '^(segments|elapsed_seconds|complete) ' lpr.ckpt | tr '\n' ' ')"
       fi ;;
esac
srun ./lpr ${lpr_mode} ${lpr_args} ${checkpoint_args} --log-interval 120 --log-file "lpr_${SLURM_JOB_ID}.log"
status=$?

case ${status} in
    0) case " ${lpr_args} " in
           *" --dry-run "*) echo "lpr finished: dry run (producer only), nothing verified" ;;
           *) echo "lpr finished: verification complete" ;;
       esac ;;
    1) echo "lpr finished: Grosswald failure or unresolved candidate (see lpr_${SLURM_JOB_ID}.err)" ;;
    2) echo "lpr did not start: usage error" ;;
    3) echo "lpr was interrupted: partial run, nothing verified, no checkpoint written" ;;
    4|143) # 143: SIGTERM before lpr had started (lpr.ckpt, if any, is untouched)
       if [ ${status} = 143 ] && [ ! -f lpr.ckpt ]; then
           echo "lpr was killed by SIGTERM before it started; no checkpoint exists"
           exit ${status}
       fi
       runs=$(grep '^segments ' lpr.ckpt 2>/dev/null | awk '{print $2}')
       echo "lpr was interrupted and saved its progress to lpr.ckpt (${runs:-?} runs so far)"
       if [ "${LPR_REQUEUE:-1}" = 1 ] && [ "${runs:-0}" -lt "${LPR_MAX_RUNS:-20}" ]; then
           echo "Requeueing job ${SLURM_JOB_ID} to continue"
           scontrol requeue "${SLURM_JOB_ID}" && exit 0
           echo "scontrol requeue failed: continue with resubmit.sh in $(pwd)"
       else
           echo "Not requeueing (LPR_REQUEUE=${LPR_REQUEUE:-1}, limit LPR_MAX_RUNS=${LPR_MAX_RUNS:-20}): continue with resubmit.sh in $(pwd)"
       fi ;;
    *) echo "lpr exited with unexpected status ${status}" ;;
esac
exit ${status}
