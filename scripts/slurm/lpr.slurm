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

# Search parameters: edit the config file (or add --omega N / --s N, which override it)
srun ./lpr --parallel --config config/omega33.conf --log-interval 120 --log-file "lpr_${SLURM_JOB_ID}.log"
status=$?

case ${status} in
    0) echo "lpr finished: verification complete" ;;
    1) echo "lpr finished: Grosswald failure or unresolved candidate (see lpr_${SLURM_JOB_ID}.err)" ;;
    2) echo "lpr did not start: usage error" ;;
    3) echo "lpr was interrupted: partial run, nothing verified" ;;
    *) echo "lpr exited with unexpected status ${status}" ;;
esac
exit ${status}
