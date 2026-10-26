# Portable Makefile (macOS with Homebrew, or Linux).
#
#   make                 # lpr (release) and lpr_g (debug/profiling, asserts on)
#   make release | debug
#   make GMP_PREFIX=/path/to/gmp   # GMP installed outside the default search paths
#   make dfs_test | dyadic | interval_check   # standalone tools in utils/
#
# On a cluster, load a compiler and GMP first (e.g. `module load gcc gmp`), or
# point GMP_PREFIX at a local GMP build.

UNAME_S    ?= $(shell uname -s)
CXX        ?= g++
GIT_COMMIT := $(shell git describe --always --dirty 2>/dev/null || echo unknown)

CXXFLAGS   := -std=c++17 -Wall -Wextra -DLPR_GIT_COMMIT='"$(GIT_COMMIT)"'
LDLIBS     := -lgmpxx -lgmp
# GCC: parallel LTO link jobs (silences "using serial compilation of N LTRANS jobs").
# Clang treats -flto=auto as full LTO (-flto=full), the same as plain -flto.
LTOFLAGS   := -flto=auto

ifeq ($(UNAME_S),Darwin)
    # Homebrew GMP (Apple Silicon default /opt/homebrew)
    GMP_PREFIX ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
else
    # Linux: system or module GMP; std::thread needs -pthread
    CXXFLAGS += -pthread
    LDLIBS   += -pthread
endif

ifneq ($(strip $(GMP_PREFIX)),)
    # -isystem: GMP's headers are third-party, so their warnings are suppressed (e.g. clang 21's
    # -Wdeprecated-literal-operator in gmpxx.h); warnings in src/ still show
    CXXFLAGS += -isystem $(GMP_PREFIX)/include
    LDFLAGS  += -L$(GMP_PREFIX)/lib
endif

# Extra compiler flags, appended to the defaults instead of replacing them (a plain
# `make CXXFLAGS=...` would drop -std=c++17 and the GMP paths), e.g.
#   make EXTRA_CXXFLAGS=-march=znver3
CXXFLAGS += $(EXTRA_CXXFLAGS)
# Extra libraries, appended likewise (e.g. EXTRA_LDLIBS="-L/path/to/mpi/lib -lmpi")
LDLIBS += $(EXTRA_LDLIBS)

RELEASEFLAGS := -O3 -DNDEBUG
DEBUGFLAGS   := -O3 -g -fno-inline -fno-optimize-sibling-calls

SRCS := src/main.cpp src/primality.cpp src/corollary4.cpp
HDRS := $(wildcard src/*.hpp)

# Extra tooling
DYADIC_SRC         := utils/dyadic_bound.cpp
INTERVAL_CHECK_SRC := utils/interval_check.cpp
DFS_TEST_SRC       := utils/dfs_test.cpp

.PHONY: all release debug mpi mpi_debug dyadic interval_check clean

# Default target (make / make all) - builds lpr and lpr_g only
all: release debug

release: lpr
debug: lpr_g

# Release build: -O3 -DNDEBUG
lpr: $(SRCS) $(HDRS) Makefile
	$(CXX) $(RELEASEFLAGS) $(CXXFLAGS) $(LTOFLAGS) $(SRCS) $(LDFLAGS) $(LDLIBS) -o $@

# Debug / profiling build: -O3 -g, no inlining, asserts enabled
lpr_g: $(SRCS) $(HDRS) Makefile
	$(CXX) $(DEBUGFLAGS) $(CXXFLAGS) $(LTOFLAGS) $(SRCS) $(LDFLAGS) $(LDLIBS) -o $@

# MPI build (docs/mpi.md): lpr_mpi, with --mpi for one search across MPI ranks.
# Uses the MPI compiler wrapper by default; to build with another compiler, pass it plus
# the MPI flags, as scripts/slurm/build_lpr.sh does (LPR_TARGET=lpr_mpi), e.g.
#   make lpr_mpi MPICXX=clang++ EXTRA_CXXFLAGS=-I/usr/include/mpich-x86_64 \
#        EXTRA_LDLIBS="-L/usr/lib64/mpich/lib -Wl,-rpath,/usr/lib64/mpich/lib -lmpi"
MPICXX ?= mpicxx
mpi: lpr_mpi
mpi_debug: lpr_mpi_g

lpr_mpi: $(SRCS) $(HDRS) Makefile
	$(MPICXX) $(RELEASEFLAGS) $(CXXFLAGS) -DLPR_MPI $(LTOFLAGS) $(SRCS) $(LDFLAGS) $(LDLIBS) -o $@

lpr_mpi_g: $(SRCS) $(HDRS) Makefile
	$(MPICXX) $(DEBUGFLAGS) $(CXXFLAGS) -DLPR_MPI $(LTOFLAGS) $(SRCS) $(LDFLAGS) $(LDLIBS) -o $@

# Dyadic approximation helper utility (built only when explicitly requested: 'make dyadic')
dyadic:
	$(CXX) -O3 $(CXXFLAGS) $(DYADIC_SRC) $(LDFLAGS) $(LDLIBS) -o dyadic_bound

interval_check:
	$(CXX) -O3 $(CXXFLAGS) $(INTERVAL_CHECK_SRC) $(LDFLAGS) $(LDLIBS) -o interval_check

dfs_test: $(DFS_TEST_SRC) utils/parameters.hpp
	$(CXX) $(DEBUGFLAGS) $(CXXFLAGS) $(DFS_TEST_SRC) $(LDFLAGS) $(LDLIBS) -o dfs_test

clean:
	rm -f lpr lpr_g lpr_mpi lpr_mpi_g dyadic_bound interval_check dfs_test
	rm -rf lpr.dSYM lpr_g.dSYM dfs_test.dSYM
