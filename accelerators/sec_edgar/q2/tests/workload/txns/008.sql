-- A filing changes its fiscal year (all of its rows move between parameter classes) and its name.
UPDATE sub SET fy = 2024, name = 'Renamed Filer' WHERE adsh = '0001114446-24-001605';
