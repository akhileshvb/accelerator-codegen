// Q8 keeps no separately encoded state images: the published state is the
// in-memory COW state in module.hpp. This translation unit only anchors the
// query's aux directory for the build.
#include "tpch/q8/postgres/aux/module.hpp"
