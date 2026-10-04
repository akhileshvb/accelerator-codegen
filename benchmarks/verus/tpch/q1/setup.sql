INSERT INTO lineitem
SELECT template.* REPLACE (
    fixture.linenumber AS l_linenumber,
    7.25 AS l_quantity,
    1000.01 AS l_extendedprice,
    0.03 AS l_discount,
    0.07 AS l_tax,
    fixture.returnflag AS l_returnflag,
    fixture.linestatus AS l_linestatus,
    fixture.shipdate AS l_shipdate,
    fixture.shipdate AS l_commitdate,
    fixture.shipdate + 2 AS l_receiptdate,
    fixture.comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (VALUES
    (101, 'N', 'O', DATE '1998-10-01', 'q1-setup-boundary'),
    (102, 'N', 'O', DATE '1998-10-02', 'q1-setup-boundary'),
    (103, 'N', 'O', DATE '1998-10-03', 'q1-setup-boundary'),
    (104, 'N', 'O', DATE '1998-09-01', 'q1-setup-boundary'),
    (105, 'N', 'O', DATE '1998-09-02', 'q1-setup-boundary'),
    (106, 'N', 'O', DATE '1998-09-03', 'q1-setup-boundary'),
    (107, 'N', 'O', DATE '1998-08-02', 'q1-setup-boundary'),
    (108, 'N', 'O', DATE '1998-08-03', 'q1-setup-boundary'),
    (109, 'N', 'O', DATE '1998-08-04', 'q1-setup-boundary'),
    (110, 'n', 'O', DATE '1998-09-02', 'q1-setup-text'),
    (111, 'N', 'o', DATE '1998-09-02', 'q1-setup-text'),
    (112, ' ', 'F', DATE '1998-09-02', 'q1-setup-text'),
    (113, '', 'F', DATE '1998-09-02', 'q1-setup-text')
) AS fixture(linenumber, returnflag, linestatus, shipdate, comment);
