-- an amendment: a new filing of the same company marking the amended one
INSERT INTO sub SELECT * REPLACE ('cp-0002' AS adsh, 1 AS prevrpt) FROM sub WHERE adsh = '0000950170-23-060916';
UPDATE sub SET prevrpt = 1 WHERE adsh = 'cp-0001';
INSERT INTO num SELECT * REPLACE ('cp-0002' AS adsh, value * 1.01 AS value) FROM num WHERE adsh = '0000950170-23-060916';
INSERT INTO pre SELECT * REPLACE ('cp-0002' AS adsh) FROM pre WHERE adsh = '0000950170-23-060916';
