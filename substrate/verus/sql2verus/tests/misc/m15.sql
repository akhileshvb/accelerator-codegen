SELECT sku, SUM(price * qty) / SUM(qty) AS avgp FROM prices GROUP BY sku ORDER BY sku
