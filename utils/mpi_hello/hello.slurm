#!/bin/bash
# MPI smoke test on two nodes (docs/mpi_plan.md, step 0). Submit from this directory:
#   sbatch hello.slurm                      # Open MPI (default)
#   MPI_MODULE=mpi/mpich-x86_64 sbatch hello.slurm
#   MPI_MODULE=mpi/latest sbatch hello.slurm        # Intel MPI
# Fill in or delete the partition/account lines as in your lpr job file.
#
# Expected output (in mpi_hello_<jobid>.out): two "rank ... on <host>" lines with
# DIFFERENT host names, a ping-pong line with the right token, and "ok" twice on the
# reduce line. If srun cannot start the ranks, the script falls back to mpirun.

#SBATCH --job-name=mpi_hello
##SBATCH --partition=<partition>
##SBATCH --account=<account>
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=1G
#SBATCH --time=00:05:00
#SBATCH --output=mpi_hello_%j.out
#SBATCH --error=mpi_hello_%j.err

set -u
module purge 2>/dev/null || true
module load "${MPI_MODULE:-mpi/openmpi-x86_64}" || { echo "module load ${MPI_MODULE:-mpi/openmpi-x86_64} failed"; exit 1; }

echo "Job ${SLURM_JOB_ID}: nodes ${SLURM_JOB_NODELIST}, module ${MPI_MODULE:-mpi/openmpi-x86_64}"
echo "mpicxx: $(mpicxx --showme 2>/dev/null || mpicxx -show 2>/dev/null)"
mpirun --version 2>&1 | head -1
echo "PMI types offered by Slurm: $(srun --mpi=list 2>&1 | tr '\n' ' ')"
command -v ompi_info > /dev/null && echo "Open MPI Slurm/PMIx support: $(ompi_info 2>/dev/null | grep -i -E 'slurm|pmix' | tr -s ' ' | tr '\n' ';')"

# Build on the compute node (same environment the real job will have)
mpicxx -O2 -std=c++17 hello.cpp -o mpi_hello || { echo "build failed"; exit 1; }

# 1. srun with PMIx (preferred), then PMI-2
for pmi in pmix pmi2; do
    echo "--- srun --mpi=$pmi"
    if srun --mpi=$pmi ./mpi_hello; then
        echo "srun --mpi=$pmi: OK"
        exit 0
    fi
    echo "srun --mpi=$pmi: failed"
done

# 2. Fall back to mpirun inside the allocation
echo "--- mpirun (one rank per node)"
if mpirun -np "${SLURM_NNODES}" --map-by ppr:1:node ./mpi_hello 2>/dev/null || mpirun -np "${SLURM_NNODES}" -ppn 1 ./mpi_hello; then
    echo "mpirun: OK"
    exit 0
fi
echo "no launch method worked"
exit 1
