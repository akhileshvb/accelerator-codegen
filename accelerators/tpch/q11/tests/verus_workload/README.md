# Q11 differential workload

`params.sql` covers all 25 TPC-H nations and small, empty, byte-sensitive,
and adversarial answers. Only the nation name is a parameter: the query and
generated spec fix the fraction at 0.0001, despite workload.json's extra entry.

`setup.sql` adds ordinary depot inventories, a two-part small market, an exact
HAVING boundary market, duplicate bags on all three relations, and facts that
initially lack either their supplier or their nation. All identifiers for new
rows are derived from SQL; comments identify the controlled fixtures.

Transactions 001–030 model replenishment, repricing, stock withdrawal, opening
and closing depots/markets, and supplier relocation. They include point writes,
small batches, a 20-row insertion, larger updates, mixed-table writes, and
reversals. Transactions 031–040 exercise strict HAVING boundaries, negative
totals and zero-valued groups, ties, multiplicative duplicates and individual
duplicate deletion, dormant joins, supplier rescans with concurrent fact
deltas, group/join-key changes, byte-sensitive names, full group removal and
recreation, and integer/decimal extrema. Schema-valid negative quantities and
costs deliberately test semantics beyond normal TPC-H distributions.

In the boundary market, the initial total is exactly 10000.00, so 0.99, 1.00,
and 1.01 groups bracket the strict threshold. Transaction 031 lowers the total
to 9999.99 without changing the 1.00 group, making it enter the result. Later
transactions drive the total negative, huge, back to its initial value, and
finally to zero. Zero-valued groups must exist independently of their sum.
Transactions 033 and 034 remove and recreate a zero-valued group while the
total is negative, so its membership changes even though no sum changes.

Transactions commit in filename order. No temporary objects or previously
captured row identifiers are needed across connections or snapshots.
