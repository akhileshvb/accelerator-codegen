UPDATE lineitem SET l_extendedprice = 100.01 WHERE l_comment = 'q6-isolated-1';
UPDATE lineitem SET l_extendedprice = -100.01 WHERE l_comment = 'q6-isolated-2';
DELETE FROM lineitem WHERE l_comment = 'q6-isolated-3';
