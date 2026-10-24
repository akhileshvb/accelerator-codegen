UPDATE part SET p_partkey = (SELECT max(p_partkey) + 1 FROM part)
WHERE p_comment = 'v14-regular';
UPDATE lineitem SET l_extendedprice = l_extendedprice + 777.77
WHERE l_comment = 'v14-base-2';
UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.13
WHERE l_comment = 'v14-base-3';
