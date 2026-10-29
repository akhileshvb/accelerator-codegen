#!/bin/bash
# vf.sh module function [extra]: verify one function
cd /flash1/akhilvb/vsec2/c3; O=/flash1/akhilvb/vsec2/c3/build-verus; export TMPDIR=/dev/shm
m=$1; f=$2; shift 2
tools/verus/verus-x86-linux/verus accelerators/sec_edgar/q3/verus_kit/sec_edgar_q3_vgen/crate/src/lib.rs --crate-type=staticlib --crate-name=x_verus --import verus_common=$O/verus_common.vir --extern verus_common=$O/libverus_common.rlib --import verus_substrate=$O/verus_substrate.vir --extern verus_substrate=$O/libverus_substrate.rlib --import verus_prooflib=$O/verus_prooflib.vir --extern verus_prooflib=$O/libverus_prooflib.rlib -L dependency=$O --multiple-errors 8 --verify-only-module $m --verify-function $f "$@" 2>&1 | grep -v "^warning\|^ *|$\|^$\|trigger\|^note\|^ *(\|help:" | grep -A12 "^error\|verification results"
