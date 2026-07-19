#!/usr/bin/env bash
# Prepare RPM source tree (spec + tarball) for Sunshine.
# Shared logic with LizardByte/copr-ci copr-ci.sh for COPR, OBS, and local builds.

set -euo pipefail

package_name="Sunshine"
output_dir="${HOME}/rpmbuild"
version=""
branch="master"
commit=""
source_root=""

usage() {
  cat <<'EOF'
Usage: prepare_rpm_source.sh [options]

Prepare Sunshine.spec and source tarball for rpmbuild.

Options:
  --output-dir DIR   RPM topdir (default: ~/rpmbuild)
  --version VER      Package version without leading "v" (required)
  --branch BRANCH    Source branch name (default: master)
  --commit SHA       Source commit hash (required)
  --package-name NM  RPM package name (default: Sunshine)
  --source-root DIR  Repository root (default: script parent/..)
  -h, --help         Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      output_dir="$2"
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
    --package-name)
      package_name="$2"
      shift 2
      ;;
    --source-root)
      source_root="$2"
      shift 2
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "${source_root}" ]]; then
  source_root="$(cd "${script_dir}/.." && pwd)"
fi

if [[ -z "${version}" || -z "${commit}" ]]; then
  echo "ERROR: --version and --commit are required." >&2
  usage >&2
  exit 1
fi

version="${version#v}"

spec_file=""
directories=(
  "${source_root}"
  "${source_root}/packaging/linux/copr"
)
for dir in "${directories[@]}"; do
  if [[ -f "${dir}/${package_name}.spec" ]]; then
    spec_file="${dir}/${package_name}.spec"
    break
  fi
done

if [[ -z "${spec_file}" ]]; then
  echo "ERROR: ${package_name}.spec not found under ${source_root}" >&2
  exit 1
fi

echo "Using spec file: ${spec_file}"

function read_copr_ci_excludes() {
  local -n _out=$1
  local copr_ci_file="${source_root}/.copr-ci"
  if [[ ! -f "${copr_ci_file}" ]]; then
    return 0
  fi

  echo "Reading exclusions from .copr-ci"
  while IFS= read -r line || [[ -n "${line}" ]]; do
    [[ -z "${line}" || "${line}" =~ ^[[:space:]]*# ]] && continue
    _out+=("${line}")
    echo "  Excluding: ${line}"
  done < "${copr_ci_file}"
}

function default_tar_excludes() {
  local -n _out=$1
  local defaults=(
    ".git"
    ".cursor"
    "node_modules"
    "build"
    "artifacts"
    "rpmbuild"
    "obs-package"
    "sunshine_state.json"
    "test_sunshine.log"
  )
  _out+=("${defaults[@]}")
}

excluded_paths=()
default_tar_excludes excluded_paths
read_copr_ci_excludes excluded_paths

mkdir -p \
  "${output_dir}/BUILD" \
  "${output_dir}/BUILDROOT" \
  "${output_dir}/RPMS" \
  "${output_dir}/SOURCES" \
  "${output_dir}/SPECS" \
  "${output_dir}/SRPMS"

tar_excludes=(
  "--exclude=./cmake-build-*"
  "--exclude=./write_file_test_*"
)
for path in "${excluded_paths[@]}"; do
  tar_excludes+=("--exclude=./${path}")
done

echo "Creating source tarball..."
start_time=${SECONDS}
(
  cd "${source_root}"
  if command -v pigz >/dev/null 2>&1; then
    # Fast parallel gzip (pigz -1)
    tar "${tar_excludes[@]}" -cf - . | pigz -1 > "${output_dir}/SOURCES/tarball.tar.gz"
  else
    # Fastest single-threaded gzip level
    GZIP=-1 tar -czf "${output_dir}/SOURCES/tarball.tar.gz" "${tar_excludes[@]}" .
  fi
)
elapsed=$((SECONDS - start_time))
tarball_size="$(du -h "${output_dir}/SOURCES/tarball.tar.gz" | awk '{print $1}')"
echo "Created tarball.tar.gz (${tarball_size}) in ${elapsed}s"

cp "${spec_file}" "${output_dir}/SPECS/${package_name}.spec"

sed -i "s|%global build_version 0|%global build_version ${version}|" \
  "${output_dir}/SPECS/${package_name}.spec"
sed -i "s|%global branch 0|%global branch ${branch}|" \
  "${output_dir}/SPECS/${package_name}.spec"
sed -i "s|%global commit 0|%global commit ${commit}|" \
  "${output_dir}/SPECS/${package_name}.spec"

if command -v rpmlint >/dev/null 2>&1; then
  rpmlint "${output_dir}/SPECS/${package_name}.spec" || true
fi

echo "Prepared RPM sources in ${output_dir}"
echo "  SPECS/${package_name}.spec"
echo "  SOURCES/tarball.tar.gz"
