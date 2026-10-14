UPDATE orders SET o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 1')
WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 2');
DELETE FROM customer WHERE c_name = 'Q13 customer 2';
