#!/bin/bash
# Submit an lpr run from a fixed commit (run on the cluster's login node).
#
#   scripts/submit.sh [options] [-- sbatch options...]
#
# Steps:
#   1. update the repository: git pull --ff-only on the chosen branch (skip with --no-pull);
#   2. export the chosen commit (git archive) into a new per-run directory
#      $LPR_RUN_ROOT/[<name>_]<date>_<commit>, so later pulls or edits cannot change a
#      queued or running job;
#   3. record the commit, branch and submission details in RUN_INFO in that directory;
#   4. submit scripts/slurm/lpr.slurm with the run directory as the working directory and
#      LPR_BUILD=1, so lpr is built on the compute node that runs it (with the commit
#      compiled in via LPR_COMMIT). The job's output, error and log files are written to
#      the run directory.
# The job saves checkpoints to lpr.ckpt in the run directory and, when stopped by the
# time limit, requeues itself until the search is complete (see scripts/slurm/lpr.slurm
# and docs/checkpointing.md). To continue a stopped run by hand, run resubmit.sh in its
# run directory (it resubmits the same job there; extra sbatch options may be given).
# The build (scripts/slurm/build_lpr.sh, copied into the run directory from this
# repository) runs in the job, never on the login node: it picks a compiler that can
# link GMP and, if there is no usable GMP, builds GMP into $HOME/opt (once, shared by
# later jobs). The first such job needs ~10 extra minutes; allow for it in --time.
#
# Options:
#   -b, --branch NAME   branch to pull and run (default: the repository's current branch)
#   -c, --commit REF    run this commit instead of the branch tip (any git revision)
#   -n, --name LABEL    label at the start of the run directory name
#   -a, --lpr-args "A"  lpr arguments for this run, replacing the job file's default
#                       "--config config/omega33.conf", e.g. "--omega 32 --s 29" or
#                       "--config config/omega33.conf --single". The job adds --parallel
#                       (work stealing) unless the arguments give a mode (--single,
#                       --parallel, --parallel-queue, --dry-run).
#       --no-pull       do not run git pull first
#       --cxx NAME      compiler for the job's build (default: try clang++, then g++)
#       --cxxflags "F"  extra compiler flags for the build, e.g. "-march=znver3"
#       --pgo           profile-guided build (instrumented build, training run, rebuild)
#       --pgo-train "A" lpr arguments for the PGO training run (default "--single")
#       --gmp-tarball F GMP source tarball for a build on the compute node (default
#                       $HOME/gmp-6.3.0.tar.xz; downloaded by the job if missing and the
#                       node has internet access)
#       --dry-run       do everything except sbatch, and print the sbatch command
#       --allow-in-job  run even inside a Slurm job (normally refused, see below)
#   -h, --help          show this help
# Run it on the login node. Inside a job (e.g. an srun --pty session) it stops: the
# job's environment and per-job directories would leak into the submitted job.
# Everything after "--" is passed to sbatch and overrides the #SBATCH lines in lpr.slurm,
# e.g.  scripts/submit.sh -n s29 -- --cpus-per-task=32 --time=04:00:00
#
# Environment:
#   LPR_REPO       repository (default: the one containing this script)
#   LPR_RUN_ROOT   where run directories are created (default: $HOME/lpr_runs). It must
#                  be on storage every compute node can see and that outlives jobs, e.g. a
#                  shared scratch filesystem. Not $SCRATCH inside a job: Slurm clusters often
#                  set that to a per-job directory that other nodes cannot see.
#   LPR_JOB        job script (default: $LPR_REPO/scripts/slurm/lpr.slurm). Fill in its
#                  <...> placeholders, or override them with sbatch options after "--".

set -euo pipefail

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

repo=${LPR_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
run_root=${LPR_RUN_ROOT:-$HOME/lpr_runs}
job_script=${LPR_JOB:-$repo/scripts/slurm/lpr.slurm}
branch="" commit="" name="" pull=1 dry_run=0 allow_in_job=0
lpr_args="" cxxflags="" pgo=0 pgo_train="" cxx="" gmp_tarball=""
sbatch_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--branch) branch=${2:?--branch needs a name}; shift 2 ;;
        -c|--commit) commit=${2:?--commit needs a revision}; shift 2 ;;
        -n|--name)   name=${2:?--name needs a label}; shift 2 ;;
        -a|--lpr-args) lpr_args=${2:?--lpr-args needs arguments}; shift 2 ;;
        --cxxflags)  cxxflags=${2:?--cxxflags needs flags}; shift 2 ;;
        --pgo)       pgo=1; shift ;;
        --pgo-train) pgo_train=${2:?--pgo-train needs arguments}; shift 2 ;;
        --no-pull)   pull=0; shift ;;
        --cxx)       cxx=${2:?--cxx needs a compiler}; shift 2 ;;
        --gmp-tarball) gmp_tarball=${2:?--gmp-tarball needs a file}; shift 2 ;;
        --dry-run)   dry_run=1; shift ;;
        --allow-in-job) allow_in_job=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        --)          shift; sbatch_args=("$@"); break ;;
        *)           echo "submit.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
done

# Refuse to run inside a Slurm job: per-job paths (such as $SCRATCH there) and the job's
# SLURM_* environment would be passed to the new job, which may run on another node
if [ -n "${SLURM_JOB_ID:-}" ]; then
    if [ "$allow_in_job" = 0 ]; then
        echo "submit.sh: this shell is inside Slurm job $SLURM_JOB_ID (on $(hostname))." >&2
        echo "  Run submit.sh on the login node instead (exit the srun session first)," >&2
        echo "  or pass --allow-in-job if you are sure LPR_RUN_ROOT ($run_root) is shared." >&2
        exit 2
    fi
    echo "submit.sh: warning: running inside Slurm job $SLURM_JOB_ID (--allow-in-job)" >&2
fi
case "$run_root/" in
    *"/${SLURM_JOB_ID:-no-job-id}/"*)
        echo "submit.sh: LPR_RUN_ROOT ($run_root) is inside this job's own directory," >&2
        echo "  which other nodes cannot see and which is removed when the job ends." >&2
        exit 2 ;;
esac

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
run_dir="$run_root/${name:+${name}_}$(date +%Y%m%d_%H%M%S)_${short}" # [<name>_]<date>_<commit>
if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
    echo "submit.sh: note: the repository has uncommitted changes; they are NOT included (running $short)" >&2
fi
mkdir -p "$run_dir"
git -C "$repo" archive "$sha" | tar -x -C "$run_dir"
# The build script comes from this repository, so it also works for older commits
cp "$repo/scripts/slurm/build_lpr.sh" "$run_dir/build_lpr.sh"
# The job file as submitted, so resubmit.sh uses the same one even if it is edited later
cp "$job_script" "$run_dir/job.slurm"

# 3. Record what is being run
{
    echo "commit:    $sha"
    echo "branch:    $branch${commit:+ (commit given: $commit)}"
    echo "subject:   $(git -C "$repo" log -1 --format=%s "$sha")"
    echo "submitted: $(date '+%Y-%m-%d %H:%M:%S %Z') by ${USER:-unknown} on $(hostname)"
    echo "job file:  $job_script"
    echo "sbatch:    ${sbatch_args[*]:-}"
    echo "lpr args:  ${lpr_args:-(job file default)}"
    echo "build:     compiler ${cxx:-auto}, extra flags ${cxxflags:-none}, PGO $( [ "$pgo" = 1 ] && echo "on (training: ${pgo_train:---single})" || echo off ), GMP tarball ${gmp_tarball:-default}"
} > "$run_dir/RUN_INFO"

# 4. Submit, building lpr on the compute node inside the run directory.
# Build settings reach the job through --export=ALL.
if [ -n "$cxx" ]; then export LPR_CXX="$cxx"; fi
if [ -n "$lpr_args" ]; then export LPR_ARGS="$lpr_args"; fi
if [ -n "$cxxflags" ]; then export LPR_CXXFLAGS="$cxxflags"; fi
if [ "$pgo" = 1 ]; then export LPR_PGO=1; fi
if [ -n "$pgo_train" ]; then export LPR_PGO_TRAIN="$pgo_train"; fi
if [ -n "$gmp_tarball" ]; then export LPR_GMP_TARBALL="$gmp_tarball"; fi
# (sbatch options must come before the job script; "${a[@]+...}" keeps bash 3.2 happy with set -u)
cmd=(sbatch --parsable --chdir="$run_dir" --export=ALL,LPR_BUILD=1,LPR_COMMIT="$short"
     ${sbatch_args[@]+"${sbatch_args[@]}"})
# resubmit.sh: the same submission, to continue a stopped run from its lpr.ckpt
{
    echo '#!/bin/bash'
    echo "# Continue this run: resubmits its job in this directory, where lpr resumes from"
    echo "# lpr.ckpt. Extra sbatch options may be given, e.g. resubmit.sh --time=08:00:00"
    echo 'set -euo pipefail'
    for var in LPR_CXX LPR_ARGS LPR_CXXFLAGS LPR_PGO LPR_PGO_TRAIN LPR_GMP_TARBALL; do
        if [ -n "${!var:-}" ]; then printf 'export %s=%q\n' "$var" "${!var}"; fi
    done
    printf '%q ' "${cmd[@]}"; printf '"$@" %q\n' "$run_dir/job.slurm"
} > "$run_dir/resubmit.sh"
chmod +x "$run_dir/resubmit.sh"
cmd+=("$run_dir/job.slurm")
if [ "$dry_run" = 1 ]; then
    echo "Dry run: would submit from $run_dir:"
    printf '  %q' "${cmd[@]}"; echo
    exit 0
fi
job_id=$("${cmd[@]}" | cut -d';' -f1)
echo "job id:    $job_id" >> "$run_dir/RUN_INFO"
echo "Submitted job $job_id: commit $short, run directory $run_dir"
