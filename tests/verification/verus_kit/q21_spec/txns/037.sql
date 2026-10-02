UPDATE supplier SET s_nationkey = (s_nationkey + 1) % 25 WHERE s_suppkey = (SELECT s_suppkey FROM supplier ORDER BY hash(s_suppkey + 37) LIMIT 1);
