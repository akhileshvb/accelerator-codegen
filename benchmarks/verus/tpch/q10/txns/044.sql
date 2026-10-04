INSERT INTO nation
SELECT (SELECT c_nationkey FROM customer WHERE c_name = 'Q10-03'),
       'q10 Federation ', n_regionkey, 'Restored missing join partner'
FROM nation ORDER BY n_nationkey LIMIT 1;
INSERT INTO customer
SELECT (SELECT o_custkey FROM orders WHERE o_comment = 'Q10-24'), 'Q10-24', 'Restored customer address',
       (SELECT n_nationkey FROM nation WHERE n_name = 'q10 Federation '), '10-100-100-1000',
       -9999999999999.99, 'BUILDING', 'Restored missing customer';
