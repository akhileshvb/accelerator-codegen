UPDATE lineitem SET l_discount=0.07 WHERE l_comment='q7 live 1';

DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment IN ('q7 zero','q7 orphan'));

DELETE FROM orders WHERE o_comment IN ('q7 zero','q7 orphan');

DELETE FROM supplier WHERE s_comment='q7 supplier Z';

DELETE FROM customer WHERE c_comment='q7 customer Z';

DELETE FROM nation WHERE n_comment IN ('q7 zero nation','q7 text alias');
