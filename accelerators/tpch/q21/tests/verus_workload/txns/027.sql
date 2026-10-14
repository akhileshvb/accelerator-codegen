-- Re-add one on-time line from another supplier: the order qualifies again.
INSERT INTO lineitem
SELECT template.* REPLACE (
  2 AS l_linenumber,
  (SELECT MIN(s_suppkey) FROM supplier WHERE s_suppkey <> template.l_suppkey) AS l_suppkey,
  DATE '1996-01-06' AS l_receiptdate
)
FROM lineitem AS template
WHERE template.l_comment = 'q21:ordinary:line' AND template.l_linenumber = 1;
