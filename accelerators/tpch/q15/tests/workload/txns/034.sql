UPDATE supplier SET s_suppkey = (SELECT MAX(s_suppkey) + 1 FROM supplier)
WHERE s_comment = 'q15-edge-b';
