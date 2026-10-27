SELECT g.label, SUM(m.y) AS s FROM m JOIN g ON m.grp = g.grp WHERE m.x < m.y GROUP BY g.label ORDER BY g.label
