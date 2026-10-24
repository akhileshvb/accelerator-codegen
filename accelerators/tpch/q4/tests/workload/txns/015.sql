UPDATE lineitem SET l_receiptdate = l_commitdate + 1
WHERE l_comment BETWEEN 'q4:seed:05' AND 'q4:seed:08' AND l_linenumber = 1;
