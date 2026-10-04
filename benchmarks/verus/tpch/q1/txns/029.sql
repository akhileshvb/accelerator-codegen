DELETE FROM lineitem
WHERE l_comment IN ('q1-exact-duplicate', 'q1-extreme-text', 'q1-setup-boundary', 'q1-setup-text');
