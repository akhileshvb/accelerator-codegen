#!/usr/bin/env bash
#
# Download and extract SEC EDGAR Financial Statement Data Sets.
#
# Usage:
#   export SEC_EDGAR_USER_AGENT="YourName your-email@example.com"
#   bash eval/bench/sec_edgar/setup_data.sh [YEARS]
#
# SEC.gov requires a User-Agent identifying you with a contact email.
# Generic browser strings are rejected with HTTP 403.
# See: https://www.sec.gov/os/webmaster-faq#code-support

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YEARS="${1:-3}"
DATA_DIR="${SEC_EDGAR_DATA_DIR:-${SCRIPT_DIR}/data}"

END_YEAR=2024
START_YEAR=$((END_YEAR - YEARS + 1))

# SEC fair-access policy: include app name + contact email (not a browser default).
SEC_USER_AGENT="${SEC_EDGAR_USER_AGENT:-accelerator-codegen akhileshvb03@gmail.com}"

echo "=== SEC EDGAR Data Setup ==="
echo "Years: ${YEARS} (${START_YEAR}-${END_YEAR})"
echo "Data directory: ${DATA_DIR}"
echo "User-Agent: ${SEC_USER_AGENT}"

mkdir -p "${DATA_DIR}"

BASE_URL="https://www.sec.gov/files/dera/data/financial-statement-data-sets"
DOWNLOADED=0
FAILED=0

for year in $(seq "${START_YEAR}" "${END_YEAR}"); do
  for quarter in 1 2 3 4; do
    tag="${year}q${quarter}"
    zip_file="${DATA_DIR}/${tag}.zip"
    extract_dir="${DATA_DIR}/${tag}"

    if [[ -d "${extract_dir}" ]] && [[ -f "${extract_dir}/sub.txt" ]]; then
      echo "  ${tag}: already extracted, skipping download."
      DOWNLOADED=$((DOWNLOADED + 1))
      continue
    fi

    url="${BASE_URL}/${tag}.zip"
    echo "  Downloading ${tag}.zip..."
    if curl -fSL --retry 3 --retry-delay 10 \
      -H "User-Agent: ${SEC_USER_AGENT}" \
      -H "Accept-Encoding: gzip, deflate" \
      -H "Host: www.sec.gov" \
      -o "${zip_file}" "${url}"; then
      echo "  Extracting ${tag}.zip..."
      mkdir -p "${extract_dir}"
      unzip -o -q "${zip_file}" -d "${extract_dir}"
      rm -f "${zip_file}"
      DOWNLOADED=$((DOWNLOADED + 1))
    else
      echo "  WARNING: Failed to download ${tag}.zip" >&2
      rm -f "${zip_file}"
      FAILED=$((FAILED + 1))
    fi

    # Stay under SEC 10 requests/second limit.
    sleep 0.2
  done
done

echo ""
if [[ "${DOWNLOADED}" -eq 0 ]]; then
  echo "ERROR: No quarterly data available under ${DATA_DIR}." >&2
  echo "If every download returned 403, check SEC_EDGAR_USER_AGENT or wait ~10 minutes" >&2
  echo "(SEC may temporarily block an IP after rejected requests)." >&2
  exit 1
fi

echo "=== SEC EDGAR data ready (${DOWNLOADED} quarters, ${FAILED} failed) ==="
