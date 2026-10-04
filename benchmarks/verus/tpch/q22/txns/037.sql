UPDATE customer SET c_acctbal = 0.00 WHERE c_comment = 'q22-boundary';
UPDATE customer SET c_acctbal = 0.02 WHERE c_comment = 'q22-fraction';
UPDATE customer SET c_acctbal = 0.01 WHERE c_name = 'Nonpositive zero' AND c_comment = 'q22-zero';
