SELECT sku, price FROM prices WHERE price > (SELECT AVG(price) FROM prices) ORDER BY sku, price
