SELECT nation, o_year, SUM(amount) AS sum_profit
FROM accel_tpch_q9_matview
WHERE color = 'powder'
GROUP BY nation, o_year
ORDER BY nation, o_year DESC
