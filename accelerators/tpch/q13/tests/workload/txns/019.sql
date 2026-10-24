INSERT INTO customer
SELECT key_base.max_key + seed.ordinal, 'Q13 pending ' || seed.ordinal,
       '35 Warehouse Lane', template.c_nationkey, '13-555-0300', 0.00,
       'HOUSEHOLD', 'Awaiting first purchase'
FROM (SELECT MAX(c_custkey) AS max_key FROM customer) key_base
CROSS JOIN range(1, 6) seed(ordinal)
CROSS JOIN (SELECT c_nationkey FROM customer ORDER BY c_custkey LIMIT 1) template;
