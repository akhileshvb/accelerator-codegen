-- Cancel the other suppliers' lines: a single-supplier order does not qualify.
DELETE FROM lineitem WHERE l_comment = 'q21:ordinary:line' AND l_linenumber > 1;
