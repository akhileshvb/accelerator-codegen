-- point_insert / part with a key beyond 2^32 (the SF3000 key width class): a fresh part row typed
-- PROMO. On its own it joins nothing, so it changes no answer; 017 writes the lineitems that use it.
INSERT INTO part
    (p_partkey, p_name, p_mfgr, p_brand, p_type, p_size, p_container, p_retailprice, p_comment)
VALUES (4294967311, 'q14 big key', 'Manufacturer#1', 'Brand#11', 'PROMO Q14BIG', 1, 'SM BOX',
        1000.00, 'Q14BIG');
