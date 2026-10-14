INSERT INTO part
SELECT (part_base + offset_id)::INTEGER, 'replacement brass fitting', 'Manufacturer#2', 'Brand#12',
       'ECONOMY BRASS', 10, 'SM BOX', 125.00, 'new catalog item'
FROM q2_test_keys CROSS JOIN range(126,129) AS offsets(offset_id);
INSERT INTO partsupp
SELECT p_partkey, supp_base + 1, 250, 12.00, 'New stock'
FROM part CROSS JOIN q2_test_keys WHERE p_partkey BETWEEN part_base + 126 AND part_base + 128;
