-- Small SEC-EDGAR subset for fast shared-interface tests (sec_edgar_test.duckdb).
--
-- Run in a fresh DuckDB database with the source (SF3) database attached
-- read-only as `src`. Built by scripts/create_sec_edgar_test_db.py and, for
-- ctest, by make_sec_edgar_test_db (eval/tests/make_sec_edgar_test_db.cpp)
-- into the build dir.
--
-- selected_adsh holds filings that give each shared query's catalog probe a
-- row (80 filings per seed); the tables keep every row of those filings.
CREATE TEMP TABLE selected_adsh AS
    SELECT DISTINCT adsh FROM (
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL
         ORDER BY 1
         LIMIT 80)
        UNION
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL
         ORDER BY 1
         LIMIT 80)
        UNION
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         JOIN src.pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
         JOIN src.tag t ON n.tag = t.tag AND n.version = t.version
         WHERE n.uom = 'USD' AND p.stmt = 'EQ'
           AND s.sic BETWEEN 4000 AND 4999
           AND n.value IS NOT NULL AND t.abstract = 0
         ORDER BY 1
         LIMIT 80)

        UNION
        -- Q4: every filing of a few (sic, tlabel) groups that have >= 2
        -- companies, so the subset keeps a non-empty Q4 result to probe.
        (SELECT DISTINCT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         JOIN src.pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
         JOIN src.tag t ON n.tag = t.tag AND n.version = t.version
         JOIN (SELECT s2.sic, t2.tlabel
               FROM src.num n2
               JOIN src.sub s2 ON n2.adsh = s2.adsh
               JOIN src.pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND n2.version = p2.version
               JOIN src.tag t2 ON n2.tag = t2.tag AND n2.version = t2.version
               WHERE n2.uom = 'USD' AND p2.stmt = 'EQ'
                 AND s2.sic BETWEEN 4000 AND 4999
                 AND n2.value IS NOT NULL AND t2.abstract = 0
               GROUP BY s2.sic, t2.tlabel
               HAVING COUNT(DISTINCT s2.cik) >= 2
               ORDER BY s2.sic, t2.tlabel
               LIMIT 3) g ON s.sic = g.sic AND t.tlabel = g.tlabel
         WHERE n.uom = 'USD' AND p.stmt = 'EQ'
           AND s.sic BETWEEN 4000 AND 4999
           AND n.value IS NOT NULL AND t.abstract = 0)
        UNION
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         JOIN src.pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
         WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 AND n.value IS NOT NULL
         ORDER BY 1
         LIMIT 80)
        UNION
        (SELECT adsh
         FROM src.pre
         WHERE stmt IS NOT NULL
         ORDER BY 1
         LIMIT 80)
        UNION
        (SELECT n.adsh
         FROM src.num n
         LEFT JOIN src.pre p
           ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh
         WHERE n.uom = 'USD'
           AND n.ddate BETWEEN 20230101 AND 20231231
           AND n.value IS NOT NULL
           AND p.adsh IS NULL
         ORDER BY 1
         LIMIT 80)

        -- Parameter-domain coverage: the Verus pipeline tests every query on random draws from
        -- eval/bench/sec_edgar/params/q<N>.json, so every binding with an answer on the source has one here.
        UNION
        -- Q2, Q3: three companies per (fy, uom), each with one filing.
        (SELECT adsh FROM (
            SELECT MIN(s.adsh) AS adsh,
                   ROW_NUMBER() OVER (PARTITION BY s.fy, n.uom ORDER BY s.cik) AS k
            FROM src.num n JOIN src.sub s ON n.adsh = s.adsh
            WHERE s.fy BETWEEN 2021 AND 2024 AND n.value IS NOT NULL
              AND n.uom IN ('USD', 'shares', 'pure', 'CAD', 'CNY', 'EUR')
            GROUP BY s.fy, n.uom, s.cik) per_company
         WHERE k <= 3)
        UNION
        -- Q6: two companies per (fy, stmt, uom) with presented facts.
        (SELECT adsh FROM (
            SELECT MIN(s.adsh) AS adsh,
                   ROW_NUMBER() OVER (PARTITION BY s.fy, p.stmt, n.uom ORDER BY s.cik) AS k
            FROM src.num n
            JOIN src.sub s ON n.adsh = s.adsh
            JOIN src.pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
            WHERE s.fy BETWEEN 2021 AND 2024 AND n.value IS NOT NULL
              AND n.uom IN ('USD', 'shares', 'pure', 'CAD', 'CNY', 'EUR')
              AND p.stmt IN ('BS', 'IS', 'CF', 'EQ', 'CI')
            GROUP BY s.fy, p.stmt, n.uom, s.cik) per_company
         WHERE k <= 2)
        UNION
        -- Q4: per (SIC range, stmt, uom), two filers of its first (sic, tlabel) group with >= 2 companies.
        (WITH ranges(lo, hi) AS (VALUES (100, 999), (1000, 1499), (1500, 1799), (2000, 3999), (4000, 4999),
                                        (5000, 5199), (5200, 5999), (6000, 6799), (7000, 8999), (9000, 9999)),
              facts AS (
                SELECT DISTINCT r.lo, p.stmt, n.uom, s.sic, t.tlabel, s.cik, s.adsh
                FROM src.num n
                JOIN src.sub s ON n.adsh = s.adsh
                JOIN src.tag t ON n.tag = t.tag AND n.version = t.version
                JOIN src.pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
                JOIN ranges r ON s.sic BETWEEN r.lo AND r.hi
                WHERE n.value IS NOT NULL AND t.abstract = 0
                  AND n.uom IN ('USD', 'shares', 'pure', 'CAD', 'CNY', 'EUR')
                  AND p.stmt IN ('BS', 'IS', 'CF', 'EQ', 'CI')),
              first_group AS (
                SELECT lo, stmt, uom, sic, tlabel FROM (
                    SELECT lo, stmt, uom, sic, tlabel,
                           ROW_NUMBER() OVER (PARTITION BY lo, stmt, uom ORDER BY sic, tlabel) AS k
                    FROM facts GROUP BY lo, stmt, uom, sic, tlabel HAVING COUNT(DISTINCT cik) >= 2) ranked
                WHERE k = 1)
         SELECT adsh FROM (
            SELECT MIN(f.adsh) AS adsh, ROW_NUMBER() OVER (PARTITION BY f.lo, f.stmt, f.uom ORDER BY f.cik) AS k
            FROM facts f JOIN first_group g
              ON f.lo = g.lo AND f.stmt = g.stmt AND f.uom = g.uom AND f.sic = g.sic AND f.tlabel = g.tlabel
            GROUP BY f.lo, f.stmt, f.uom, f.cik) picked
         WHERE k <= 2)
        UNION
        -- Q24: every filing of each (tag, version) with more than 10 unpresented facts in a (year, uom).
        (WITH unpresented AS (
            SELECT n.adsh, n.tag, n.version, n.ddate // 10000 AS y, n.uom
            FROM src.num n
            ANTI JOIN src.pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh
            WHERE n.ddate // 10000 BETWEEN 2019 AND 2024 AND n.value IS NOT NULL
              AND n.uom IN ('USD', 'shares', 'pure', 'CAD', 'CNY', 'EUR'))
         SELECT u.adsh FROM unpresented u
         JOIN (SELECT y, uom, tag, version FROM unpresented GROUP BY y, uom, tag, version HAVING COUNT(*) > 10) g
           ON u.y = g.y AND u.uom = g.uom AND u.tag = g.tag AND u.version = g.version)
    ) seeds;
CREATE TABLE sub AS SELECT * FROM src.sub WHERE adsh IN (SELECT adsh FROM selected_adsh);
CREATE TABLE num AS SELECT * FROM src.num WHERE adsh IN (SELECT adsh FROM selected_adsh);
CREATE TABLE pre AS SELECT * FROM src.pre WHERE adsh IN (SELECT adsh FROM selected_adsh);
CREATE TABLE tag AS
SELECT DISTINCT t.*
FROM src.tag t
JOIN (
    SELECT DISTINCT tag, version FROM num
    UNION
    SELECT DISTINCT tag, version FROM pre
) refs ON t.tag = refs.tag AND t.version = refs.version;
