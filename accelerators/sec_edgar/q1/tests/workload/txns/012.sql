-- A whole group is renamed to NULL and gone from every parameter.
UPDATE pre SET stmt = NULL WHERE stmt = 'UN';
