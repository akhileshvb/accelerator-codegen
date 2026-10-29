-- a row crosses the year boundary and changes uom
UPDATE num SET ddate = 20310101 WHERE adsh = 'x-1' AND tag = 'Dates' AND ddate = 20301231;
UPDATE num SET uom = 'MIX' WHERE adsh = 'x-1' AND tag = 'Eleven' AND value < 0.35;
