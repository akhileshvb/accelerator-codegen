-- point_insert / part, duplicating the join key: a second part row under a p_partkey that exists in
-- part, has in-window lineitems, and whose rows are all non-PROMO, typed PROMO. The schema declares no
-- key, so this is a valid duplicate; it fans the key's joined rows out and adds them to the numerator,
-- so the answer must change. A kernel that assumes one part row per key answers wrong here. 010 removes
-- the duplicate again, so the pair must be symmetric.
INSERT INTO part
    (p_partkey, p_name, p_mfgr, p_brand, p_type, p_size, p_container, p_retailprice, p_comment)
SELECT p.p_partkey, p.p_name, p.p_mfgr, p.p_brand, 'PROMO Q14DUP', p.p_size, p.p_container,
       p.p_retailprice, 'Q14DUP'
FROM part p
WHERE p.p_partkey = (
    SELECT q.p_partkey FROM part q JOIN lineitem l ON l.l_partkey = q.p_partkey
     WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
       AND NOT (q.p_type LIKE 'PROMO%')
       AND NOT EXISTS (SELECT 1 FROM part r WHERE r.p_partkey = q.p_partkey AND r.p_type LIKE 'PROMO%')
     ORDER BY q.p_partkey LIMIT 1)
ORDER BY p.p_partkey LIMIT 1;
