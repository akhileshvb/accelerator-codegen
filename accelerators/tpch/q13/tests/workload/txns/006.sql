UPDATE orders SET o_comment = 'Repeat order correction approved'
WHERE o_comment = 'unusual corrected accounts';
