-- a new filing: sub + num + pre, tags shared with existing groups
INSERT INTO sub SELECT * REPLACE ('cp-0001' AS adsh) FROM sub WHERE adsh = '0000950170-23-060916';
INSERT INTO num SELECT * REPLACE ('cp-0001' AS adsh) FROM num WHERE adsh = '0000950170-23-060916';
INSERT INTO pre SELECT * REPLACE ('cp-0001' AS adsh) FROM pre WHERE adsh = '0000950170-23-060916';
