INSERT INTO customer
SELECT 0, 'Q13 text cases', c_address, c_nationkey, c_phone, 0.00,
       c_mktsegment, 'Case sensitive literal pattern tests'
FROM customer WHERE c_name = 'Q13 customer 1';
INSERT INTO orders
SELECT key_base.max_key + ROW_NUMBER() OVER (ORDER BY patterns.ordinal),
       customers.c_custkey, 'O', 10.00, DATE '1995-03-26', '3-MEDIUM',
       'Clerk#Q13', 0, patterns.comment
FROM customer customers
CROSS JOIN (VALUES
    (1, ''), (2, 'unusualaccounts'), (3, 'unusual accounts'),
    (4, '  unusual  accounts  '), (5, 'Unusual accounts'),
    (6, 'unusual Accounts'), (7, 'UNUSUAL ACCOUNTS'),
    (8, 'accounts unusual'), (9, 'unusual account'),
    (10, 'un usual accounts'), (11, 'unusual%accounts'),
    (12, 'unusual_accounts'), (13, 'unusual' || chr(10) || 'accounts'),
    (14, 'unusualéaccounts'), (15, 'accounts unusual accounts'),
    (16, REPEAT('x', 64) || 'unusualaccounts')) patterns(ordinal, comment)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders WHERE o_orderkey < 2147483647) key_base
WHERE customers.c_name = 'Q13 text cases';
INSERT INTO customer
SELECT c_custkey, 'Q13 orphan restored', c_address, c_nationkey, c_phone,
       c_acctbal, c_mktsegment, c_comment
FROM customer WHERE c_name = 'Q13 customer 1';
UPDATE customer SET c_custkey = (
    SELECT MIN(o_custkey) FROM orders WHERE o_custkey NOT IN (SELECT c_custkey FROM customer))
WHERE c_name = 'Q13 orphan restored';
