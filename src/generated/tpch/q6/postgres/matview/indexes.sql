CREATE INDEX accel_tpch_q6_matview_ship_idx ON accel_tpch_q6_matview (l_shipdate);
CREATE INDEX accel_tpch_q6_matview_disc_qty_idx ON accel_tpch_q6_matview (l_discount, l_quantity);
CREATE INDEX accel_tpch_q6_matview_covering_idx ON accel_tpch_q6_matview (l_shipdate, l_discount, l_quantity);
