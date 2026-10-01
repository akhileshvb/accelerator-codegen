SELECT sku, price * qty AS v FROM prices WHERE qty <> 0 ORDER BY v DESC
