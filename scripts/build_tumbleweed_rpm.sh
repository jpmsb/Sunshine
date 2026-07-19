#!/usr/bin/env bash
# Build a Sunshine RPM on openSUSE Tumbleweed using packaging/linux/copr/Sunshine.spec.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_root="$(cd "${script_dir}/.." && pwd)"

package_name="Sunshine"
rpm_topdir="${HOME}/rpmbuild"
output_dir=""
version=""
branch=""
commit=""
install_deps=1
run_check=1
sudo_cmd="sudo"
force_distro=0
skip_cuda_runfile=0

usage() {
  cat <<'EOF'
Usage: build_tumbleweed_rpm.sh [options]

Build Sunshine RPM packages on openSUSE Tumbleweed.

Options:
  --output-dir DIR     Copy built RPMs to this directory (default: ./artifacts)
  --rpm-topdir DIR     rpmbuild topdir (default: ~/rpmbuild)
  --version VER        Package version without leading "v" (default: from git)
  --branch BRANCH      Source branch (default: current git branch)
  --commit SHA         Source commit (default: HEAD)
  --install-deps       Install BuildRequires with zypper (default)
  --skip-deps          Do not install BuildRequires
  --skip-cuda-runfile  Do not download the NVIDIA CUDA runfile (use system nvcc or build without CUDA)
  --nocheck            Skip spec %%check (unit tests)
  --sudo-off           Do not use sudo for zypper
  --force              Allow running outside openSUSE Tumbleweed
  -h, --help           Show this help

Examples:
  ./scripts/build_tumbleweed_rpm.sh
  ./scripts/build_tumbleweed_rpm.sh --output-dir /tmp/sunshine-rpms
  ./scripts/build_tumbleweed_rpm.sh --skip-cuda-runfile
  ./scripts/build_tumbleweed_rpm.sh --skip-deps --nocheck
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      output_dir="$2"
      shift 2
      ;;
    --rpm-topdir)
      rpm_topdir="$2"
      shift 2
      ;;
    --version)
      version="$2"
      shift 2
      ;;
    --branch)
      branch="$2"
      shift 2
      ;;
    --commit)
      commit="$2"
      shift 2
      ;;
    --install-deps)
      install_deps=1
      shift
      ;;
    --skip-deps)
      install_deps=0
      shift
      ;;
    --skip-cuda-runfile)
      skip_cuda_runfile=1
      shift
      ;;
    --nocheck)
      run_check=0
      shift
      ;;
    --sudo-off)
      sudo_cmd=""
      shift
      ;;
    --force)
      force_distro=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

function is_tumbleweed() {
  if [[ ! -f /etc/os-release ]]; then
    return 1
  fi
  grep -q 'openSUSE Tumbleweed' /etc/os-release 2>/dev/null || \
    grep -q 'VERSION_ID="rolling"' /etc/os-release 2>/dev/null
}

function require_command() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "ERROR: required command not found: ${cmd}" >&2
    exit 1
  fi
}

function git_metadata() {
  if [[ -z "${branch}" ]]; then
    branch="$(git -C "${source_root}" rev-parse --abbrev-ref HEAD)"
  fi
  if [[ -z "${commit}" ]]; then
    commit="$(git -C "${source_root}" rev-parse HEAD)"
  fi
  if [[ -z "${version}" ]]; then
    if git -C "${source_root}" describe --tags --abbrev=0 >/dev/null 2>&1; then
      version="$(git -C "${source_root}" describe --tags --abbrev=0)"
    else
      version="0.0.$(git -C "${source_root}" rev-parse --short HEAD)"
    fi
  fi
  version="${version#v}"
}

function init_submodules() {
  if [[ ! -f "${source_root}/.gitmodules" ]]; then
    return 0
  fi

  local excluded_paths=()
  if [[ -f "${source_root}/.copr-ci" ]]; then
    while IFS= read -r line || [[ -n "${line}" ]]; do
      [[ -z "${line}" || "${line}" =~ ^[[:space:]]*# ]] && continue
      excluded_paths+=("${line}")
    done < "${source_root}/.copr-ci"
  fi

  echo "Initializing git submodules..."
  if [[ ${#excluded_paths[@]} -eq 0 ]]; then
    git -C "${source_root}" submodule update --init --recursive
    return 0
  fi

  mapfile -t top_submodules < <(git -C "${source_root}" submodule status | awk '{print $2}')
  for submodule in "${top_submodules[@]}"; do
    local skip=false
    for excluded in "${excluded_paths[@]}"; do
      if [[ "${submodule}" == "${excluded}" || "${submodule}" == "${excluded}/"* ]]; then
        skip=true
        break
      fi
    done
    if [[ "${skip}" == true ]]; then
      echo "Skipping submodule: ${submodule}"
      continue
    fi
    git -C "${source_root}" submodule update --init --recursive --depth 1 -- "${submodule}"
  done
}

function get_installed_rpm_names() {
  local -n _out=$1
  while IFS= read -r name; do
    [[ -n "${name}" ]] && _out["${name}"]=1
  done < <(rpm -qa --qf "%{NAME}\n")
}

function build_require_package_name() {
  # First token is the RPM name; version constraints follow (e.g. "cmake >= 3.25.0").
  awk '{print $1}' <<<"$1"
}

function find_missing_build_requires() {
  local -n _requires=$1
  local -n _missing=$2
  local -A installed=()
  local -a extra_tools=(rpm-build rpmlint)
  local req name

  _missing=()
  get_installed_rpm_names installed

  for req in "${extra_tools[@]}" "${_requires[@]}"; do
    name="$(build_require_package_name "${req}")"
    if [[ -z "${installed[${name}]:-}" ]]; then
      _missing+=("${name}")
    fi
  done
}

function install_build_requires() {
  local spec_file="${rpm_topdir}/SPECS/${package_name}.spec"
  local -a build_requires=()
  local -a missing_requires=()

  echo "Resolving BuildRequires from ${spec_file}..."
  mapfile -t build_requires < <(rpmspec -q --buildrequires "${spec_file}")

  if [[ ${#build_requires[@]} -eq 0 ]]; then
    echo "ERROR: no BuildRequires resolved from spec." >&2
    exit 1
  fi

  find_missing_build_requires build_requires missing_requires
  if [[ ${#missing_requires[@]} -eq 0 ]]; then
    echo "All ${#build_requires[@]} BuildRequires (plus rpm-build and rpmlint) are already installed; skipping zypper."
    return 0
  fi

  echo "Installing ${#missing_requires[@]} missing build dependencies with zypper..."
  echo "  Missing: ${missing_requires[*]}"
  if [[ -n "${sudo_cmd}" ]]; then
    ${sudo_cmd} zypper --non-interactive refresh
    ${sudo_cmd} zypper --non-interactive install -y --no-recommends \
      rpm-build rpmlint "${build_requires[@]}"
  else
    zypper --non-interactive refresh
    zypper --non-interactive install -y --no-recommends \
      rpm-build rpmlint "${build_requires[@]}"
  fi
}

function copy_artifacts() {
  local dest="$1"
  mkdir -p "${dest}"
  find "${rpm_topdir}/RPMS" -type f -name "*.rpm" ! -name "*.src.rpm" -print0 |
    while IFS= read -r -d '' rpm_file; do
      cp -v "${rpm_file}" "${dest}/"
    done
}

if [[ "${force_distro}" -eq 0 ]] && ! is_tumbleweed; then
  echo "ERROR: this script is intended for openSUSE Tumbleweed." >&2
  echo "Use --force to override." >&2
  exit 1
fi

require_command git
require_command rpm
require_command rpmbuild
require_command rpmspec
require_command zypper
require_command tar

if [[ -z "${output_dir}" ]]; then
  output_dir="${source_root}/artifacts"
fi

git_metadata
init_submodules

echo "Building Sunshine RPM for openSUSE Tumbleweed"
echo "  version: ${version}"
echo "  branch:  ${branch}"
echo "  commit:  ${commit}"
echo "  topdir:  ${rpm_topdir}"
if [[ "${skip_cuda_runfile}" -eq 1 ]]; then
  echo "  cuda:    skip runfile download (system nvcc or build without CUDA)"
fi

"${script_dir}/prepare_rpm_source.sh" \
  --output-dir "${rpm_topdir}" \
  --version "${version}" \
  --branch "${branch}" \
  --commit "${commit}" \
  --package-name "${package_name}" \
  --source-root "${source_root}"

if [[ "${install_deps}" -eq 1 ]]; then
  install_build_requires
fi

rpmbuild_args=(
  -ba
  --define "_topdir ${rpm_topdir}"
  # Match docs/GitHub release naming: Sunshine-{version}-1.tw.{arch}.rpm
  --define "dist .tw"
)
if [[ "${run_check}" -eq 0 ]]; then
  rpmbuild_args+=(--nocheck)
fi
if [[ "${skip_cuda_runfile}" -eq 1 ]]; then
  rpmbuild_args+=(--without bundled_cuda)
fi
rpmbuild_args+=("${rpm_topdir}/SPECS/${package_name}.spec")

echo "Running rpmbuild..."
rpmbuild "${rpmbuild_args[@]}"

if command -v rpmlint >/dev/null 2>&1; then
  echo "Linting built RPMs..."
  rpmlint_config="${source_root}/packaging/linux/copr/Sunshine.rpmlintrc"
  find "${rpm_topdir}/RPMS" -type f -name "*.rpm" ! -name "*.src.rpm" -print0 |
    while IFS= read -r -d '' rpm_file; do
      if [[ -f "${rpmlint_config}" ]]; then
        rpmlint -r "${rpmlint_config}" "${rpm_file}" || true
      else
        rpmlint "${rpm_file}" || true
      fi
    done
fi

copy_artifacts "${output_dir}"

echo "Build complete. RPM artifacts:"
ls -la "${output_dir}"/*.rpm
