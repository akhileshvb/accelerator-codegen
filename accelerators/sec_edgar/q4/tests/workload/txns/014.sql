-- A statement of a line changes and a tag becomes abstract: tuples leave the answer.
UPDATE pre SET stmt = 'IS' WHERE adsh = 'F-H1';
UPDATE tag SET abstract = 1 WHERE tag = 'Rounding' AND version = 'vF';
