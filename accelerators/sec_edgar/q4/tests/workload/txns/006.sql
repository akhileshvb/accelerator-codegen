-- Filers change their industry: their tuples move to other sic groups.
UPDATE sub SET sic = 5000 WHERE adsh = 'B1-2';
UPDATE sub SET sic = 5200 WHERE adsh = 'F-H2';
