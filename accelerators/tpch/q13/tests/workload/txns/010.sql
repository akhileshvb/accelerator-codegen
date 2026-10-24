DELETE FROM orders WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 new 1');
DELETE FROM customer WHERE c_name = 'Q13 new 1';
