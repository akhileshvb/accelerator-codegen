-- One INSERT ... SELECT of 2^19 rows, a chunk of which crosses the row group boundary (see 034)
INSERT INTO lineitem SELECT source.* FROM lineitem AS source CROSS JOIN range(2) AS copies(copy_number) WHERE l_comment = 'q6-boundary-7';
