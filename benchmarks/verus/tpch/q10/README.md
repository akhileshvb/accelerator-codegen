# Q10 differential workload

The SQL has two date parameters, despite workload.json listing only the start
date. Every tuple supplies both endpoints explicitly. Most tuples are quarterly
TPC-H windows; additional tuples cover broad, one-day, empty, reversed, and
out-of-domain windows. Text is not a parameter of this query, so case, whitespace,
and UTF-8 tests are writes instead.

Setup adds 24 complete customer/order groups with two returned lines and one
non-returned line apiece. Their single-day window has more than 20 groups and
no original returned orders, giving a controlled top-20 boundary. Keys are
allocated from the database; all columns, including unused NOT NULL columns,
are populated. Original data remains available for ordinary quarterly windows.

Transactions 001–032 are ordinary point and batch writes, including complete
order creation/deletion in both parent-first and child-first order, return
processing, price/discount adjustments, date corrections, customer reassignment,
dimension maintenance, top-20 promotion/demotion, and multi-table changes.

Transactions 033–044 are the smaller adversarial portion: exact duplicates in
all four tables, deletion of only one bag copy, orphan lines across commits,
the last returned-line disappearance/reappearance, case-sensitive flags, text
changes, coordinated key changes, ties across the LIMIT boundary, zero/tiny/
negative revenues, maximum DECIMAL(15,2) prices, and missing/recreated dimension
partners. The schema has no keys or discount CHECK constraints. The zero and
negative groups are visible in the December 31 window, not hidden by LIMIT.

Files execute in numeric order with a commit between files. No file starts or
commits a transaction itself. The pipeline supplies differential and snapshot
checks; these tests never call an accelerator or encode expected answers.
