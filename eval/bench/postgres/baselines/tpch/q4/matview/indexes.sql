CREATE INDEX accel_tpch_q4_matview_odate_idx ON accel_tpch_q4_matview (o_orderdate);
CREATE INDEX accel_tpch_q4_matview_prio_idx ON accel_tpch_q4_matview (o_orderpriority, o_orderdate);
