UPDATE orders SET o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q10-05')
WHERE o_comment = 'Q10-New-A';
