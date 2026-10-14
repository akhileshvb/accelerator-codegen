UPDATE lineitem SET l_linestatus = 'F'
WHERE l_comment = 'q1-new-small' AND l_linenumber = 1;

UPDATE orders SET o_orderstatus = 'P'
WHERE o_comment = 'q1-new-small';
