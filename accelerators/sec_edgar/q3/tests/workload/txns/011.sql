-- Half of the tied companies go: the average moves and the tied group changes.
DELETE FROM num WHERE adsh IN (SELECT 'TIEQ-' || i FROM range(0, 300, 3) t(i));
DELETE FROM sub WHERE adsh IN (SELECT 'TIEQ-' || i FROM range(0, 300, 3) t(i));
