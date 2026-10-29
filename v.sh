#!/bin/bash
# v.sh [extra]: verify Q3 crate; prints errors + result
cd /flash1/akhilvb/vsec2/c3; export TMPDIR=/dev/shm ACCEL_VERUS_LIB_DIR=/flash1/akhilvb/vsec2/c3/build-verus
Q=accelerators/sec_edgar/q3/verus_kit/sec_edgar_q3_vgen
scripts/build_verus_crate.sh $Q/crate sec_edgar_q3_vgen --verify-only "$@" > /dev/shm/c3_v.log 2>&1
grep -A12 "^error" /dev/shm/c3_v.log | grep -v "^warning" | head -${LINES_MAX:-80}
grep "verification results\|^built" /dev/shm/c3_v.log
