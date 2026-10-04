UPDATE part SET p_type = 'promo POLISHED STEEL'
WHERE rowid = (SELECT min(rowid) FROM part WHERE p_comment = 'v14-duplicate');
DELETE FROM part
WHERE rowid = (SELECT min(rowid) FROM part WHERE p_comment = 'v14-duplicate');
DELETE FROM lineitem
WHERE rowid = (SELECT min(rowid) FROM lineitem WHERE l_comment = 'v14-base-3');
UPDATE part SET p_type = 'PROMO POLISHED STEEL '
WHERE p_comment = 'v14-twin' AND p_type = ' PROMO POLISHED STEEL';
