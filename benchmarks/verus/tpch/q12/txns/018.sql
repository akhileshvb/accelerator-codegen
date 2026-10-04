UPDATE lineitem SET l_shipdate = DATE '1996-02-26', l_commitdate = DATE '1996-02-28',
  l_receiptdate = DATE '1996-02-29'
WHERE l_comment = 'q12_batch_line' AND l_linenumber % 2 = 0;
