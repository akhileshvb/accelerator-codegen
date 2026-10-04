INSERT INTO supplier
SELECT (SELECT min(ps_suppkey) FROM partsupp WHERE ps_comment = 'v16 blocked edge'),
       'Supplier#V16-07', s_address, s_nationkey, s_phone, 100.00,
       'v16 Customer service Complaints reopened'
FROM supplier ORDER BY s_suppkey LIMIT 1;
