-- groups whose values are 1e308 in magnitude and cancel: the sum is 0, but the sum of magnitudes is unsafe for float8 SUM on Postgres (the accelerator declines there).
-- (2016, IS, USD) has such a group beside a plain one; (2017, IS, USD) has one among plain groups of two filings
-- idempotent: a repeated run (snapshots mode) first withdraws its rows
DELETE FROM num WHERE adsh IN ('ov-1', 'ov-2', 'ov-3');
DELETE FROM pre WHERE adsh IN ('ov-1', 'ov-2', 'ov-3');
DELETE FROM sub WHERE adsh IN ('ov-1', 'ov-2', 'ov-3');
INSERT INTO sub (adsh, cik, name, fy, form) VALUES ('ov-1', 91, 'OVF ONE', 2016, '10-K'), ('ov-2', 92, 'OVF TWO', 2017, '10-K'), ('ov-3', 93, 'OVF THREE', 2017, '10-K');
INSERT INTO pre (adsh, report, line, stmt, tag, version, plabel) VALUES
  ('ov-1', 1, 1, 'IS', 'Ovf', 'v', 'Over'), ('ov-1', 1, 2, 'IS', 'Fine', 'v', 'Fine'),
  ('ov-2', 1, 1, 'IS', 'Mix', 'v', 'Mix'), ('ov-2', 1, 2, 'IS', 'Fine', 'v', 'Fine'), ('ov-3', 1, 1, 'IS', 'Fine', 'v', 'Fine');
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
  ('ov-1', 'Ovf', 'v', 20161231, 4, 'USD', 1.0e308), ('ov-1', 'Ovf', 'v', 20161231, 4, 'USD', -1.0e308), ('ov-1', 'Fine', 'v', 20161231, 4, 'USD', 5.0),
  ('ov-2', 'Mix', 'v', 20171231, 4, 'USD', 1.0e308), ('ov-2', 'Mix', 'v', 20171231, 4, 'USD', -1.0e308),
  ('ov-2', 'Fine', 'v', 20171231, 4, 'USD', 7.0), ('ov-3', 'Fine', 'v', 20171231, 4, 'USD', 11.0);
