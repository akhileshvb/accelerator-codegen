-- Line numbers change; some become NULL, some NULL ones get a value.
UPDATE pre SET line = line + 1000 WHERE stmt = 'BS' AND report = 1;
UPDATE pre SET line = NULL WHERE stmt = 'IS' AND line % 5 = 0;
UPDATE pre SET line = 3 WHERE tag IN ('NullLine', 'NullBoth');
