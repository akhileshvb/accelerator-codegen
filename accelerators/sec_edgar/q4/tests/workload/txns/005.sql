-- A tag's label changes: its groups move to the new label (and merge with that label's group).
UPDATE tag SET tlabel = 'Dup label' WHERE tag = 'BatchTag' AND version = 'vB1';
