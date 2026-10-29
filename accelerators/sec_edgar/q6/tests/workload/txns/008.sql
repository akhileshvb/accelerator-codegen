-- big batch: eight copies of real filings
INSERT INTO sub SELECT * REPLACE ('bb-' || adsh AS adsh) FROM sub WHERE fy = 2023 AND rowid % 3 = 0;
INSERT INTO num SELECT * REPLACE ('bb-' || adsh AS adsh) FROM num WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2023 AND rowid % 3 = 0);
INSERT INTO pre SELECT * REPLACE ('bb-' || adsh AS adsh) FROM pre WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2023 AND rowid % 3 = 0);
