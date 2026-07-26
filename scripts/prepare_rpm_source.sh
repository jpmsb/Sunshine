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

# Ensure NVENC headers exist when the full build-deps submodule is skipped.
# After LizardByte/Sunshine#5449, cmake includes:
#   third-party/build-deps/third-party/FFmpeg/nv-codec-headers/include
# Packaging excludes build-deps to keep tarballs small; without these headers the
# compiler silently picks older CUDA toolkit ffnvcodec headers and nvenc_base.cpp
# fails the NVENCAPI_VERSION == 13.0 guard.
function ensure_nv_codec_headers() {
  local dest="${source_root}/third-party/build-deps/third-party/FFmpeg/nv-codec-headers"
  local hdr="${dest}/include/ffnvcodec/nvEncodeAPI.h"
  if [[ -f "${hdr}" ]]; then
    local major minor
    major="$(sed -n 's/^#define NVENCAPI_MAJOR_VERSION[[:space:]]\+\([0-9]\+\).*/\1/p' "${hdr}" | head -1)"
    minor="$(sed -n 's/^#define NVENCAPI_MINOR_VERSION[[:space:]]\+\([0-9]\+\).*/\1/p' "${hdr}" | head -1)"
    if [[ "${major}" == "13" && "${minor}" == "0" ]]; then
      echo "nv-codec-headers ${major}.${minor} already present at ${dest}"
      return 0
    fi
    echo "nv-codec-headers present but ${major:-?}.${minor:-?} (need 13.0); replacing..."
    rm -rf "${dest}"
  fi

  echo "Cloning FFmpeg nv-codec-headers sdk/13.0 into packaging tree..."
  mkdir -p "$(dirname "${dest}")"
  git clone --depth 1 --branch sdk/13.0 \
    https://github.com/FFmpeg/nv-codec-headers.git "${dest}"
  # Drop VCS metadata from the injected clone (not needed in the RPM tarball).
  rm -rf "${dest}/.git"
  if [[ ! -f "${hdr}" ]]; then
    echo "ERROR: failed to populate nv-codec-headers at ${hdr}" >&2
    exit 1
  fi
}

# cmake/dependencies/ffmpeg.cmake resolves the prebuilt FFmpeg URL from the
# build-deps git tag. RPM tarballs exclude build-deps/.git, so without this stamp
# CMake used to fall back to "latest" (which can ship NVENC 13.1 and break the build).
function write_build_deps_ffmpeg_tag_stamp() {
  local stamp="${source_root}/packaging/linux/.sunshine-ffmpeg-tag"
  local bd="${source_root}/third-party/build-deps"
  local tag=""

  mkdir -p "$(dirname "${stamp}")"

  if [[ -d "${bd}/.git" || -f "${bd}/.git" ]]; then
    tag="$(git -C "${bd}" describe --tags --exact-match 2>/dev/null || true)"
    if [[ -z "${tag}" ]]; then
      tag="$(git -C "${bd}" describe --tags --abbrev=0 2>/dev/null || true)"
    fi
  fi

  if [[ -z "${tag}" ]]; then
    # Resolve the gitlink SHA recorded in Sunshine, then find a matching release tag
    # via the network only when the local submodule checkout is unavailable.
    local sha
    sha="$(git -C "${source_root}" rev-parse HEAD:third-party/build-deps 2>/dev/null || true)"
    if [[ -n "${sha}" ]]; then
      tag="$(git ls-remote --tags https://github.com/LizardByte/build-deps.git \
        | awk -v s="${sha}" '$1 == s { gsub(/^.*\//, "", $2); gsub(/\^\{\}$/, "", $2); print $2; exit }')"
    fi
  fi

  if [[ -z "${tag}" ]]; then
    echo "ERROR: could not resolve build-deps FFmpeg release tag for packaging stamp." >&2
    echo "Initialize third-party/build-deps or ensure network access to LizardByte/build-deps tags." >&2
    exit 1
  fi

  printf '%s\n' "${tag}" > "${stamp}"
  echo "Wrote FFmpeg packaging stamp: ${stamp} (${tag})"
}

# .copr-ci excludes the whole build-deps tree. Replace that blanket exclude with
# selective excludes so only nv-codec-headers (required by cmake) ships in the
# tarball — even when a full local build-deps checkout is present (~1GB+).
function refine_build_deps_excludes() {
  local -n _out=$1
  local filtered=()
  local path
  local has_build_deps_exclude=0
  for path in "${_out[@]}"; do
    if [[ "${path}" == "third-party/build-deps" ]]; then
      has_build_deps_exclude=1
      continue
    fi
    filtered+=("${path}")
  done
  _out=("${filtered[@]}")

  if [[ "${has_build_deps_exclude}" -eq 0 ]]; then
    return 0
  fi

  local bd="${source_root}/third-party/build-deps"
  if [[ ! -d "${bd}" ]]; then
    return 0
  fi

  echo "Refining third-party/build-deps excludes (keep nv-codec-headers only)"
  _out+=("third-party/build-deps/.git")

  local item base
  for item in "${bd}"/*; do
    [[ -e "${item}" ]] || continue
    base="$(basename "${item}")"
    if [[ "${base}" != "third-party" ]]; then
      _out+=("third-party/build-deps/${base}")
      echo "  Excluding: third-party/build-deps/${base}"
    fi
  done

  if [[ -d "${bd}/third-party" ]]; then
    for item in "${bd}/third-party"/*; do
      [[ -e "${item}" ]] || continue
      base="$(basename "${item}")"
      if [[ "${base}" != "FFmpeg" ]]; then
        _out+=("third-party/build-deps/third-party/${base}")
        echo "  Excluding: third-party/build-deps/third-party/${base}"
      fi
    done
  fi

  if [[ -d "${bd}/third-party/FFmpeg" ]]; then
    for item in "${bd}/third-party/FFmpeg"/*; do
      [[ -e "${item}" ]] || continue
      base="$(basename "${item}")"
      if [[ "${base}" != "nv-codec-headers" ]]; then
        _out+=("third-party/build-deps/third-party/FFmpeg/${base}")
        echo "  Excluding: third-party/build-deps/third-party/FFmpeg/${base}"
      fi
    done
  fi

  _out+=("third-party/build-deps/third-party/FFmpeg/nv-codec-headers/.git")
}

write_build_deps_ffmpeg_tag_stamp
ensure_nv_codec_headers
refine_build_deps_excludes excluded_paths

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
