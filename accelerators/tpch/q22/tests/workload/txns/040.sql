UPDATE customer SET c_phone = 'aa-555-0101'
WHERE c_name = 'Upper high' AND c_comment = 'q22-text';
UPDATE customer SET c_phone = 'A -555-0101'
WHERE c_name = 'Lower high' AND c_comment = 'q22-text';
UPDATE customer SET c_phone = 'x'
WHERE c_name = 'Empty high' AND c_comment = 'q22-text';
UPDATE customer SET c_phone = ''
WHERE c_name = 'Spaces high' AND c_comment = 'q22-text';
