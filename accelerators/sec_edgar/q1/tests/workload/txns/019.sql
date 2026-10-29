-- A filing leaves one statement group only (its distinct count there drops, its other rows stay).
DELETE FROM pre WHERE stmt = 'IS' AND adsh = '0009990001-24-000001';
