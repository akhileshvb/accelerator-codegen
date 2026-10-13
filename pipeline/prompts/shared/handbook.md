# Optimization handbook (MVP single file)

## dense_direct_index

Use when: integer keys, bounded domain (known at bind time, non-negative, within a sane cap), hash probe dominates; otherwise use sparse `prims/` primitives.

Prior successes: Q3, Q9, Q12, Q14.

## dictionary_over_flat

Use when: DICTIONARY→FLAT layouts dominate profile.

## bitmap_membership

Use when: set membership or tiny classification.

Prior successes: Q4, Q9.

## Anti-patterns (retry only with different measured profile)

- `unordered_map` for dense bounded domains (and node-based maps / per-row `std::string` anywhere: use `prims/` columnar primitives)
- Narrow key packing (32-bit keys, bit-fields, fixed dense caps) and per-call rebuild for un-precomputed parameters
- SF1-hardcoded capacities
- Full-domain finalization scans
- Ambient env vars on production path
