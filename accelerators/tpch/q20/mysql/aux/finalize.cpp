#include "tpch/q20/mysql/aux/module.hpp"

// Q20 has no separate finalization step: TpchQ20Module::EmitResult resolves the
// qualifying suppliers and orders them by s_name.
