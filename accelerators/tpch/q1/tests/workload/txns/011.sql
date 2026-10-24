UPDATE lineitem
SET l_discount = 0.03, l_tax = 0.04
WHERE l_comment = 'q1-corrected-batch';
