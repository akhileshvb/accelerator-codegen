UPDATE customer SET c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL' AND n_comment <> 'q8-nation') WHERE c_comment = 'q8-customer-a';
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL' AND n_comment <> 'q8-nation') WHERE s_comment = 'q8-supplier-a';
DELETE FROM nation WHERE n_comment = 'q8-nation';
