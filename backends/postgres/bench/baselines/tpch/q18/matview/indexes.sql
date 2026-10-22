CREATE INDEX accel_tpch_q18_matview_thr_idx ON accel_tpch_q18_matview (quantity_threshold);
CREATE INDEX accel_tpch_q18_matview_price_idx ON accel_tpch_q18_matview (quantity_threshold, o_totalprice DESC);
