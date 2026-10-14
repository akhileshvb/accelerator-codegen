INSERT INTO lineitem (
    l_orderkey, l_partkey, l_suppkey, l_linenumber,
    l_quantity, l_extendedprice, l_discount, l_tax,
    l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate,
    l_shipinstruct, l_shipmode, l_comment
)
SELECT
    (SELECT MIN(o_orderkey) FROM orders),
    (SELECT MIN(p_partkey) FROM part),
    (SELECT MIN(s_suppkey) FROM supplier),
    fixture.linenumber,
    fixture.quantity, fixture.price, fixture.discount, fixture.tax,
    fixture.returnflag, fixture.linestatus, fixture.shipdate,
    fixture.shipdate, fixture.shipdate + 2,
    'DELIVER IN PERSON', 'TRUCK', 'q1-repopulate'
FROM (VALUES
    (1, 'A', 'F', DATE '1992-02-12', 1.01, 100.01, 0.03, 0.02),
    (2, 'R', 'F', DATE '1992-02-12', 2.02, 200.02, 0.07, 0.08),
    (3, 'N', 'F', DATE '1998-08-03', 3.03, 300.03, 0.01, 0.04),
    (4, 'N', 'O', DATE '1998-09-02', 4.04, 400.04, 0.05, 0.06),
    (5, 'n', 'O', DATE '1998-09-03', 5.05, 500.05, 0.09, 0.03),
    (6, ' ', 'F', DATE '1998-10-02', 6.06, 600.06, 0.00, 0.00)
) AS fixture(linenumber, returnflag, linestatus, shipdate, quantity, price, discount, tax);
