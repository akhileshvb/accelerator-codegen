UPDATE lineitem SET l_extendedprice=0.00, l_discount=1.00 WHERE l_suppkey IN (SELECT s_suppkey FROM supplier JOIN nation ON s_nationkey=n_nationkey WHERE n_name='INDIA');
