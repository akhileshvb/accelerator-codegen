-- The maximum of a group moves: the top row doubles, another top row falls to the second best, a third is deleted.
UPDATE num SET value = value * 2 WHERE rowid = (SELECT rowid FROM num WHERE uom = 'USD' AND adsh = '0001114446-24-001605' AND tag = 'Assets' ORDER BY value DESC, rowid LIMIT 1);
UPDATE num SET value = 1.0 WHERE rowid = (SELECT rowid FROM num WHERE uom = 'USD' AND adsh = '0001114446-24-001605' AND tag = 'EquityAndLiabilities' ORDER BY value DESC, rowid LIMIT 1);
DELETE FROM num WHERE rowid = (SELECT rowid FROM num WHERE uom = 'USD' AND adsh = '0001193125-23-151943' ORDER BY value DESC, rowid LIMIT 1);
