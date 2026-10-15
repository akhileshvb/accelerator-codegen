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
