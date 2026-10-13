CREATE INDEX accel_tpch_q21_matview_nation_idx ON accel_tpch_q21_matview (n_name);
CREATE INDEX accel_tpch_q21_matview_wait_idx ON accel_tpch_q21_matview (n_name, numwait DESC);
