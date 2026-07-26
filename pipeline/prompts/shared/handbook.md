# Optimization handbook (MVP single file)

## dense_direct_index

Use when: integer keys, bounded domain, hash probe dominates.

Prior successes: Q3, Q9, Q12, Q14.

## dictionary_over_flat

Use when: DICTIONARY→FLAT layouts dominate profile.

## bitmap_membership

Use when: set membership or tiny classification.

Prior successes: Q4, Q9.

## Anti-patterns (retry only with different measured profile)

- `unordered_map` for dense bounded domains
- SF1-hardcoded capacities
- Full-domain finalization scans
- Ambient env vars on production path
