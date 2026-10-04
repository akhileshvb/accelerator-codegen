INSERT INTO customer
SELECT key_base.max_key + seed.ordinal, 'Q13 customer ' || seed.ordinal,
       '13 Market Street', template.c_nationkey, '13-555-0100',
       CAST(250.00 AS DECIMAL(15,2)), 'BUILDING', 'Q13 workload customer'
FROM (SELECT MAX(c_custkey) AS max_key FROM customer) key_base
CROSS JOIN range(1, 9) seed(ordinal)
CROSS JOIN (SELECT c_nationkey FROM customer ORDER BY c_custkey LIMIT 1) template;

INSERT INTO orders
SELECT key_base.max_key + ROW_NUMBER() OVER (ORDER BY customers.c_custkey, series.ordinal),
       customers.c_custkey, 'O', CAST(125.50 AS DECIMAL(15,2)), DATE '1995-03-15',
       '2-HIGH', 'Clerk#Q13', 0,
       CASE WHEN series.ordinal = 0 THEN 'unusual pending accounts'
            ELSE 'Routine customer order' END
FROM customer customers
CROSS JOIN range(0, 21) series(ordinal)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders) key_base
WHERE customers.c_name LIKE 'Q13 customer %'
  AND series.ordinal <= CASE customers.c_name
      WHEN 'Q13 customer 1' THEN 0 WHEN 'Q13 customer 2' THEN 1
      WHEN 'Q13 customer 3' THEN 2 WHEN 'Q13 customer 4' THEN 3
      WHEN 'Q13 customer 5' THEN 5 WHEN 'Q13 customer 6' THEN 8
      WHEN 'Q13 customer 7' THEN 12 WHEN 'Q13 customer 8' THEN 20 END;
