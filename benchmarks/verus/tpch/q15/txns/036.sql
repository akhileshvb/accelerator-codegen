INSERT INTO supplier
SELECT * REPLACE ('Révenue Élite' AS s_name, 'Rue de la Gare' AS s_address,
  '14-111-222-3333' AS s_phone) FROM test_q15_orphan_supplier;
