UPDATE lineitem SET l_shipdate = l_receiptdate - 2, l_commitdate = l_receiptdate - 1
WHERE l_comment = 'q12_new_b_line';
