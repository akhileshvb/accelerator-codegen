-- the overflow rows are withdrawn: both classes answer again
DELETE FROM num WHERE adsh IN ('ov-1', 'ov-2', 'ov-3');
DELETE FROM pre WHERE adsh IN ('ov-1', 'ov-2', 'ov-3');
DELETE FROM sub WHERE adsh IN ('ov-1', 'ov-2', 'ov-3');
