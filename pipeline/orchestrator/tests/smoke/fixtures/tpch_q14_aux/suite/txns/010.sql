-- sequence (remove the duplicate of 009): the key is unique again, so the answer returns to its shape
-- before 009. The accelerator must treat the duplicate and its removal symmetrically.
DELETE FROM part WHERE p_comment = 'Q14DUP';
