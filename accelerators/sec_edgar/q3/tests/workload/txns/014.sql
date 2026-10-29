-- Every figure of a unit is scaled: all totals and the average move together.
UPDATE num SET value = value * 2.0 WHERE uom = 'TQ' AND adsh LIKE 'Q-%';
