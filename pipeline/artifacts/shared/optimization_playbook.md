# Optimization playbook (seeded)

Cross-query experiment memory. Updated by orchestrator from `playbook_proposal.md`.

## dense_direct_index

**Worked:** Q3 (+12% SF1 typical), Q9, Q12, Q14  
**Skip if:** domain >90% sparse, non-integer keys

## dictionary_over_flat

**Worked:** when DICTIONARY→FLAT dominates lineitem/orders scans

## bitmap_membership

**Worked:** Q4 order-key membership

## Anti-patterns

- unordered_map for dense integer domains without SF10 evidence
- SF1 table-size constants in production path
