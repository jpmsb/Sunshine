#!/usr/bin/env bash
# Wait for an OBS package build and download binary RPMs.

set -euo pipefail

obs_project=""
obs_package="Sunshine"
repository="openSUSE_Tumbleweed"
architectures=()
output_dir=""
timeout_minutes=90
poll_seconds=30

usage() {
  cat <<'EOF'
Usage: obs_download_rpms.sh [options]

Poll OBS build results and download RPM binaries.

Options:
  --obs-project PROJ     OBS project (required)
  --obs-package NAME     OBS package name (default: Sunshine)
  --repository REPO      OBS repository (default: openSUSE_Tumbleweed)
  --arch ARCH            Target architecture (repeatable, default: x86_64 aarch64)
  --output-dir DIR       Directory for downloaded RPMs (required)
  --timeout-minutes MIN  Max wait time (default: 90)
  --poll-seconds SEC     Poll interval (default: 30)
  -h, --help             Show this help
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
    --repository)
      repository="$2"
      shift 2
      ;;
    --arch)
      architectures+=("$2")
      shift 2
      ;;
    --output-dir)
      output_dir="$2"
      shift 2
      ;;
    --timeout-minutes)
      timeout_minutes="$2"
      shift 2
      ;;
    --poll-seconds)
      poll_seconds="$2"
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

if [[ "${#architectures[@]}" -eq 0 ]]; then
  architectures=("x86_64" "aarch64")
fi

if [[ -z "${obs_project}" || -z "${output_dir}" ]]; then
  echo "ERROR: --obs-project and --output-dir are required." >&2
  usage >&2
  exit 1
fi

mkdir -p "${output_dir}"

deadline=$((SECONDS + timeout_minutes * 60))
declare -A arch_status=()

wait_for_builds() {
  while ((SECONDS < deadline)); do
    local all_done=true
    local any_failed=false

    for arch in "${architectures[@]}"; do
      local status
      status="$(
        osc results "${obs_project}" "${obs_package}" \
          -r "${repository}" \
          -a "${arch}" \
          --csv 2>/dev/null | tail -n 1 | cut -d\; -f4
      )"

      arch_status["${arch}"]="${status:-unknown}"
      echo "OBS ${repository}/${arch}: ${arch_status[${arch}]}"

      case "${arch_status[${arch}]}" in
        succeeded)
          continue
          ;;
        failed | unresolvable | broken)
          any_failed=true
          ;;
        *)
          all_done=false
          ;;
      esac
    done

    if [[ "${any_failed}" == true ]]; then
      echo "ERROR: OBS build failed." >&2
      osc results "${obs_project}" "${obs_package}" -r "${repository}" || true
      exit 1
    fi

    if [[ "${all_done}" == true ]]; then
      return 0
    fi

    sleep "${poll_seconds}"
  done

  echo "ERROR: Timed out waiting for OBS builds after ${timeout_minutes} minutes." >&2
  exit 1
}

download_arch() {
  local arch="$1"

  (
    cd "${output_dir}"
    osc getbinaries \
      --destdir . \
      "${obs_project}" \
      "${obs_package}" \
      "${repository}" \
      "${arch}"
  )

  find "${output_dir}" -type f -name "*.rpm" ! -name "*.src.rpm" -print
}

wait_for_builds

for arch in "${architectures[@]}"; do
  download_arch "${arch}"
done

echo "Downloaded OBS RPMs to ${output_dir}"
