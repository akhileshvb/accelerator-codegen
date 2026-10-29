-- Rows move between groups.
UPDATE pre SET stmt = 'CF' WHERE stmt = 'CI' AND report % 2 = 0;
