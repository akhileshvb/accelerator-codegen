UPDATE customer SET c_acctbal = c_acctbal + 250.00, c_address = 'Revised batch billing address'
WHERE c_name BETWEEN 'Q10-01' AND 'Q10-12';
