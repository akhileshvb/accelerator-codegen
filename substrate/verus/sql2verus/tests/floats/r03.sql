SELECT s.store FROM stores s WHERE s.target < (SELECT SUM(a.amount) FROM sales a WHERE a.store = s.store)
