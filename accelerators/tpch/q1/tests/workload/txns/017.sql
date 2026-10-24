INSERT INTO lineitem
SELECT template.* REPLACE (
    3 AS l_linenumber,
    18.50 AS l_quantity,
    1950.01 AS l_extendedprice,
    0.04 AS l_discount,
    0.06 AS l_tax,
    'N' AS l_returnflag,
    'O' AS l_linestatus,
    DATE '1998-09-01' AS l_shipdate,
    DATE '1998-09-01' AS l_commitdate,
    DATE '1998-09-04' AS l_receiptdate
)
FROM (SELECT * FROM lineitem WHERE l_comment = 'q1-new-small' AND l_linenumber = 2) AS template;
