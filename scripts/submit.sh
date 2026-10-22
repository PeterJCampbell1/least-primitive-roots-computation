#!/bin/bash
# Submit an lpr run from a fixed commit (run on the cluster's login node).
#
#   scripts/submit.sh [options] [-- sbatch options...]
#
# Steps:
#   1. update the repository: git pull --ff-only on the chosen branch (skip with --no-pull);
#   2. export the chosen commit (git archive) into a new per-run directory
#      $LPR_RUN_ROOT/<date>_<commit>[_<name>], so later pulls or edits cannot change a
#      queued or running job;
#   3. record the commit, branch and submission details in RUN_INFO in that directory;
#   4. submit scripts/slurm/lpr.slurm with the run directory as the working directory and
#      LPR_BUILD=1, so lpr is built on the compute node that runs it (with the commit
#      compiled in via LPR_COMMIT). The job's output, error and log files are written to
#      the run directory.
# The build (scripts/slurm/build_lpr.sh, copied into the run directory from this
# repository) runs in the job, never on the login node: it picks a compiler that can
# link GMP and, if there is no usable GMP, builds GMP into $HOME/opt (once, shared by
# later jobs). The first such job needs ~10 extra minutes; allow for it in --time.
#
# Options:
#   -b, --branch NAME   branch to pull and run (default: the repository's current branch)
#   -c, --commit REF    run this commit instead of the branch tip (any git revision)
#   -n, --name LABEL    label appended to the run directory name
#       --no-pull       do not run git pull first
#       --cxx NAME      compiler for the job's build (default: try clang++, then g++)
#       --gmp-tarball F GMP source tarball for a build on the compute node (default
#                       $HOME/gmp-6.3.0.tar.xz; downloaded by the job if missing and the
#                       node has internet access)
#       --dry-run       do everything except sbatch, and print the sbatch command
#   -h, --help          show this help
# Everything after "--" is passed to sbatch and overrides the #SBATCH lines in lpr.slurm,
# e.g.  scripts/submit.sh -n s29 -- --cpus-per-task=32 --time=04:00:00
#
# Environment:
#   LPR_REPO       repository (default: the one containing this script)
#   LPR_RUN_ROOT   where run directories are created (default: $SCRATCH/lpr_runs if
#                  SCRATCH is set, else $HOME/lpr_runs). Set it to your scratch space.
#   LPR_JOB        job script (default: $LPR_REPO/scripts/slurm/lpr.slurm). Fill in its
#                  <...> placeholders, or override them with sbatch options after "--".

set -euo pipefail

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

repo=${LPR_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
run_root=${LPR_RUN_ROOT:-${SCRATCH:-$HOME}/lpr_runs}
job_script=${LPR_JOB:-$repo/scripts/slurm/lpr.slurm}
branch="" commit="" name="" pull=1 dry_run=0 cxx="" gmp_tarball=""
sbatch_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--branch) branch=${2:?--branch needs a name}; shift 2 ;;
        -c|--commit) commit=${2:?--commit needs a revision}; shift 2 ;;
        -n|--name)   name=${2:?--name needs a label}; shift 2 ;;
        --no-pull)   pull=0; shift ;;
        --cxx)       cxx=${2:?--cxx needs a compiler}; shift 2 ;;
        --gmp-tarball) gmp_tarball=${2:?--gmp-tarball needs a file}; shift 2 ;;
        --dry-run)   dry_run=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        --)          shift; sbatch_args=("$@"); break ;;
        *)           echo "submit.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
done

git -C "$repo" rev-parse --git-dir > /dev/null
[ -f "$job_script" ] || { echo "submit.sh: job script '$job_script' not found" >&2; exit 2; }
if [ -n "$gmp_tarball" ]; then
    [ -f "$gmp_tarball" ] || { echo "submit.sh: GMP tarball '$gmp_tarball' not found" >&2; exit 2; }
    gmp_tarball="$(cd "$(dirname "$gmp_tarball")" && pwd)/$(basename "$gmp_tarball")"   # absolute path for the job
fi
branch=${branch:-$(git -C "$repo" rev-parse --abbrev-ref HEAD)}

# 1. Update the repository
if [ "$pull" = 1 ]; then
    current=$(git -C "$repo" rev-parse --abbrev-ref HEAD)
    if [ "$current" != "$branch" ]; then
        git -C "$repo" checkout "$branch"
    fi
    git -C "$repo" pull --ff-only
fi

# 2. Resolve the commit and export it into a new run directory
sha=$(git -C "$repo" rev-parse --verify "${commit:-$branch}^{commit}")
short=$(git -C "$repo" rev-parse --short "$sha")
run_dir="$run_root/$(date +%Y%m%d_%H%M%S)_${short}${name:+_$name}"
if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
    echo "submit.sh: note: the repository has uncommitted changes; they are NOT included (running $short)" >&2
fi
mkdir -p "$run_dir"
git -C "$repo" archive "$sha" | tar -x -C "$run_dir"
# The build script comes from this repository, so it also works for older commits
cp "$repo/scripts/slurm/build_lpr.sh" "$run_dir/build_lpr.sh"

# 3. Record what is being run
{
    echo "commit:    $sha"
    echo "branch:    $branch${commit:+ (commit given: $commit)}"
    echo "subject:   $(git -C "$repo" log -1 --format=%s "$sha")"
    echo "submitted: $(date '+%Y-%m-%d %H:%M:%S %Z') by ${USER:-unknown} on $(hostname)"
    echo "job file:  $job_script"
    echo "sbatch:    ${sbatch_args[*]:-}"
    echo "build:     compiler ${cxx:-auto}, GMP tarball ${gmp_tarball:-default}"
} > "$run_dir/RUN_INFO"

# 4. Submit, building lpr on the compute node inside the run directory.
# Build settings reach the job through --export=ALL.
if [ -n "$cxx" ]; then export LPR_CXX="$cxx"; fi
if [ -n "$gmp_tarball" ]; then export LPR_GMP_TARBALL="$gmp_tarball"; fi
# (sbatch options must come before the job script; "${a[@]+...}" keeps bash 3.2 happy with set -u)
cmd=(sbatch --parsable --chdir="$run_dir" --export=ALL,LPR_BUILD=1,LPR_COMMIT="$short"
     ${sbatch_args[@]+"${sbatch_args[@]}"} "$job_script")
if [ "$dry_run" = 1 ]; then
    echo "Dry run: would submit from $run_dir:"
    printf '  %q' "${cmd[@]}"; echo
    exit 0
fi
job_id=$("${cmd[@]}" | cut -d';' -f1)
echo "job id:    $job_id" >> "$run_dir/RUN_INFO"
echo "Submitted job $job_id: commit $short, run directory $run_dir"
