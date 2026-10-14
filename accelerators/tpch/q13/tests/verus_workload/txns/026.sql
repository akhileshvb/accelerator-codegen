DELETE FROM customer WHERE rowid = (SELECT MIN(rowid) FROM customer WHERE c_name = 'Q13 customer 4');
DELETE FROM orders WHERE rowid = (
    SELECT MIN(rowid) FROM orders
    WHERE o_orderkey IN (SELECT o_orderkey FROM orders GROUP BY o_orderkey HAVING COUNT(*) > 1));
UPDATE customer SET c_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 5')
WHERE c_name = 'Q13 customer 4';
UPDATE orders SET o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 5')
WHERE o_custkey NOT IN (SELECT c_custkey FROM customer);
