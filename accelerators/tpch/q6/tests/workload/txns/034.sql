-- Regression for a bulk insert across a row group boundary (034-054): back to a single 'q6-boundary-7' row, then doubled 18 times (2^18 rows), then 053 inserts two copies of them all (2^19 rows in one statement). Bounded: every pass starts here
DELETE FROM lineitem WHERE l_comment = 'q6-boundary-7';
INSERT INTO lineitem VALUES (7000000000, 1, 1, 7, 23.99, 12345.67, 0.06, 0.02, 'N', 'O', DATE '1994-06-15', DATE '1994-06-15', DATE '1994-06-16', 'NONE', 'AIR', 'q6-boundary-7');
