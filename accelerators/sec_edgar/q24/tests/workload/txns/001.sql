-- a new filing: sub + num + pre copied from an existing one, tags shared with existing groups
INSERT INTO sub SELECT * REPLACE ('cp-0001' AS adsh) FROM sub WHERE adsh = '0001558370-24-012598';
INSERT INTO num SELECT * REPLACE ('cp-0001' AS adsh) FROM num WHERE adsh = '0001558370-24-012598';
INSERT INTO pre SELECT * REPLACE ('cp-0001' AS adsh) FROM pre WHERE adsh = '0001558370-24-012598';
