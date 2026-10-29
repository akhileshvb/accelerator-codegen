-- Amendment: the filing holding the largest USD value of fiscal year 2023 restates all its values downwards, so the next in line move up.
UPDATE num SET value = value / 1.0e6
WHERE uom = 'USD' AND adsh = (SELECT n.adsh FROM num n JOIN sub s ON n.adsh = s.adsh WHERE s.fy = 2023 AND n.uom = 'USD' AND n.value IS NOT NULL ORDER BY n.value DESC, n.adsh LIMIT 1);
