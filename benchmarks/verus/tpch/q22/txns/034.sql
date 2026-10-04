DELETE FROM orders WHERE o_comment = 'q22-duplicate-order';
UPDATE customer SET c_phone = '17-555-0199', c_acctbal = 9800.00
WHERE c_comment = 'q22-seed' AND substring(c_phone, 1, 2) = '13' AND c_acctbal = 9500.00;
