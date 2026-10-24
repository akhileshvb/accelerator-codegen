-- point_insert / part, same-type duplicate: a second row under a key with in-window lineitems, keeping
-- the row's own p_type. Both sums fan out over the key, so the ratio moves through the denominator
-- alone; a kernel that keys parts by p_partkey without a multiplicity count answers wrong here.
INSERT INTO part
    (p_partkey, p_name, p_mfgr, p_brand, p_type, p_size, p_container, p_retailprice, p_comment)
SELECT p.p_partkey, p.p_name, p.p_mfgr, p.p_brand, p.p_type, p.p_size, p.p_container,
       p.p_retailprice, 'Q14DUP2'
FROM part p
WHERE p.p_partkey = (
    SELECT q.p_partkey FROM part q JOIN lineitem l ON l.l_partkey = q.p_partkey
     WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
       AND NOT (q.p_type LIKE 'PROMO%')
     ORDER BY q.p_partkey LIMIT 1)
ORDER BY p.p_partkey LIMIT 1;
