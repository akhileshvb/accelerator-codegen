-- uom and tag edits: rows change class and group.
UPDATE num SET uom = 'USD' WHERE uom = 'pure' AND rowid % 4 = 0;
UPDATE num SET tag = tag || '_x' WHERE uom = 'shares' AND rowid % 5 = 0;
