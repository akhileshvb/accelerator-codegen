UPDATE supplier SET s_name = s_name || 'x' WHERE s_suppkey = (SELECT s_suppkey FROM supplier ORDER BY hash(s_suppkey + 28) LIMIT 1);
