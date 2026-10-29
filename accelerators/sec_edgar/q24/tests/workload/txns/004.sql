-- an amendment: a filing's num rows are replaced by changed ones
DELETE FROM num WHERE adsh = 'cp-0001';
INSERT INTO num SELECT * REPLACE ('cp-0001' AS adsh, value * 2 AS value, ddate + 1 AS ddate) FROM num WHERE adsh = '0001558370-24-012598' AND value IS NOT NULL;
