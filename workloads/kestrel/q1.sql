-- Kestrel Q1: revenue by region and item family for one region over a sale-date window; sold between $1 and $2,
-- region as $3.
SELECT s.region, i.family, COUNT(*) AS sales, SUM(l.qty * l.price) AS revenue, AVG(l.price) AS avg_price
FROM sale AS l
JOIN shop AS s ON s.shop_code = l.shop_code
JOIN item AS i ON i.sku = l.sku
WHERE l.sold BETWEEN $1 AND $2 AND s.region = $3
GROUP BY s.region, i.family
ORDER BY revenue DESC, i.family
