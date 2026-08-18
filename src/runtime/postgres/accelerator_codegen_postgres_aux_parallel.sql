-- Optional snippet for a database that already has accelerator_codegen_postgres
-- installed from a parallel-aux build. CREATE OR REPLACE does not DROP the
-- extension. Do not run this against a live ST sweep database whose .so lacks
-- the *_Parallel C symbols.

CREATE OR REPLACE FUNCTION accel_sec_edgar_q1_aux_parallel()
RETURNS TABLE (
  stmt text,
  rfile text,
  cnt bigint,
  num_filings bigint,
  avg_line_num double precision
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxSecEdgarQ1Parallel'
LANGUAGE C;

CREATE OR REPLACE FUNCTION accel_sec_edgar_q2_aux_parallel()
RETURNS TABLE (
  name text,
  tag text,
  value double precision
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxSecEdgarQ2Parallel'
LANGUAGE C;

CREATE OR REPLACE FUNCTION accel_sec_edgar_q3_aux_parallel()
RETURNS TABLE (
  name text,
  cik bigint,
  total_value double precision
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxSecEdgarQ3Parallel'
LANGUAGE C;

CREATE OR REPLACE FUNCTION accel_sec_edgar_q4_aux_parallel()
RETURNS TABLE (
  sic bigint,
  tlabel text,
  stmt text,
  num_companies bigint,
  total_value double precision,
  avg_value double precision
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxSecEdgarQ4Parallel'
LANGUAGE C;

CREATE OR REPLACE FUNCTION accel_sec_edgar_q6_aux_parallel()
RETURNS TABLE (
  name text,
  stmt text,
  tag text,
  plabel text,
  total_value double precision,
  cnt bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxSecEdgarQ6Parallel'
LANGUAGE C;

CREATE OR REPLACE FUNCTION accel_sec_edgar_q24_aux_parallel()
RETURNS TABLE (
  tag text,
  version text,
  cnt bigint,
  total double precision
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxSecEdgarQ24Parallel'
LANGUAGE C;
