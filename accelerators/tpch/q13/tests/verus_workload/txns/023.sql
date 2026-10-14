DELETE FROM orders WHERE o_custkey IN
    (SELECT c_custkey FROM customer WHERE c_name IN ('Q13 pending 3', 'Q13 pending 4', 'Q13 pending 5'));
DELETE FROM customer WHERE c_name IN ('Q13 pending 3', 'Q13 pending 4', 'Q13 pending 5');
UPDATE orders SET o_comment = 'unusual reconciliation accounts'
WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 4')
  AND o_comment NOT LIKE '%unusual%accounts%';
