INSERT INTO supplier SELECT * FROM supplier WHERE s_comment='q7 supplier A';

INSERT INTO customer SELECT * FROM customer WHERE c_comment='q7 customer A';

INSERT INTO orders SELECT * FROM orders WHERE o_comment='q7 live';

INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment='q7 live 1';

INSERT INTO nation SELECT * FROM nation WHERE n_name IN ('FRANCE','GERMANY');
