INSERT INTO customer
SELECT * FROM customer
WHERE c_comment = 'q22-seed' AND substring(c_phone, 1, 2) = '13' AND c_acctbal = 9500.00;
