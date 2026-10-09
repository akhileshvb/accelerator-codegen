// The Q4 module is engine-neutral: the same translation unit as the DuckDB
// engine (src/generated/sec_edgar/q4/common/aux/), compiled into this engine's
// aux library.  Engine specifics live in extension/scan.cpp (SQL over SPI).
#include "sec_edgar/q4/common/aux/module.cpp"
