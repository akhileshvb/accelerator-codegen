-- A second supplier of that order delivers late too: neither is the only late supplier any more.
UPDATE lineitem SET l_receiptdate = DATE '1996-01-20'
WHERE l_comment = 'q21:ordinary:line' AND l_linenumber = 2;
