UPDATE lineitem SET l_extendedprice = -9999999999999.99 WHERE l_comment = 'q6-extreme-2';
UPDATE lineitem SET l_discount = -0.05, l_quantity = -0.02 WHERE l_comment = 'q6-extreme-1';
UPDATE lineitem SET l_quantity = 9999999999999.98 WHERE l_comment = 'q6-extreme-3';
DELETE FROM lineitem WHERE l_comment = 'q6-isolated-2';
DELETE FROM lineitem WHERE l_comment = 'q6-extreme-4';
