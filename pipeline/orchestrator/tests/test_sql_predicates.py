from __future__ import annotations

from accel_pipeline.sql_predicates import (
    extract_predicate_checklist,
    format_predicate_bootstrap_section,
)


Q16_SQL = """
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey
  AND p_brand <> 'Brand#13'
  AND p_type NOT LIKE 'SMALL BRUSHED%'
  AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33)
  AND ps_suppkey NOT IN (
    SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size;
"""


def test_q16_checklist_covers_polarity():
    checks = extract_predicate_checklist(Q16_SQL)
    joined = "\n".join(checks)
    assert "NOT LIKE" in joined
    assert "Inequality" in joined
    assert "Anti-join" in joined or "complaint" in joined.lower()
    assert "Hot-path" in joined


def test_bootstrap_section_mentions_contract(tmp_path):
    sql_path = tmp_path / "16.sql"
    sql_path.write_text(Q16_SQL, encoding="utf-8")
    section = format_predicate_bootstrap_section(Q16_SQL, str(sql_path))
    assert "unit_test_contract.md" in section
    assert "NOT LIKE" in section
    assert "Brand#13" in section or "<>" in section
