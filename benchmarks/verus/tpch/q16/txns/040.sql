INSERT INTO supplier
SELECT s_suppkey, 'Supplier#V16-06-copy1', s_address, s_nationkey, s_phone, s_acctbal,
       'CustomerComplaints' FROM supplier WHERE s_name = 'Supplier#V16-06';
INSERT INTO supplier
SELECT s_suppkey, 'Supplier#V16-06-copy2', s_address, s_nationkey, s_phone, s_acctbal,
       'Customer repeated Customer Complaints' FROM supplier WHERE s_name = 'Supplier#V16-06';
DELETE FROM supplier WHERE s_name = 'Supplier#V16-06';
