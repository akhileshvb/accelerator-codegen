UPDATE lineitem SET l_shipdate = l_commitdate - 1
WHERE l_comment = 'q12_new_a_line' AND l_linenumber = 3;
