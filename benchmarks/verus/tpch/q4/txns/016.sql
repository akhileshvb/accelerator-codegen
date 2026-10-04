DELETE FROM lineitem
WHERE l_comment IN ('q4:seed:09', 'q4:seed:10') AND l_commitdate < l_receiptdate;
