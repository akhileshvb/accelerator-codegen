UPDATE lineitem SET l_commitdate = l_receiptdate - 1
WHERE l_comment BETWEEN 'q4:seed:01' AND 'q4:seed:04' AND l_linenumber = 1;
