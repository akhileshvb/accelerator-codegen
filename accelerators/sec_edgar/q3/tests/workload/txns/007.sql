-- A fiscal year is corrected: a filing moves between years.
UPDATE sub SET fy = 2098 WHERE adsh = 'Q-N';
UPDATE sub SET fy = 2099 WHERE adsh = 'Q-NF';
