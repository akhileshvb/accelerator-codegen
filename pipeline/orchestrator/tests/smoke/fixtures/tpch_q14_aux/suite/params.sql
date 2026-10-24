-- TPC-H Q14 fixed parameter tuples, one per line, in $n order:
--   $1 = start_date (DATE), $2 = start_date + INTERVAL '1' month (the derived end date).
-- The accelerator's SQL entry point takes only $1 (arity 1) and derives $2 itself, so every tuple
-- below keeps $2 = $1 + 1 month exactly as DuckDB evaluates it, including the month-end clamps.
-- Tuple classes are recorded in suite.yaml (edge_coverage); comments are not counted.

-- 1-5: in spec. The benchmark's own generator draws the first of a month in [1993-01, 1997-12];
-- these spread over the whole data range (1992-01-02 .. 1998-12-01), where the answer is non-empty.
DATE '1995-06-01', DATE '1995-07-01'
DATE '1993-01-01', DATE '1993-02-01'
DATE '1997-12-01', DATE '1998-01-01'
DATE '1994-04-01', DATE '1994-05-01'
DATE '1996-09-01', DATE '1996-10-01'

-- 6-8: off spec but valid DATEs, outside the generator's first-of-month set: a mid-month start, a
-- window opening on the last data day, and a window opening on the first data day (partial month).
DATE '1995-06-15', DATE '1995-07-15'
DATE '1998-12-01', DATE '1999-01-01'
DATE '1992-01-02', DATE '1992-02-02'

-- 9-11: windows that qualify no lineitem: entirely after the data, entirely before it, and starting
-- past the last ship date. The answer is a single row whose value is NULL (see suite.yaml).
DATE '1998-12-15', DATE '1999-01-15'
DATE '1990-05-05', DATE '1990-06-05'
DATE '2000-01-01', DATE '2000-02-01'

-- 12: NULL parameter. SQL semantics: l_shipdate >= NULL is never true, so no row qualifies and the
-- scalar aggregate is NULL. A kernel must answer NULL, never throw.
NULL::DATE, NULL::DATE

-- 13-18: extreme dates: month-end clamping (Jan 31 -> Feb 28), leap day, a year boundary, the minimum
-- DATE, and a DATE whose end date is still inside DuckDB's range.
DATE '1995-01-31', DATE '1995-02-28'
DATE '1996-02-29', DATE '1996-03-29'
DATE '1995-12-31', DATE '1996-01-31'
DATE '0001-01-01', DATE '0001-02-01'
DATE '9999-11-30', DATE '9999-12-30'
DATE '1993-03-31', DATE '1993-04-30'
