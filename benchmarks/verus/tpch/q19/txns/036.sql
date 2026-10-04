UPDATE part SET p_partkey = (SELECT max(p_partkey) + 1 FROM part) WHERE p_name = 'q19-stock-11';
DELETE FROM part WHERE p_name = 'q19-stock-09';
UPDATE part SET p_size = 15 WHERE p_name = 'q19-stock-12';
UPDATE part SET p_size = 10 WHERE p_name = 'q19-stock-07';
