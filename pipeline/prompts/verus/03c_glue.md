# Verus pipeline: the host glue ({{benchmark}} q{{query}})

Write `glue/glue.cpp`: the C++ that connects the engine to the kernel's C ABI
for accelerator `{{accel_id}}`. Read `contracts/host_glue.md` (it specifies
the file completely), `ref/kit/verus_kit.hpp`, `ref/kit/aux_types.hpp`,
`frozen/{{crate}}_verus.h`, `inputs/q.inputs` (what to deliver) and
`query/query.sql`. Include the header as `#include "{{crate}}_verus.h"` and
the kit as `#include "substrate/verus/kit/verus_kit.hpp"`.

This code is trusted: the kernel's proof assumes it delivers exactly the
declared inputs. Prefer plain, obviously-correct code to clever code.
Performance matters too (the build scans every declared row; maintenance runs
on every write): reserve vectors when you can, avoid per-cell copies you do not
need, but never at the cost of exactness.

Check with `verus_glue_check`: it builds the accelerator with your glue (the
kernel may still be a stub) and runs the contract check (in record mode, every
row your glue passes is compared with the declared SQL's results; mismatches
show the rows). Iterate until it passes.

Feedback from the previous round: {{feedback}}

{{include:shared/rules.md}}
