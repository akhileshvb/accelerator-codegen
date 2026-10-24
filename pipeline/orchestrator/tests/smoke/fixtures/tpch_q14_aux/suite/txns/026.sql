-- sequence (remove the duplicated row of 025): one copy is left, and the answer is back. The pair of
-- transactions is what makes a de-duplicating kernel visible.
DELETE FROM lineitem WHERE l_comment = 'Q14DUPROW';
