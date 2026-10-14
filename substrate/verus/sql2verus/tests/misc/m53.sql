SELECT sku, SUM(price * qty) * $1 AS scaled FROM prices GROUP BY sku ORDER BY sku
