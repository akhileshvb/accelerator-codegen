UPDATE lineitem SET l_shipdate = DATE '1992-02-11'
WHERE l_comment = 'q1-exact-duplicate';

UPDATE lineitem SET l_shipdate = l_shipdate + 1
WHERE l_comment IN ('q1-extreme-text', 'q1-setup-boundary', 'q1-setup-text');
