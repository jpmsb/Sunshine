#!/usr/bin/env bash
# Configure osc credentials for GitHub Actions.

set -euo pipefail

config_file="${HOME}/.config/osc/oscrc"
template="$(dirname "${BASH_SOURCE[0]}")/oscrc.template"

if [[ -z "${OBS_USER:-}" || -z "${OBS_PASSWORD:-}" ]]; then
  echo "ERROR: OBS_USER and OBS_PASSWORD must be set." >&2
  exit 1
fi

if [[ -e "${config_file}" ]]; then
  echo "ERROR: ${config_file} already exists." >&2
  exit 1
fi

mkdir -p "$(dirname "${config_file}")"

# Avoid sed delimiter issues with special characters in credentials.
python3 - "${template}" "${config_file}" <<'PY'
import os
import sys

template_path, config_path = sys.argv[1], sys.argv[2]
with open(template_path, encoding="utf-8") as handle:
    content = handle.read()
content = content.replace("@OBS_USER@", os.environ["OBS_USER"])
content = content.replace("@OBS_PASSWORD@", os.environ["OBS_PASSWORD"])
with open(config_path, "w", encoding="utf-8") as handle:
    handle.write(content)
PY
chmod 600 "${config_file}"

osc api /about >/dev/null
echo "OBS authentication verified for ${OBS_USER}"
