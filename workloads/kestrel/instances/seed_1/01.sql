SELECT s.region, i.family, COUNT(*) AS sales, SUM(l.qty * l.price) AS revenue, AVG(l.price) AS avg_price
FROM sale AS l
JOIN shop AS s ON s.shop_code = l.shop_code
JOIN item AS i ON i.sku = l.sku
WHERE l.sold BETWEEN DATE '2024-03-01' AND DATE '2024-05-31' AND s.region = 'north'
GROUP BY s.region, i.family
ORDER BY revenue DESC, i.family;
