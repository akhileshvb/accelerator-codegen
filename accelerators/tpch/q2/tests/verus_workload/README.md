# Q2 differential workload

The query has two parameters: integer part size and region name. The workload
JSON also lists the literal type pattern; it is not a third query parameter.

Setup adds 126 catalog parts, two suppliers, and a linked nation/region with
keys derived from the live database. Size 15 / EUROPE initially has more than
100 winners, exercising replacement across the LIMIT boundary. Persistent
fixture copies support delete/reinsert cycles without NULL-producing lookups.
The winners view selects actual regional minimum-cost offers in the live data.

Transactions 001–040 are catalog maintenance: point and batch repricing,
supplier contacts and balances, manufacturer and size changes, material
classification, onboarding/deletion, geography corrections, and mixed-table
writes. Transactions 041–060 cover schema-valid edge states: bag duplication
and partial removal, equal minima and ordering ties, decimal/integer limits,
negative key changes, LIKE case/suffix/spacing, empty minima, disconnected
quotes, delayed joins, and first/last BRASS membership at a duplicated key.
Transactions 061–072 resume ordinary catalog maintenance after the edge
states: quote revisions, contacts, batch size/manufacturer updates, redo of
catalog insert/delete cycles, supplier reactivation, and geography corrections.
There are 52 ordinary maintenance transactions and 20 edge transactions.
All five dependent tables receive inserts, updates, and deletes. Unrelated
orders, lineitem, and customer tables are deliberately untouched.

Ordering ties introduced at 043 have fewer than 100 distinct qualifying rows
per tested size; duplicate rows at 041 are identical in every output column.
This avoids asserting a particular member of an ambiguous LIMIT boundary tie.
The pipeline supplies commit and concurrent-snapshot checks; transaction files
contain only statements, not explicit BEGIN/COMMIT commands.
