#!/bin/bash
# Build lpr in the current directory (a run directory made by scripts/submit.sh),
# on the node that will run it. Called by scripts/slurm/lpr.slurm when LPR_BUILD=1.
#
# 1. Find a compiler and LTO setting that can compile, link and run a small C++17
#    gmpxx test program, trying in order:
#      clang++ + LTO with lld;  g++ + -flto=auto (GCC >= 10);  g++ + -flto (older GCC);
#      clang++ without LTO;  g++ without LTO.
#    GMP is looked for in the system paths, in $GMP_PREFIX, then in $LPR_GMP_HOME. The test
#    exercises GMP's multiplication and powm kernels, so a GMP that crashes on this node's
#    CPU (built for a newer one) is skipped.
# 2. If no combination can link GMP, build GMP (static libraries, with the C++
#    wrapper; a fat binary on x86-64, so it runs on every node type sharing $HOME)
#    into $LPR_GMP_HOME from $LPR_GMP_TARBALL, downloading the tarball
#    first if it is missing and the node has internet access. Concurrent jobs
#    share one build (flock). Then retry step 1.
# 3. make release with the chosen settings, compiling in the commit ($LPR_COMMIT),
#    plus any extra flags ($LPR_CXXFLAGS). With LPR_PGO=1 the build is profile-guided:
#    an instrumented build, one training run ($LPR_PGO_TRAIN), then the final build
#    using the profile (clang needs llvm-profdata). See docs/optimisation_notes.md,
#    22/10/26: on the M2, clang + PGO was ~10% faster, GCC + PGO ~8% slower.
#
# A compiler is never installed: building GCC takes hours and itself needs GMP,
# MPFR and MPC. Set LPR_CXX to force a compiler (e.g. LPR_CXX=g++).
#
# Environment (all optional):
#   LPR_CXX          compiler to use instead of trying clang++ then g++
#   LPR_GMP_VERSION  GMP version to build if needed (default 6.3.0)
#   LPR_GMP_HOME     install prefix for a GMP built here (default $HOME/opt/gmp-<version>)
#   LPR_GMP_TARBALL  GMP source tarball (default $HOME/gmp-<version>.tar.xz)
#   GMP_PREFIX       an existing GMP installation to try (include/ and lib/ below it)
#   LPR_COMMIT       commit to compile into the run header
#   LPR_CXXFLAGS     extra compiler flags, e.g. "-march=znver3" (also used in the test compile)
#   LPR_PGO          1 for a profile-guided build
#   LPR_PGO_TRAIN    lpr arguments for the PGO training run (default "--single", OMEGA=33)
#   LPR_PROFDATA     llvm-profdata to use for clang PGO (default: from PATH, or xcrun on macOS)

set -euo pipefail

gmp_version=${LPR_GMP_VERSION:-6.3.0}
gmp_home=${LPR_GMP_HOME:-$HOME/opt/gmp-$gmp_version}
tarball=${LPR_GMP_TARBALL:-$HOME/gmp-$gmp_version.tar.xz}
gmp_url="https://ftp.gnu.org/gnu/gmp/gmp-$gmp_version.tar.xz"
jobs=${SLURM_CPUS_PER_TASK:-4}

log() { echo "[build_lpr] $*"; }

# Test program: C++17, gmpxx and threads as lpr uses them, and GMP's arithmetic kernels
# (multiplication at several sizes, squaring, division, modular exponentiation). A GMP
# built for a newer CPU than this node's dies here with "Illegal instruction" (e.g. ADX
# code on a Haswell node), so that GMP is skipped instead of crashing lpr later.
cat > .build_test.cpp <<'EOF'
#include <gmpxx.h>
#include <iostream>
#include <optional>
#include <thread>
int main() {
    int failures = 0;
    // Fermat test of the Mersenne prime 2^127 - 1 (as in lpr: powm modulo a few limbs)
    const mpz_class p = (mpz_class(1) << 127) - 1;
    mpz_class r;
    mpz_powm(r.get_mpz_t(), mpz_class(3).get_mpz_t(), mpz_class(p - 1).get_mpz_t(), p.get_mpz_t());
    failures += (r != 1);
    // A 190-bit modulus and exponent (lpr's candidate sizes)
    const mpz_class m = (mpz_class(1) << 189) + 1235, e = (mpz_class(1) << 188) + 77;
    mpz_powm(r.get_mpz_t(), mpz_class(5).get_mpz_t(), e.get_mpz_t(), m.get_mpz_t());
    failures += (r >= m);
    // Products from basecase to Toom/FFT sizes, checked by exact division
    for (unsigned bits : {64u, 192u, 2000u, 20000u, 200000u}) {
        const mpz_class a = (mpz_class(1) << bits) / 3 + 12345;
        const mpz_class b = (mpz_class(1) << (bits / 2 + 7)) / 7 + 1;
        const mpz_class c = a * b, sq = a * a;
        failures += (c / b != a) || (c % b != 0) || (sq / a != a);
    }
    std::optional<mpz_class> o = p + 1;
    std::thread t([] {});
    t.join();
    failures += (mpz_sizeinbase(o->get_mpz_t(), 2) != 128);
    std::cout << (failures ? "GMP self-check FAILED" : "GMP self-check passed") << '\n';
    return failures == 0 ? 0 : 1;
}
EOF

# Compiler candidates: "name|cxx|ltoflags|ldflags"
candidates=()
add() { command -v "$2" > /dev/null 2>&1 && candidates+=("$1|$2|$3|$4") || true; }
if [ -n "${LPR_CXX:-}" ]; then
    add "$LPR_CXX + -flto=auto" "$LPR_CXX" "-flto=auto" ""
    add "$LPR_CXX + -flto" "$LPR_CXX" "-flto" ""
    add "$LPR_CXX (no LTO)" "$LPR_CXX" "" ""
else
    command -v ld.lld > /dev/null 2>&1 && add "clang++ + LTO (lld)" clang++ "-flto=auto" "-fuse-ld=lld"
    add "g++ + -flto=auto" g++ "-flto=auto" ""
    add "g++ + -flto" g++ "-flto" ""
    add "clang++ (no LTO)" clang++ "" ""
    add "g++ (no LTO)" g++ "" ""
fi
[ ${#candidates[@]} -gt 0 ] || { log "no C++ compiler found (tried ${LPR_CXX:-clang++ and g++})"; exit 1; }

# GMP locations to try: system (empty prefix), GMP_PREFIX, our own build
gmp_prefixes() {
    echo ""
    [ -n "${GMP_PREFIX:-}" ] && echo "$GMP_PREFIX"
    [ -d "$gmp_home" ] && echo "$gmp_home"
    return 0
}

chosen=""
find_toolchain() {
    local prefix cand name cxx lto ld inc lib
    while IFS= read -r prefix; do
        for cand in "${candidates[@]}"; do
            IFS='|' read -r name cxx lto ld <<< "$cand"
            inc=""; lib=""
            [ -n "$prefix" ] && inc="-I$prefix/include" && lib="-L$prefix/lib -Wl,-rpath,$prefix/lib"
            if "$cxx" -std=c++17 -O1 $lto ${LPR_CXXFLAGS:-} $inc .build_test.cpp $ld $lib -lgmpxx -lgmp -pthread \
                   -o .build_test > .build_test.log 2>&1 && ( ./.build_test; exit $? ) > /dev/null 2>&1; then
                # (a subshell that is not a bare exec: a GMP that dies with SIGILL is expected on some nodes; this
                # keeps bash's "Illegal instruction" notice out of the job's .err file)
                chosen="$name|$cxx|$lto|$ld|$prefix"
                return 0
            fi
        done
    done < <(gmp_prefixes)
    return 1
}

build_gmp() {
    mkdir -p "$(dirname "$gmp_home")"
    # One build at a time per installation, and skip it if another job just finished one
    if command -v flock > /dev/null 2>&1; then
        exec 9> "$gmp_home.lock"
        log "waiting for the GMP build lock ($gmp_home.lock)"
        flock 9
    fi
    if [ -f "$gmp_home/include/gmpxx.h" ] && find_toolchain; then
        log "GMP was built by another job in the meantime"
        return 0
    fi
    if [ ! -f "$tarball" ]; then
        log "downloading $gmp_url"
        mkdir -p "$(dirname "$tarball")"
        if ! curl -fsSL --retry 3 -o "$tarball.part" "$gmp_url"; then
            rm -f "$tarball.part"
            log "cannot download GMP from this node (no internet access?)."
            log "Copy the source here first, e.g. from your own computer:"
            log "  curl -LO $gmp_url && scp gmp-$gmp_version.tar.xz <user>@<cluster>:$tarball"
            exit 1
        fi
        mv "$tarball.part" "$tarball"
    fi
    local cc cxx work
    cc=$(command -v gcc || command -v clang || true)
    cxx=$(command -v g++ || command -v clang++ || true)
    [ -n "$cc" ] && [ -n "$cxx" ] || { log "no C and C++ compiler to build GMP with"; exit 1; }
    # A "fat" build contains code for every x86-64 CPU family and picks one at run time,
    # so the shared ~/opt installation is safe on all node types (supported on x86 only)
    local fat=""
    case "$(uname -m)" in x86_64|i?86) fat="--enable-fat" ;; esac
    work="$PWD/.gmp_build"
    rm -rf "$work" && mkdir -p "$work"
    log "building GMP $gmp_version into $gmp_home (static, C++ wrapper${fat:+, fat binary}) using $cc / $cxx"
    tar -xf "$tarball" -C "$work"
    (
        cd "$work/gmp-$gmp_version"
        ./configure --prefix="$gmp_home" --enable-cxx --disable-shared $fat CC="$cc" CXX="$cxx" > configure.log 2>&1 \
            || { tail -20 configure.log; exit 1; }
        make -j"$jobs" > make.log 2>&1 || { tail -20 make.log; exit 1; }
        make check -j"$jobs" > check.log 2>&1 || { log "GMP self-tests FAILED:"; tail -30 check.log; exit 1; }
        make install > install.log 2>&1 || { tail -20 install.log; exit 1; }
    )
    rm -rf "$work"
    log "GMP $gmp_version built and its self-tests passed"
}

if ! find_toolchain; then
    log "no compiler could link GMP (last attempt: $(tail -1 .build_test.log 2>/dev/null))"
    build_gmp
    find_toolchain || { log "still cannot build the GMP test program:"; cat .build_test.log; exit 1; }
fi
rm -f .build_test .build_test.cpp .build_test.log

IFS='|' read -r name cxx lto ld prefix <<< "$chosen"
log "using: $name; GMP: ${prefix:-system}${LPR_CXXFLAGS:+; extra flags: $LPR_CXXFLAGS}"
# GMP_PREFIX and LDFLAGS go through the environment so the Makefile appends its own flags
# to them; CXX, LTOFLAGS and EXTRA_CXXFLAGS are make arguments.
export GMP_PREFIX="$prefix"
export LDFLAGS="${ld}${prefix:+ -Wl,-rpath,$prefix/lib}"
commit_tag=""                # "-pgo" for PGO builds, so the run header shows it
[ "${LPR_PGO:-0}" = 1 ] && commit_tag="-pgo"
build() {   # build ./lpr with extra flags $1 (on top of LPR_CXXFLAGS)
    rm -f lpr
    make release CXX="$cxx" LTOFLAGS="$lto" EXTRA_CXXFLAGS="${LPR_CXXFLAGS:-} $1" \
        ${LPR_COMMIT:+GIT_COMMIT=$LPR_COMMIT$commit_tag}
}

if [ "${LPR_PGO:-0}" != 1 ]; then
    build ""
    log "built ./lpr"
    exit 0
fi

# Profile-guided build: instrument, train, rebuild with the profile
train=${LPR_PGO_TRAIN:---single}
pgo_dir="$PWD/pgo_profile"
rm -rf "$pgo_dir" && mkdir -p "$pgo_dir"
if "$cxx" --version 2>/dev/null | grep -qi clang; then
    profdata=${LPR_PROFDATA:-$(command -v llvm-profdata || xcrun --find llvm-profdata 2>/dev/null || true)}
    [ -n "$profdata" ] || { log "PGO with clang needs llvm-profdata, which is not on PATH"; exit 1; }
    build "-fprofile-instr-generate"
    log "PGO training run: ./lpr $train"
    LLVM_PROFILE_FILE="$pgo_dir/train.profraw" ./lpr $train --log-interval 0 > "$pgo_dir/train.out" 2>&1 \
        || { log "PGO training run failed:"; tail -20 "$pgo_dir/train.out"; exit 1; }
    "$profdata" merge -output="$pgo_dir/train.profdata" "$pgo_dir/train.profraw"
    build "-fprofile-instr-use=$pgo_dir/train.profdata"
else
    # GCC names the profile files after the output, so both builds produce ./lpr
    build "-fprofile-generate -fprofile-dir=$pgo_dir"
    log "PGO training run: ./lpr $train"
    ./lpr $train --log-interval 0 > "$pgo_dir/train.out" 2>&1 \
        || { log "PGO training run failed:"; tail -20 "$pgo_dir/train.out"; exit 1; }
    build "-fprofile-use -fprofile-dir=$pgo_dir"
fi
log "built ./lpr (PGO, trained with: $train; training output in $pgo_dir/train.out)"
