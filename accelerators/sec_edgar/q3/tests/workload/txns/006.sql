-- A company number is corrected: two companies merge (total 30), another loses its number (NULL company).
UPDATE sub SET cik = 8000001 WHERE adsh = 'Q-B';
UPDATE sub SET cik = NULL WHERE adsh = 'Q-D';
