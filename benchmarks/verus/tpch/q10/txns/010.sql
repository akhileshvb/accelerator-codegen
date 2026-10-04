UPDATE customer SET c_name = 'Q10-01', c_address = 'New return billing address',
       c_phone = '10-200-200-2000', c_acctbal = c_acctbal - 150.25, c_comment = 'Return credit posted'
WHERE c_name = 'Q10-01';
