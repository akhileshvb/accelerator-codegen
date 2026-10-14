SELECT c_count, custdist
FROM accel_tpch_q13_matview
WHERE comment_pattern = '%unusual%accounts%'
ORDER BY custdist DESC, c_count DESC
