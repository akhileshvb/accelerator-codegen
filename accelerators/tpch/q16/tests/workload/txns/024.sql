DELETE FROM partsupp WHERE ps_comment = 'v16 mixed write edge';
UPDATE part SET p_type = 'LARGE PLATED BRASS' WHERE p_name = 'Verus catalog 05';
UPDATE supplier SET s_comment = 'v16 billing review completed' WHERE s_name = 'Supplier#V16-08';
