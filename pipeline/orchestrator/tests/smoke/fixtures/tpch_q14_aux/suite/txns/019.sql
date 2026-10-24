-- sequence (undo of 016): drop the big-key part row, the chunk of 017 is already gone.
DELETE FROM part WHERE p_comment = 'Q14BIG';
