UPDATE customer SET c_acctbal = 1250.00
WHERE c_comment = 'q22-seed' AND substring(c_phone, 1, 2) = '13' AND c_acctbal = 50000.00;
