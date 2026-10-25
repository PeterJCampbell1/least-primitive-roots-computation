// hello.cpp: MPI smoke test for the multi-node plan (docs/mpi_plan.md, step 0).
//
// Checks, with one rank per node:
//   1. every rank starts, and knows its rank, the total and its host;
//   2. the threading level we plan to use (MPI_THREAD_FUNNELED) is available;
//   3. point-to-point messages work: a ping-pong between rank 0 and the last rank,
//      timed (the latency between nodes);
//   4. collectives work: MPI_Reduce sums the ranks (must be size*(size-1)/2) and
//      takes the maximum of a 64-bit value, as the final statistics will.
//
// Build:  mpicxx -O2 -std=c++17 hello.cpp -o mpi_hello
// Run:    srun --mpi=pmix ./mpi_hello   or   mpirun ./mpi_hello   (see hello.slurm)

#include <mpi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);

    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    char host[MPI_MAX_PROCESSOR_NAME] = {};
    int host_len = 0;
    MPI_Get_processor_name(host, &host_len);

    const char* cpus = std::getenv("SLURM_CPUS_PER_TASK");
    std::printf("rank %d of %d on %s: pid %d, %u hardware threads, SLURM_CPUS_PER_TASK=%s, thread level %s\n",
                rank, size, host, static_cast<int>(getpid()), std::thread::hardware_concurrency(),
                cpus ? cpus : "(unset)",
                provided >= MPI_THREAD_FUNNELED ? "FUNNELED or better (ok)" : "SINGLE (too low)");
    std::fflush(stdout);

    // Ping-pong between rank 0 and the last rank (on another node with one rank per node)
    if (size > 1) {
        const int other = size - 1;
        constexpr int ROUNDS = 1000;
        std::uint64_t token = 0;
        MPI_Barrier(MPI_COMM_WORLD);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < ROUNDS; ++i) {
            if (rank == 0) {
                ++token;
                MPI_Send(&token, 1, MPI_UINT64_T, other, 0, MPI_COMM_WORLD);
                MPI_Recv(&token, 1, MPI_UINT64_T, other, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            } else if (rank == other) {
                MPI_Recv(&token, 1, MPI_UINT64_T, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                ++token;
                MPI_Send(&token, 1, MPI_UINT64_T, 0, 0, MPI_COMM_WORLD);
            }
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (rank == 0) {
            std::printf("ping-pong with rank %d: %d round trips, token %llu (expect %d), %.1f us per round trip\n",
                        other, ROUNDS, static_cast<unsigned long long>(token), 2 * ROUNDS, us / ROUNDS);
        }
    }

    // Collectives, as the final statistics will use them
    long long rank_sum = 0;
    const long long mine = rank;
    MPI_Reduce(&mine, &rank_sum, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    std::uint64_t max_value = 0;
    const std::uint64_t value = 1000 + static_cast<std::uint64_t>(rank);
    MPI_Reduce(&value, &max_value, 1, MPI_UINT64_T, MPI_MAX, 0, MPI_COMM_WORLD);
    if (rank == 0) {
        const long long expect = static_cast<long long>(size) * (size - 1) / 2;
        std::printf("reduce: sum of ranks %lld (expect %lld) %s; max %llu (expect %d) %s\n",
                    rank_sum, expect, rank_sum == expect ? "ok" : "WRONG",
                    static_cast<unsigned long long>(max_value), 1000 + size - 1,
                    max_value == static_cast<std::uint64_t>(1000 + size - 1) ? "ok" : "WRONG");
    }

    MPI_Finalize();
    return 0;
}
