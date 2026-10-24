UPDATE lineitem
SET l_shipdate = DATE '1995-06-01',
    l_comment = 'SHIPDATE RESTORED 20261004'
WHERE l_comment = 'SHIPDATE EDGE 20261004';
