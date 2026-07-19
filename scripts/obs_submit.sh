#!/usr/bin/env bash
# Submit prepared Sunshine RPM sources to the Open Build Service (OBS).

set -euo pipefail

obs_project=""
obs_package="Sunshine"
version=""
branch="master"
commit=""
source_root=""
package_dir=""

usage() {
  cat <<'EOF'
Usage: obs_submit.sh [options]

Prepare sources and commit them to an OBS package.

Options:
  --obs-project PROJ   OBS project (e.g. home:LizardByte:sunshine) (required)
  --obs-package NAME   OBS package name (default: Sunshine)
  --version VER        Package version without leading "v" (required)
  --branch BRANCH      Source branch name (default: master)
  --commit SHA         Source commit hash (required)
  --source-root DIR    Repository root (default: parent of scripts/)
  --package-dir DIR    OBS checkout directory (default: ./obs-package)
  -h, --help           Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --obs-project)
      obs_project="$2"
      shift 2
      ;;
    --obs-package)
      obs_package="$2"
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
    --source-root)
      source_root="$2"
      shift 2
      ;;
    --package-dir)
      package_dir="$2"
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
if [[ -z "${package_dir}" ]]; then
  package_dir="${source_root}/obs-package"
fi

if [[ -z "${obs_project}" || -z "${version}" || -z "${commit}" ]]; then
  echo "ERROR: --obs-project, --version, and --commit are required." >&2
  usage >&2
  exit 1
fi

version="${version#v}"
rpm_topdir="$(mktemp -d)"
trap 'rm -rf "${rpm_topdir}"' EXIT

"${script_dir}/prepare_rpm_source.sh" \
  --output-dir "${rpm_topdir}" \
  --version "${version}" \
  --branch "${branch}" \
  --commit "${commit}" \
  --package-name "${obs_package}"

changes_file="${source_root}/packaging/linux/obs/${obs_package}.changes"
if [[ ! -f "${changes_file}" ]]; then
  echo "ERROR: changelog not found: ${changes_file}" >&2
  exit 1
fi

stamp="$(LC_ALL=C date -u '+%a %b %e %T %Z %Y')"
changelog_entry=$(
  cat <<EOF
-------------------------------------------------------------------
${stamp} - LizardByte <https://github.com/LizardByte>

- Update to version ${version} (${commit})

EOF
)

rm -rf "${package_dir}"
osc checkout --output-dir "${package_dir}" "${obs_project}" "${obs_package}"

shopt -s nullglob
rm -f "${package_dir}"/*.spec "${package_dir}"/*.tar.gz "${package_dir}"/*_service
shopt -u nullglob

cp "${rpm_topdir}/SPECS/${obs_package}.spec" "${package_dir}/"
cp "${rpm_topdir}/SOURCES/tarball.tar.gz" "${package_dir}/"
cp "${source_root}/packaging/linux/obs/_service" "${package_dir}/_service"

# OBS workers typically have no outbound network during %build. Disable the
# NVIDIA CUDA runfile download so the package can build without NVENC toolchain.
sed -i \
  's/^%bcond_without bundled_cuda 1$/%bcond_with bundled_cuda/' \
  "${package_dir}/${obs_package}.spec"

{
  printf '%s' "${changelog_entry}"
  sed -n '1,200p' "${changes_file}"
} > "${package_dir}/${obs_package}.changes"

(
  cd "${package_dir}"
  osc addremove
  osc commit -m "Sunshine ${version} (${commit})"
)

echo "Submitted ${obs_package} ${version} to ${obs_project}"
