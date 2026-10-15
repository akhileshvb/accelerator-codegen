CREATE INDEX accel_tpch_q1_matview_shipdate_idx ON accel_tpch_q1_matview (l_shipdate);
CREATE INDEX accel_tpch_q1_matview_flag_idx ON accel_tpch_q1_matview (l_returnflag, l_linestatus, l_shipdate);
