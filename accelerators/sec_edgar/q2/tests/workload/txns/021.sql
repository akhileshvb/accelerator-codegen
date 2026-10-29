-- A sub row changes fiscal year and name: a filing holding top rows of one class moves to another class.
UPDATE sub SET fy = 2024, name = 'Moved Filer'
WHERE adsh = (SELECT n.adsh FROM num n JOIN sub s ON n.adsh = s.adsh WHERE s.fy = 2021 AND n.uom = 'USD' AND n.value IS NOT NULL ORDER BY n.value DESC, n.adsh LIMIT 1);
