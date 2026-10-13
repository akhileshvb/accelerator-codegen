SELECT supp_nation, cust_nation, l_year, revenue
FROM accel_tpch_q7_matview
WHERE (supp_nation = 'UNITED KINGDOM' AND cust_nation = 'BRAZIL')
   OR (supp_nation = 'BRAZIL' AND cust_nation = 'UNITED KINGDOM')
ORDER BY supp_nation, cust_nation, l_year
