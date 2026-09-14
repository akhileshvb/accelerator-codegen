DELIMITER //

DROP TRIGGER IF EXISTS accel_codegen_tpch_lineitem_i//
CREATE TRIGGER accel_codegen_tpch_lineitem_i AFTER INSERT ON lineitem
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('1', '0', NEW.l_orderkey, NEW.l_partkey, NEW.l_suppkey, NEW.l_linenumber, NEW.l_quantity, NEW.l_extendedprice, NEW.l_discount, NEW.l_tax, NEW.l_returnflag, NEW.l_linestatus, NEW.l_shipdate, NEW.l_commitdate, NEW.l_receiptdate, NEW.l_shipinstruct, NEW.l_shipmode, NEW.l_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_lineitem_d//
CREATE TRIGGER accel_codegen_tpch_lineitem_d AFTER DELETE ON lineitem
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('1', '1', OLD.l_orderkey, OLD.l_partkey, OLD.l_suppkey, OLD.l_linenumber, OLD.l_quantity, OLD.l_extendedprice, OLD.l_discount, OLD.l_tax, OLD.l_returnflag, OLD.l_linestatus, OLD.l_shipdate, OLD.l_commitdate, OLD.l_receiptdate, OLD.l_shipinstruct, OLD.l_shipmode, OLD.l_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_lineitem_u//
CREATE TRIGGER accel_codegen_tpch_lineitem_u AFTER UPDATE ON lineitem
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('1', '2', OLD.l_orderkey, OLD.l_partkey, OLD.l_suppkey, OLD.l_linenumber, OLD.l_quantity, OLD.l_extendedprice, OLD.l_discount, OLD.l_tax, OLD.l_returnflag, OLD.l_linestatus, OLD.l_shipdate, OLD.l_commitdate, OLD.l_receiptdate, OLD.l_shipinstruct, OLD.l_shipmode, OLD.l_comment, NEW.l_orderkey, NEW.l_partkey, NEW.l_suppkey, NEW.l_linenumber, NEW.l_quantity, NEW.l_extendedprice, NEW.l_discount, NEW.l_tax, NEW.l_returnflag, NEW.l_linestatus, NEW.l_shipdate, NEW.l_commitdate, NEW.l_receiptdate, NEW.l_shipinstruct, NEW.l_shipmode, NEW.l_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_orders_i//
CREATE TRIGGER accel_codegen_tpch_orders_i AFTER INSERT ON orders
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('2', '0', NEW.o_orderkey, NEW.o_custkey, NEW.o_orderstatus, NEW.o_totalprice, NEW.o_orderdate, NEW.o_orderpriority, NEW.o_clerk, NEW.o_shippriority, NEW.o_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_orders_d//
CREATE TRIGGER accel_codegen_tpch_orders_d AFTER DELETE ON orders
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('2', '1', OLD.o_orderkey, OLD.o_custkey, OLD.o_orderstatus, OLD.o_totalprice, OLD.o_orderdate, OLD.o_orderpriority, OLD.o_clerk, OLD.o_shippriority, OLD.o_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_orders_u//
CREATE TRIGGER accel_codegen_tpch_orders_u AFTER UPDATE ON orders
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('2', '2', OLD.o_orderkey, OLD.o_custkey, OLD.o_orderstatus, OLD.o_totalprice, OLD.o_orderdate, OLD.o_orderpriority, OLD.o_clerk, OLD.o_shippriority, OLD.o_comment, NEW.o_orderkey, NEW.o_custkey, NEW.o_orderstatus, NEW.o_totalprice, NEW.o_orderdate, NEW.o_orderpriority, NEW.o_clerk, NEW.o_shippriority, NEW.o_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_customer_i//
CREATE TRIGGER accel_codegen_tpch_customer_i AFTER INSERT ON customer
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('3', '0', NEW.c_custkey, NEW.c_name, NEW.c_address, NEW.c_nationkey, NEW.c_phone, NEW.c_acctbal, NEW.c_mktsegment, NEW.c_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_customer_d//
CREATE TRIGGER accel_codegen_tpch_customer_d AFTER DELETE ON customer
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('3', '1', OLD.c_custkey, OLD.c_name, OLD.c_address, OLD.c_nationkey, OLD.c_phone, OLD.c_acctbal, OLD.c_mktsegment, OLD.c_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_customer_u//
CREATE TRIGGER accel_codegen_tpch_customer_u AFTER UPDATE ON customer
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('3', '2', OLD.c_custkey, OLD.c_name, OLD.c_address, OLD.c_nationkey, OLD.c_phone, OLD.c_acctbal, OLD.c_mktsegment, OLD.c_comment, NEW.c_custkey, NEW.c_name, NEW.c_address, NEW.c_nationkey, NEW.c_phone, NEW.c_acctbal, NEW.c_mktsegment, NEW.c_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_part_i//
CREATE TRIGGER accel_codegen_tpch_part_i AFTER INSERT ON part
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('4', '0', NEW.p_partkey, NEW.p_name, NEW.p_mfgr, NEW.p_brand, NEW.p_type, NEW.p_size, NEW.p_container, NEW.p_retailprice, NEW.p_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_part_d//
CREATE TRIGGER accel_codegen_tpch_part_d AFTER DELETE ON part
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('4', '1', OLD.p_partkey, OLD.p_name, OLD.p_mfgr, OLD.p_brand, OLD.p_type, OLD.p_size, OLD.p_container, OLD.p_retailprice, OLD.p_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_part_u//
CREATE TRIGGER accel_codegen_tpch_part_u AFTER UPDATE ON part
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('4', '2', OLD.p_partkey, OLD.p_name, OLD.p_mfgr, OLD.p_brand, OLD.p_type, OLD.p_size, OLD.p_container, OLD.p_retailprice, OLD.p_comment, NEW.p_partkey, NEW.p_name, NEW.p_mfgr, NEW.p_brand, NEW.p_type, NEW.p_size, NEW.p_container, NEW.p_retailprice, NEW.p_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_partsupp_i//
CREATE TRIGGER accel_codegen_tpch_partsupp_i AFTER INSERT ON partsupp
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('5', '0', NEW.ps_partkey, NEW.ps_suppkey, NEW.ps_availqty, NEW.ps_supplycost, NEW.ps_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_partsupp_d//
CREATE TRIGGER accel_codegen_tpch_partsupp_d AFTER DELETE ON partsupp
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('5', '1', OLD.ps_partkey, OLD.ps_suppkey, OLD.ps_availqty, OLD.ps_supplycost, OLD.ps_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_partsupp_u//
CREATE TRIGGER accel_codegen_tpch_partsupp_u AFTER UPDATE ON partsupp
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('5', '2', OLD.ps_partkey, OLD.ps_suppkey, OLD.ps_availqty, OLD.ps_supplycost, OLD.ps_comment, NEW.ps_partkey, NEW.ps_suppkey, NEW.ps_availqty, NEW.ps_supplycost, NEW.ps_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_supplier_i//
CREATE TRIGGER accel_codegen_tpch_supplier_i AFTER INSERT ON supplier
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('6', '0', NEW.s_suppkey, NEW.s_name, NEW.s_address, NEW.s_nationkey, NEW.s_phone, NEW.s_acctbal, NEW.s_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_supplier_d//
CREATE TRIGGER accel_codegen_tpch_supplier_d AFTER DELETE ON supplier
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('6', '1', OLD.s_suppkey, OLD.s_name, OLD.s_address, OLD.s_nationkey, OLD.s_phone, OLD.s_acctbal, OLD.s_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_supplier_u//
CREATE TRIGGER accel_codegen_tpch_supplier_u AFTER UPDATE ON supplier
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('6', '2', OLD.s_suppkey, OLD.s_name, OLD.s_address, OLD.s_nationkey, OLD.s_phone, OLD.s_acctbal, OLD.s_comment, NEW.s_suppkey, NEW.s_name, NEW.s_address, NEW.s_nationkey, NEW.s_phone, NEW.s_acctbal, NEW.s_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_nation_i//
CREATE TRIGGER accel_codegen_tpch_nation_i AFTER INSERT ON nation
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('7', '0', NEW.n_nationkey, NEW.n_name, NEW.n_regionkey, NEW.n_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_nation_d//
CREATE TRIGGER accel_codegen_tpch_nation_d AFTER DELETE ON nation
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('7', '1', OLD.n_nationkey, OLD.n_name, OLD.n_regionkey, OLD.n_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_nation_u//
CREATE TRIGGER accel_codegen_tpch_nation_u AFTER UPDATE ON nation
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('7', '2', OLD.n_nationkey, OLD.n_name, OLD.n_regionkey, OLD.n_comment, NEW.n_nationkey, NEW.n_name, NEW.n_regionkey, NEW.n_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_region_i//
CREATE TRIGGER accel_codegen_tpch_region_i AFTER INSERT ON region
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('8', '0', NEW.r_regionkey, NEW.r_name, NEW.r_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_region_d//
CREATE TRIGGER accel_codegen_tpch_region_d AFTER DELETE ON region
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('8', '1', OLD.r_regionkey, OLD.r_name, OLD.r_comment);
END//

DROP TRIGGER IF EXISTS accel_codegen_tpch_region_u//
CREATE TRIGGER accel_codegen_tpch_region_u AFTER UPDATE ON region
FOR EACH ROW
BEGIN
  SET @accel_cap = mysql_accel_mutations_capture_row('8', '2', OLD.r_regionkey, OLD.r_name, OLD.r_comment, NEW.r_regionkey, NEW.r_name, NEW.r_comment);
END//

DELIMITER ;
