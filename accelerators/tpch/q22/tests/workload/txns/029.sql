DELETE FROM orders WHERE o_comment = 'q22-large-order';
UPDATE customer SET c_acctbal = c_acctbal - 500.00 WHERE c_comment = 'q22-large';
