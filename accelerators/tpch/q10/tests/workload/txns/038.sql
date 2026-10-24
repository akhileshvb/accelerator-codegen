UPDATE lineitem SET l_returnflag = 'R'
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-02') AND l_returnflag = 'r';
UPDATE customer SET c_name = 'q10-02 ', c_address = 'Case and trailing space ', c_comment = 'Résumé / 東京'
WHERE c_name = 'Q10-02';
UPDATE nation SET n_name = 'q10 Federation ' WHERE n_name = 'Q10 Federation';
