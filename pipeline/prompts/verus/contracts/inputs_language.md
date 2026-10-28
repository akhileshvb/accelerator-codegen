# Kernel inputs (inputs/q.inputs)

The kernel never reads tables itself. You declare what the host hands it, and
sql2verus turns each declaration into a contract (frozen, trusted) that the
kernel's proof may assume. Statements end with `;`, `--` starts a comment.

    scan <name> [AS <Type>]: SELECT <columns> FROM <table> [<alias>] [WHERE ...];
    rescan <name> AS <Type> KEYS <column>: SELECT <columns> FROM <table> [<alias>] [WHERE ...];
    delta <table> [AS <Type>] [OF <RowType>]: SELECT <expressions> FROM <table>;

Rules (sql2verus enforces them; `verus_check_inputs` shows its verdict):

* **scan**: one base table, plain columns (named by their SELECT names),
  filtered by plain predicates and `[NOT] EXISTS` subqueries; no parameters,
  no aggregation, no joins. The host delivers exactly the query's rows as a
  bag. A scan is read at build time.
* **rescan**: a scan restricted to rows whose `KEYS` column (which must be one
  of the SELECT columns) is in a key list the kernel computes during apply
  (`rescan_<name>_keys`, which the host passes back to apply unchanged). It reads the database **after** the transaction.
  Use it when a change needs rows the state does not keep (e.g. a parent
  becomes relevant and its child rows were never tracked). The key may be
  integer-like or text (`Vec<u8>`, compared as bytes), and NULL-able (a row
  whose key is NULL matches no key). Cost: one keyed query per transaction
  that needs it.
* **delta**: every table the query reads has exactly one delta: the
  transaction's changes to it as signed rows (+1 insert, -1 delete; an update
  is both). A *lossless* delta selects exactly the columns the spec reads of
  that table (`OF <RowType>` names its row type; with `AS` it is a struct
  `{row, sign}`, else a `(row, i64)` pair). A *lossy* delta (fewer columns, or
  computed ones) is a struct `AS <Type>` of its fields plus `sign`; computed
  fields may only compare columns with literals (`o_orderstatus = 'F'`,
  `AND`/`OR`/`NOT`), because the host glue computes them.
* **types**: integer -> i64, DECIMAL(p,s) -> i64 (value × 10^s), DATE -> i32
  (days since 1970-01-01), TIMESTAMP -> i64 (ns), text -> Vec<u8>, boolean ->
  u8, DOUBLE -> u64 (its IEEE-754 bits, finite; the spec value is
  `verus_common::rel::f64_val`: sum exactly, round once). A row type without
  text is `#[repr(C)]` and passed as a slice (cheap); one with text is passed
  column by column (a Vec of rows).
* **NULL**: a column the schema does not declare NOT NULL (or PRIMARY KEY)
  may hold NULL, and NULL is inside the envelope: the kernel must handle it.
  Its field carries a flag `<field>__null` (nonzero: NULL, the value is then
  unspecified), so a row type with one needs `AS <Type>`. A scan's WHERE keeps
  a row only where its predicate is true (a comparison with NULL is false). A
  lossy delta's fields must be NOT NULL: a computed field over a NULL-able
  column is refused (use a lossless delta, which carries the flags).
* Without an inputs file the default is every table scanned whole and every
  change captured whole: always valid, rarely fast.

What you choose here decides the build cost (scans), the maintenance cost
(deltas, rescans) and what the kernel may rely on. Keep scans to what the
state needs; prefer lossy deltas that drop text you never read; add a rescan
only when a delta alone cannot keep the state right.
