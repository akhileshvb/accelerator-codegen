-- Duplicate sub rows for one adsh (the join counts both), and duplicate num rows.
INSERT INTO sub SELECT adsh, cik, 'Twin Of ' || name, sic, countryba, stprba, cityba, countryinc, form, period, fy, fp, filed, accepted, prevrpt, nciks, afs, wksi, fye, instance FROM sub WHERE adsh = '0001114446-24-001605';
INSERT INTO num SELECT * FROM num WHERE adsh = '0001114446-24-001605' AND uom = 'USD' AND value > 1e7;
