#include "tpch_arg_variants.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <random>
#include <sstream>
#include <stdexcept>

#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen::benchmarks::aux_sweep {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace catalog_ns = accelerator_codegen::aux_state::engine;

bool EnvFlagTrue(const char *value, bool default_value) {
  if (!value || !*value) {
    return default_value;
  }
  if (value[0] == '0' && value[1] == '\0') {
    return false;
  }
  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return !(text == "0" || text == "false" || text == "off" || text == "no");
}

std::mt19937_64 MakeRng(int query_id, uint64_t stream_index) {
  // Split streams per query and index so pools are stable across process runs.
  const uint64_t seed =
      ArgSeed() ^ (static_cast<uint64_t>(query_id) * 0x9e3779b97f4a7c15ULL) ^
      (stream_index * 0xbf58476d1ce4e5b9ULL);
  return std::mt19937_64(seed);
}

template <typename T, std::size_t N>
const T &Pick(std::mt19937_64 &rng, const T (&values)[N]) {
  std::uniform_int_distribution<std::size_t> dist(0, N - 1);
  return values[dist(rng)];
}

core::AuxDate MakeDate(int32_t year, int32_t month, int32_t day) {
  const auto date = ::duckdb::Date::FromDate(year, month, day);
  return core::AuxDate{::duckdb::Date::EpochDays(date)};
}

std::string DateLiteral(const core::AuxDate &date) {
  ::duckdb::date_t value{};
  value.days = date.days_since_epoch;
  return "DATE '" + ::duckdb::Date::ToString(value) + "'";
}

std::string SqlString(const std::string &text) {
  std::string out = "'";
  for (char c : text) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string FormatDouble(double value) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(6);
  out << value;
  std::string text = out.str();
  while (text.size() > 1 && text.find('.') != std::string::npos &&
         text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

const std::string &RequireString(const core::QueryArguments &arguments,
                                 std::size_t index) {
  if (arguments.values.size() <= index || arguments.values[index].is_null ||
      !std::holds_alternative<std::string>(arguments.values[index].payload)) {
    throw std::runtime_error("expected string query argument");
  }
  return std::get<std::string>(arguments.values[index].payload);
}

const core::AuxDate &RequireDate(const core::QueryArguments &arguments,
                                 std::size_t index) {
  if (arguments.values.size() <= index || arguments.values[index].is_null ||
      !std::holds_alternative<core::AuxDate>(arguments.values[index].payload)) {
    throw std::runtime_error("expected date query argument");
  }
  return std::get<core::AuxDate>(arguments.values[index].payload);
}

int64_t RequireInt64(const core::QueryArguments &arguments, std::size_t index) {
  if (arguments.values.size() <= index || arguments.values[index].is_null ||
      !std::holds_alternative<int64_t>(arguments.values[index].payload)) {
    throw std::runtime_error("expected int64 query argument");
  }
  return std::get<int64_t>(arguments.values[index].payload);
}

double RequireDouble(const core::QueryArguments &arguments, std::size_t index) {
  if (arguments.values.size() <= index || arguments.values[index].is_null) {
    throw std::runtime_error("expected double query argument");
  }
  if (std::holds_alternative<double>(arguments.values[index].payload)) {
    return std::get<double>(arguments.values[index].payload);
  }
  if (std::holds_alternative<int64_t>(arguments.values[index].payload)) {
    return static_cast<double>(
        std::get<int64_t>(arguments.values[index].payload));
  }
  throw std::runtime_error("expected double query argument");
}

// Official TPC-H substitution domains (dists.dss / Clause 2.4).
constexpr const char *kRegions[] = {"AFRICA", "AMERICA", "ASIA", "EUROPE",
                                    "MIDDLE EAST"};

constexpr const char *kNations[] = {
    "ALGERIA",        "ARGENTINA", "BRAZIL",  "CANADA",
    "EGYPT",          "ETHIOPIA",  "FRANCE",  "GERMANY",
    "INDIA",          "INDONESIA", "IRAN",    "IRAQ",
    "JAPAN",          "JORDAN",    "KENYA",   "MOROCCO",
    "MOZAMBIQUE",     "PERU",      "CHINA",   "ROMANIA",
    "SAUDI ARABIA",   "VIETNAM",   "RUSSIA",  "UNITED KINGDOM",
    "UNITED STATES"};

constexpr const char *kCountryCodes[] = {
    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34"};

constexpr const char *kSegments[] = {"AUTOMOBILE", "BUILDING", "FURNITURE",
                                     "HOUSEHOLD", "MACHINERY"};

constexpr const char *kShipmodes[] = {"REG AIR", "AIR",  "RAIL", "SHIP",
                                      "TRUCK",   "MAIL", "FOB"};

constexpr const char *kColors[] = {
    "almond",     "antique",    "aquamarine", "azure",      "beige",
    "bisque",     "black",      "blanched",   "blue",       "blush",
    "brown",      "burlywood",  "chartreuse", "chiffon",    "chocolate",
    "coral",      "cornflower", "cornsilk",   "cream",      "cyan",
    "dark",       "deep",       "dim",        "dodger",     "drab",
    "firebrick",  "floral",     "forest",     "frost",      "gainsboro",
    "ghost",      "goldenrod",  "green",      "grey",       "honeydew",
    "hot",        "indian",     "ivory",      "khaki",      "lace",
    "lavender",   "lawn",       "lemon",      "light",      "lime",
    "linen",      "magenta",    "maroon",     "medium",     "metallic",
    "midnight",   "mint",       "misty",      "moccasin",   "navajo",
    "navy",       "olive",      "orange",     "orchid",     "pale",
    "papaya",     "peach",      "peru",       "pink",       "plum",
    "powder",     "puff",       "purple",     "red",        "rose",
    "rosy",       "royal",      "saddle",     "salmon",     "sandy",
    "seashell",   "sienna",     "sky",        "slate",      "smoke",
    "snow",       "spring",     "steel",      "tan",        "thistle",
    "tomato",     "turquoise",  "violet",     "wheat",      "white",
    "yellow"};

constexpr const char *kContainers[] = {
    "SM CASE",  "SM BOX",  "SM BAG",  "SM JAR",  "MED BAG", "MED BOX",
    "MED PKG",  "MED PACK", "LG CASE", "LG BOX", "LG PACK", "LG PKG"};

constexpr const char *kBrands[] = {"Brand#1", "Brand#2", "Brand#3", "Brand#4", "Brand#5"};

// Q16 p_type NOT LIKE '<syllable1> <syllable2>%' (TPC-H 2.4.16).
constexpr const char *kTypeSyllable1[] = {
    "STANDARD", "SMALL", "MEDIUM", "LARGE", "ECONOMY", "PROMO"};
constexpr const char *kTypeSyllable2[] = {
    "ANODIZED", "BURNISHED", "PLATED", "POLISHED", "BRUSHED"};
constexpr const char *kTypeSyllable3[] = {
    "TIN", "NICKEL", "BRASS", "STEEL", "COPPER"};
constexpr const char *kQ13CommentPatterns[] = {
    "%unusual%accounts%", "%special%requests%", "%pending%deposits%",
    "%express%packages%", "%careful%requests%"};

double Q11Fraction() {
  // Catalog pins 0.0001; official TPC-H uses 0.0001/SF. Allow override.
  const char *sf = std::getenv("ACCEL_TPCH_SF");
  if (sf && *sf) {
    const double scale = std::stod(sf);
    if (scale > 0.0) {
      return 0.0001 / scale;
    }
  }
  const char *frac = std::getenv("ACCEL_TPCH_Q11_FRACTION");
  if (frac && *frac) {
    return std::stod(frac);
  }
  return 0.0001;
}

} // namespace

bool VaryArgsEnabled() {
  return EnvFlagTrue(std::getenv("ACCEL_BENCH_VARY_ARGS"), true);
}

uint64_t ArgSeed() {
  const char *value = std::getenv("ACCEL_BENCH_ARG_SEED");
  if (!value || !*value) {
    return 1;
  }
  return static_cast<uint64_t>(std::stoull(value));
}

std::size_t ArgPoolSize() {
  const char *value = std::getenv("ACCEL_BENCH_ARG_POOL");
  if (!value || !*value) {
    return 32;
  }
  return std::max<std::size_t>(1, static_cast<std::size_t>(std::stoul(value)));
}

core::QueryArguments SampleTpchArgs(int query_id, uint64_t stream_index) {
  auto rng = MakeRng(query_id, stream_index);
  core::QueryArguments arguments;

  switch (query_id) {
  case 1: {
    std::uniform_int_distribution<int64_t> dist(60, 120);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Int64, dist(rng))};
    break;
  }
    case 2: {
    std::uniform_int_distribution<int> size_dist(1, 50);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(size_dist(rng))),
        core::AuxValue::Of(core::AuxType::String, '%' + std::string{Pick(rng, kTypeSyllable1)}),
        core::AuxValue::Of(core::AuxType::String, std::string{Pick(rng, kRegions)})};
    break;
  }
case 3: {
    std::uniform_int_distribution<int> day_dist(1, 31);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, std::string{Pick(rng, kSegments)}),
        core::AuxValue::Of(core::AuxType::Date,
                           MakeDate(1995, 3, day_dist(rng)))};
    break;
  }
  case 4: {
    // TPC-H Q4 date windows: 1993-01-01 through 1997-10-01.
    std::uniform_int_distribution<int> month_offset_dist(0, 57);
    const int offset = month_offset_dist(rng);
    const int year = 1993 + offset / 12;
    const int month = offset % 12 + 1;
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Date, MakeDate(year, month, 1))};
    break;
  }
  case 5: {
    std::uniform_int_distribution<int> year_dist(1992, 1997);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, std::string{Pick(rng, kRegions)}),
        core::AuxValue::Of(core::AuxType::Date,
                           MakeDate(year_dist(rng), 1, 1))};
    break;
  }
  case 7: {
    std::string nation1 = Pick(rng, kNations);
    std::string nation2 = Pick(rng, kNations);
    while (nation2 == nation1) {
      nation2 = Pick(rng, kNations);
    }
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, std::move(nation1)),
        core::AuxValue::Of(core::AuxType::String, std::move(nation2))};
    break;
  }
  case 8: {
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, std::string{Pick(rng, kNations)}),
        core::AuxValue::Of(core::AuxType::String, std::string{Pick(rng, kRegions)}),
        core::AuxValue::Of(core::AuxType::String,
                           std::string{Pick(rng, kTypeSyllable3)})};
    break;
  }
  case 6: {
    std::uniform_int_distribution<int> year_dist(1993, 1997);
    std::uniform_int_distribution<int> discount_cent_dist(2, 9);
    std::uniform_int_distribution<int> quantity_dist(24, 25);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Date,
                           MakeDate(year_dist(rng), 1, 1)),
        core::AuxValue::Of(core::AuxType::Double,
                           discount_cent_dist(rng) / 100.0),
        core::AuxValue::Of(core::AuxType::Double,
                           static_cast<double>(quantity_dist(rng)))};
    break;
  }
  case 9: {
    arguments.values = {core::AuxValue::Of(
        core::AuxType::String, std::string{Pick(rng, kColors)})};
    break;
  }
  case 10: {
    // First day of month in [1993-02 .. 1995-01].
    std::uniform_int_distribution<int> month_offset_dist(0, 23);
    const int offset = month_offset_dist(rng);
    const int year = 1993 + (1 + offset) / 12;
    const int month = (1 + offset) % 12 + 1;
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Date, MakeDate(year, month, 1))};
    break;
  }
  case 11: {
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, std::string{Pick(rng, kNations)}),
        core::AuxValue::Of(core::AuxType::Double, Q11Fraction())};
    break;
  }
  case 12: {
    std::uniform_int_distribution<int> year_dist(1993, 1997);
    std::string mode1 = Pick(rng, kShipmodes);
    std::string mode2 = Pick(rng, kShipmodes);
    while (mode2 == mode1) {
      mode2 = Pick(rng, kShipmodes);
    }
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, std::move(mode1)),
        core::AuxValue::Of(core::AuxType::String, std::move(mode2)),
        core::AuxValue::Of(core::AuxType::Date,
                           MakeDate(year_dist(rng), 1, 1))};
    break;
  }
  case 13: {
    arguments.values = {core::AuxValue::Of(
        core::AuxType::String, std::string{Pick(rng, kQ13CommentPatterns)})};
    break;
  }
  case 20: {
    std::uniform_int_distribution<int> year_dist(1993, 1997);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Date, MakeDate(year_dist(rng), 1, 1)),
        core::AuxValue::Of(core::AuxType::String,
                           std::string{Pick(rng, kColors)} + "%"),
        core::AuxValue::Of(core::AuxType::String, std::string{Pick(rng, kNations)})};
    break;
  }
  case 14: {
    std::uniform_int_distribution<int> month_offset_dist(0, 59);
    const int offset = month_offset_dist(rng);
    const int year = 1993 + offset / 12;
    const int month = offset % 12 + 1;
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Date, MakeDate(year, month, 1))};
    break;
  }
  case 15: {
    std::uniform_int_distribution<int> offset_dist(0, 26);
    const int offset = offset_dist(rng);
    const int year = 1993 + offset / 12;
    const int month = offset % 12 + 1;
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Date, MakeDate(year, month, 1))};
    break;
  }
  case 16: {
    std::uniform_int_distribution<int> digit_dist(1, 5);
    std::ostringstream brand;
    brand << "Brand#" << digit_dist(rng) << digit_dist(rng);
    std::string type_pattern = std::string{Pick(rng, kTypeSyllable1)} + " " +
                               Pick(rng, kTypeSyllable2) + "%";
    std::uniform_int_distribution<int32_t> size_dist(1, 50);
    std::vector<int32_t> sizes;
    sizes.reserve(8);
    while (sizes.size() < 8) {
      const int32_t size = size_dist(rng);
      if (std::find(sizes.begin(), sizes.end(), size) == sizes.end())
        sizes.push_back(size);
    }
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, brand.str()),
        core::AuxValue::Of(core::AuxType::String, std::move(type_pattern))};
    for (const auto size : sizes)
      arguments.values.push_back(
          core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(size)));
    break;
  }
  case 17: {
    std::uniform_int_distribution<int> digit_dist(1, 5);
    std::ostringstream brand;
    brand << "Brand#" << digit_dist(rng) << digit_dist(rng);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, brand.str()),
        core::AuxValue::Of(core::AuxType::String,
                           std::string{Pick(rng, kContainers)})};
    break;
  }
  case 18: {
    std::uniform_int_distribution<int64_t> dist(312, 315);
    arguments.values = {
        core::AuxValue::Of(core::AuxType::Int64, dist(rng))};
    break;
  }
  case 19: {
    std::uniform_int_distribution<int> digit_dist(1, 5);
    std::uniform_int_distribution<int> qty_dist(1, 30);
    auto make_brand = [&]() {
      std::ostringstream brand;
      brand << "Brand#" << digit_dist(rng) << digit_dist(rng);
      return brand.str();
    };
    arguments.values = {
        core::AuxValue::Of(core::AuxType::String, make_brand()),
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(qty_dist(rng))),
        core::AuxValue::Of(core::AuxType::String, make_brand()),
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(qty_dist(rng))),
        core::AuxValue::Of(core::AuxType::String, make_brand()),
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(qty_dist(rng)))};
    break;
  }
  case 21: {
    arguments.values = {core::AuxValue::Of(
        core::AuxType::String, std::string{Pick(rng, kNations)})};
    break;
  }
  case 22: {
    arguments.values.clear();
    for (int i = 0; i < 7; ++i)
      arguments.values.push_back(core::AuxValue::Of(
          core::AuxType::String,
          std::string{Pick(rng, kCountryCodes)}));
    break;
  }
  default:
    throw std::runtime_error("SampleTpchArgs: unsupported query_id " +
                             std::to_string(query_id));
  }
  return arguments;
}

std::string FormatTpchBaseSql(int query_id,
                              const core::QueryArguments &arguments) {
  switch (query_id) {
  case 1: {
    const auto delta = RequireInt64(arguments, 0);
    return "SELECT l_returnflag, l_linestatus, "
           "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
           "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
           "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
           "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
           "avg(l_discount) AS avg_disc, count(*) AS count_order "
           "FROM lineitem "
           "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '" +
           std::to_string(delta) +
           "' DAY "
           "GROUP BY l_returnflag, l_linestatus "
           "ORDER BY l_returnflag, l_linestatus";
  }
  case 2: {
    const auto size = RequireInt64(arguments, 0);
    const auto &type_pattern = RequireString(arguments, 1);
    const auto &region = RequireString(arguments, 2);
    return "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, "
           "s_phone, s_comment "
           "FROM part, supplier, partsupp, nation, region "
           "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = " +
           std::to_string(size) + " AND p_type LIKE " + SqlString(type_pattern) +
           " AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
           "AND r_name = " +
           SqlString(region) +
           " AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, "
           "supplier, nation, region "
           "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey "
           "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
           "AND r_name = " +
           SqlString(region) +
           ") ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100";
  }
  case 4: {
    const auto &date = RequireDate(arguments, 0);
    const auto date_sql = DateLiteral(date);
    return "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
           "FROM orders WHERE o_orderdate >= " + date_sql +
           " AND o_orderdate < " + date_sql +
           " + INTERVAL '3' MONTH AND EXISTS (SELECT * FROM lineitem "
           "WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate) "
           "GROUP BY o_orderpriority ORDER BY o_orderpriority";
  }
  case 5: {
    const auto &region = RequireString(arguments, 0);
    const auto &date = RequireDate(arguments, 1);
    return "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
           "FROM customer, orders, lineitem, supplier, nation, region "
           "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
           "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
           "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
           "AND r_name = " + SqlString(region) +
           " AND o_orderdate >= " + DateLiteral(date) +
           " AND o_orderdate < " + DateLiteral(date) + " + INTERVAL '1' year "
           "GROUP BY n_name ORDER BY revenue DESC";
  }
  case 7: {
    const auto &nation1 = RequireString(arguments, 0);
    const auto &nation2 = RequireString(arguments, 1);
    return "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM ("
           "SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, "
           "EXTRACT(year FROM l_shipdate) AS l_year, "
           "l_extendedprice * (1 - l_discount) AS volume "
           "FROM supplier, lineitem, orders, customer, nation n1, nation n2 "
           "WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
           "AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey "
           "AND c_nationkey = n2.n_nationkey "
           "AND ((n1.n_name = " +
           SqlString(nation1) + " AND n2.n_name = " + SqlString(nation2) +
           ") OR (n1.n_name = " + SqlString(nation2) + " AND n2.n_name = " +
           SqlString(nation1) +
           ")) "
           "AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') AS shipping "
           "GROUP BY supp_nation, cust_nation, l_year "
           "ORDER BY supp_nation, cust_nation, l_year";
  }
  case 8: {
    const auto &nation = RequireString(arguments, 0);
    const auto &region = RequireString(arguments, 1);
    const auto &part_type = RequireString(arguments, 2);
    return "SELECT o_year, SUM(CASE WHEN nation = " + SqlString(nation) +
           " THEN volume ELSE 0 END) / SUM(volume) AS mkt_share FROM ("
           "SELECT EXTRACT(year FROM o_orderdate) AS o_year, "
           "l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation "
           "FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
           "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey "
           "AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
           "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey "
           "AND r_name = " +
           SqlString(region) +
           " AND s_nationkey = n2.n_nationkey "
           "AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
           "AND p_type = " +
           SqlString(part_type) +
           ") AS all_nations "
           "GROUP BY o_year ORDER BY o_year";
  }
  case 3: {
    const auto &segment = RequireString(arguments, 0);
    const auto &date = RequireDate(arguments, 1);
    const auto date_sql = DateLiteral(date);
    return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
           "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
           "o_orderdate, o_shippriority "
           "FROM customer, orders, lineitem "
           "WHERE c_mktsegment = " +
           SqlString(segment) +
           " AND c_custkey = o_custkey AND "
           "l_orderkey = o_orderkey "
           "AND o_orderdate < " +
           date_sql + " AND l_shipdate > " + date_sql +
           " "
           "GROUP BY l_orderkey, o_orderdate, o_shippriority "
           "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)";
  }
  case 6: {
    const auto &date = RequireDate(arguments, 0);
    const double discount = RequireDouble(arguments, 1);
    const double quantity = RequireDouble(arguments, 2);
    const double lower = discount - 0.01;
    const double upper = discount + 0.01;
    return "SELECT SUM(l_extendedprice * l_discount) AS revenue "
           "FROM lineitem "
           "WHERE l_shipdate >= " +
           DateLiteral(date) + " AND l_shipdate < " + DateLiteral(date) +
           " + INTERVAL '1' year "
           "AND l_discount BETWEEN " +
           FormatDouble(lower) + " AND " + FormatDouble(upper) +
           " AND l_quantity < " + FormatDouble(quantity);
  }
  case 9: {
    const auto &color = RequireString(arguments, 0);
    return "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
           "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
           "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
           "FROM part, supplier, lineitem, partsupp, orders, nation "
           "WHERE s_suppkey = l_suppkey "
           "AND ps_suppkey = l_suppkey "
           "AND ps_partkey = l_partkey "
           "AND p_partkey = l_partkey "
           "AND o_orderkey = l_orderkey "
           "AND s_nationkey = n_nationkey "
           "AND p_name LIKE '%" +
           color +
           "%'"
           ") AS profit "
           "GROUP BY nation, o_year "
           "ORDER BY nation, o_year DESC";
  }
  case 10: {
    const auto &date = RequireDate(arguments, 0);
    const auto date_sql = DateLiteral(date);
    return "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS "
           "revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
           "FROM customer JOIN orders ON c_custkey = o_custkey "
           "JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON "
           "c_nationkey = n_nationkey "
           "WHERE o_orderdate >= " +
           date_sql + " AND o_orderdate < " + date_sql +
           " + INTERVAL '3' MONTH AND "
           "l_returnflag = 'R' "
           "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, "
           "c_comment ORDER BY revenue DESC, c_custkey LIMIT 20";
  }
  case 11: {
    const auto &nation = RequireString(arguments, 0);
    const double fraction = RequireDouble(arguments, 1);
    const auto nation_sql = SqlString(nation);
    const auto frac_sql = FormatDouble(fraction);
    return "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value "
           "FROM partsupp, supplier, nation "
           "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
           "AND n_name = " +
           nation_sql +
           " "
           "GROUP BY ps_partkey "
           "HAVING SUM(ps_supplycost * ps_availqty) > ("
           "SELECT SUM(ps_supplycost * ps_availqty) * " +
           frac_sql +
           " "
           "FROM partsupp, supplier, nation "
           "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
           "AND n_name = " +
           nation_sql +
           ") "
           "ORDER BY value DESC";
  }
  case 12: {
    const auto &mode1 = RequireString(arguments, 0);
    const auto &mode2 = RequireString(arguments, 1);
    const auto &date = RequireDate(arguments, 2);
    const auto date_sql = DateLiteral(date);
    return "SELECT l_shipmode, "
           "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' "
           "THEN 1 ELSE 0 END) AS high_line_count, "
           "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> "
           "'2-HIGH' THEN 1 ELSE 0 END) AS low_line_count "
           "FROM orders, lineitem "
           "WHERE o_orderkey = l_orderkey AND l_shipmode IN (" +
           SqlString(mode1) + ", " + SqlString(mode2) +
           ") "
           "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate "
           "AND l_receiptdate >= " +
           date_sql + " AND l_receiptdate < " + date_sql +
           " + INTERVAL '1' year "
           "GROUP BY l_shipmode ORDER BY l_shipmode";
  }
  case 13: {
    const auto &pattern = RequireString(arguments, 0);
    return "SELECT c_count, COUNT(*) AS custdist FROM ("
           "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
           "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE " +
           SqlString(pattern) +
           " GROUP BY c_custkey) AS c_orders "
           "GROUP BY c_count ORDER BY custdist DESC, c_count DESC";
  }
  case 20: {
    const auto &date = RequireDate(arguments, 0);
    const auto &pattern = RequireString(arguments, 1);
    const auto &nation = RequireString(arguments, 2);
    const auto date_sql = DateLiteral(date);
    return "SELECT s_name, s_address "
           "FROM supplier, nation "
           "WHERE s_suppkey IN ("
           "SELECT ps_suppkey FROM partsupp, ("
           "SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
           "FROM lineitem WHERE l_shipdate >= " +
           date_sql + " AND l_shipdate < " + date_sql +
           " + INTERVAL '1' year "
           "GROUP BY l_partkey, l_suppkey) agg_lineitem "
           "WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
           "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE " +
           SqlString(pattern) +
           ") AND ps_availqty > agg_quantity) "
           "AND s_nationkey = n_nationkey AND n_name = " +
           SqlString(nation) + " ORDER BY s_name";
  }
  case 14: {
    const auto &date = RequireDate(arguments, 0);
    const auto date_sql = DateLiteral(date);
    return "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
           "(1 - l_discount) ELSE 0 END) / "
           "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
           "FROM lineitem, part "
           "WHERE l_partkey = p_partkey "
           "AND l_shipdate >= " +
           date_sql + " AND l_shipdate < " + date_sql +
           " + "
           "INTERVAL '1' month";
  }
  case 15: {
    const auto &date = RequireDate(arguments, 0);
    const auto date_sql = DateLiteral(date);
    return "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue "
           "FROM supplier JOIN (SELECT l_suppkey AS supplier_no, "
           "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
           "FROM lineitem WHERE l_shipdate >= " + date_sql +
           " AND l_shipdate < " + date_sql + " + INTERVAL '3' MONTH "
           "GROUP BY l_suppkey) revenue ON s_suppkey = supplier_no "
           "WHERE total_revenue = (SELECT MAX(total_revenue) FROM ("
           "SELECT l_suppkey AS supplier_no, "
           "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
           "FROM lineitem WHERE l_shipdate >= " + date_sql +
           " AND l_shipdate < " + date_sql + " + INTERVAL '3' MONTH "
           "GROUP BY l_suppkey) revenue_max) ORDER BY s_suppkey";
  }
  case 16: {
    const auto &brand = RequireString(arguments, 0);
    const auto &type_pattern = RequireString(arguments, 1);
    std::ostringstream sql;
    sql << "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS "
           "supplier_cnt "
           "FROM partsupp, part "
           "WHERE p_partkey = ps_partkey AND p_brand <> "
        << SqlString(brand) << " AND p_type NOT LIKE "
        << SqlString(type_pattern) << " AND p_size IN (";
    for (int i = 0; i < 8; ++i) {
      if (i != 0)
        sql << ", ";
      sql << RequireInt64(arguments, static_cast<std::size_t>(2 + i));
    }
    sql << ") AND ps_suppkey NOT IN ("
           "SELECT s_suppkey FROM supplier WHERE s_comment LIKE "
           "'%Customer%Complaints%') "
           "GROUP BY p_brand, p_type, p_size "
           "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size";
    return sql.str();
  }
  case 17: {
    const auto &brand = RequireString(arguments, 0);
    const auto &container = RequireString(arguments, 1);
    return "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
           "FROM lineitem, part, "
           "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
           "FROM lineitem GROUP BY l_partkey) part_agg "
           "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
           "AND p_brand = " +
           SqlString(brand) + " AND p_container = " + SqlString(container) +
           " "
           "AND l_quantity < avg_quantity";
  }
  case 18: {
    const auto threshold = RequireInt64(arguments, 0);
    return "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, "
           "SUM(l_quantity) AS sum_qty "
           "FROM customer, orders, lineitem "
           "WHERE o_orderkey IN ("
           "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
           "HAVING SUM(l_quantity) > " +
           std::to_string(threshold) +
           ") "
           "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
           "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
           "ORDER BY o_totalprice DESC, o_orderdate ASC";
  }
  case 21: {
    const auto &nation = RequireString(arguments, 0);
    return "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
           "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
           "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
           "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
           "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
           "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
           "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
           "n_name = " +
           SqlString(nation) +
           " GROUP BY s_name ORDER BY numwait DESC, s_name";
  }
  case 22: {
    std::ostringstream sql;
    sql << "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
           "FROM (SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
           "FROM customer "
           "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN (";
    for (int i = 0; i < 7; ++i) {
      if (i > 0) sql << ", ";
      sql << SqlString(RequireString(arguments, i));
    }
    sql << ") AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer "
           "WHERE c_acctbal > 0.00 AND SUBSTRING(c_phone FROM 1 FOR 2) IN (";
    for (int i = 0; i < 7; ++i) {
      if (i > 0) sql << ", ";
      sql << SqlString(RequireString(arguments, i));
    }
    sql << ")) AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
           "AS custsale GROUP BY cntrycode ORDER BY cntrycode";
    return sql.str();
  }
case 19: {
    const auto &b1 = RequireString(arguments, 0);
    const auto &b2 = RequireString(arguments, 2);
    const auto &b3 = RequireString(arguments, 4);
    const int64_t q1 = RequireInt64(arguments, 1);
    const int64_t q2 = RequireInt64(arguments, 3);
    const int64_t q3 = RequireInt64(arguments, 5);
    return "SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue "
           "FROM lineitem, part WHERE (p_partkey = l_partkey AND p_brand = " +
           SqlString(b1) +
           " AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
           "AND l_quantity >= " + std::to_string(q1) +
           " AND l_quantity <= " + std::to_string(q1 + 10) +
           " AND p_size BETWEEN 1 AND 5 AND l_shipmode IN ('AIR', 'AIR REG') "
           "AND l_shipinstruct = 'DELIVER IN PERSON') "
           "OR (p_partkey = l_partkey AND p_brand = " + SqlString(b2) +
           " AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
           "AND l_quantity >= " + std::to_string(q2) +
           " AND l_quantity <= " + std::to_string(q2 + 10) +
           " AND p_size BETWEEN 1 AND 10 AND l_shipmode IN ('AIR', 'AIR REG') "
           "AND l_shipinstruct = 'DELIVER IN PERSON') "
           "OR (p_partkey = l_partkey AND p_brand = " + SqlString(b3) +
           " AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
           "AND l_quantity >= " + std::to_string(q3) +
           " AND l_quantity <= " + std::to_string(q3 + 10) +
           " AND p_size BETWEEN 1 AND 15 AND l_shipmode IN ('AIR', 'AIR REG') "
           "AND l_shipinstruct = 'DELIVER IN PERSON')";
  }

  default:
    throw std::runtime_error("FormatTpchBaseSql: unsupported query_id " +
                             std::to_string(query_id));
  }
}

std::string FormatTpchKernelSql(int query_id,
                                const core::QueryArguments &arguments) {
  switch (query_id) {
  case 1:
    return "SELECT * FROM accel_tpch_q1(" +
           std::to_string(RequireInt64(arguments, 0)) + ")";
  case 4:
    return "SELECT * FROM accel_tpch_q4(" + DateLiteral(RequireDate(arguments, 0)) + ")";
  case 5:
    return "SELECT * FROM accel_tpch_q5(" + SqlString(RequireString(arguments, 0)) +
           ", " + DateLiteral(RequireDate(arguments, 1)) + ")";
    case 2:
    return "SELECT * FROM accel_tpch_q2(" +
           std::to_string(RequireInt64(arguments, 0)) + ", " +
           SqlString(RequireString(arguments, 1)) + ", " +
           SqlString(RequireString(arguments, 2)) + ")";
case 3:
    return "SELECT * FROM accel_tpch_q3(" + SqlString(RequireString(arguments, 0)) +
           ", " + DateLiteral(RequireDate(arguments, 1)) + ")";
  case 6:
    return "SELECT revenue FROM accel_tpch_q6(" +
           DateLiteral(RequireDate(arguments, 0)) + ", " +
           FormatDouble(RequireDouble(arguments, 1)) + ", " +
           FormatDouble(RequireDouble(arguments, 2)) + ")";
  case 7:
    return "SELECT * FROM accel_tpch_q7(" + SqlString(RequireString(arguments, 0)) +
           ", " + SqlString(RequireString(arguments, 1)) + ")";
  case 8:
    return "SELECT * FROM accel_tpch_q8(" + SqlString(RequireString(arguments, 0)) +
           ", " + SqlString(RequireString(arguments, 1)) + ", " +
           SqlString(RequireString(arguments, 2)) + ")";
  case 9:
    return "SELECT * FROM accel_tpch_q9(" + SqlString(RequireString(arguments, 0)) +
           ")";
  case 10:
    return "SELECT * FROM accel_tpch_q10(" + DateLiteral(RequireDate(arguments, 0)) +
           ")";
  case 11:
    return "SELECT * FROM accel_tpch_q11(" + SqlString(RequireString(arguments, 0)) +
           ", " + FormatDouble(RequireDouble(arguments, 1)) + ")";
  case 12:
    return "SELECT * FROM accel_tpch_q12(" + SqlString(RequireString(arguments, 0)) +
           ", " + SqlString(RequireString(arguments, 1)) + ", " +
           DateLiteral(RequireDate(arguments, 2)) + ")";
  case 13:
    return "SELECT * FROM accel_tpch_q13(" + SqlString(RequireString(arguments, 0)) +
           ")";
  case 14:
    return "SELECT * FROM accel_tpch_q14(" + DateLiteral(RequireDate(arguments, 0)) +
           ")";
  case 15:
    return "SELECT * FROM accel_tpch_q15(" + DateLiteral(RequireDate(arguments, 0)) + ")";
  case 16: {
    std::ostringstream sql;
    sql << "SELECT * FROM accel_tpch_q16("
        << SqlString(RequireString(arguments, 0)) << ", "
        << SqlString(RequireString(arguments, 1));
    for (int i = 0; i < 8; ++i)
      sql << ", " << RequireInt64(arguments, static_cast<std::size_t>(2 + i));
    sql << ")";
    return sql.str();
  }
  case 17:
    return "SELECT * FROM accel_tpch_q17(" + SqlString(RequireString(arguments, 0)) +
           ", " + SqlString(RequireString(arguments, 1)) + ")";
  case 18:
    return "SELECT * FROM accel_tpch_q18(" +
           std::to_string(RequireInt64(arguments, 0)) + ")";
  case 22: {
    std::ostringstream sql;
    sql << "SELECT * FROM accel_tpch_q22(";
    for (int i = 0; i < 7; ++i) {
      if (i > 0) sql << ", ";
      sql << SqlString(RequireString(arguments, i));
    }
    sql << ")";
    return sql.str();
  }
  case 21:
    return "SELECT * FROM accel_tpch_q21(" + SqlString(RequireString(arguments, 0)) +
           ")";
  case 19:
    return "SELECT * FROM accel_tpch_q19(" +
           SqlString(RequireString(arguments, 0)) + ", " +
           std::to_string(RequireInt64(arguments, 1)) + ", " +
           SqlString(RequireString(arguments, 2)) + ", " +
           std::to_string(RequireInt64(arguments, 3)) + ", " +
           SqlString(RequireString(arguments, 4)) + ", " +
           std::to_string(RequireInt64(arguments, 5)) + ")";
  case 20:
    return "SELECT * FROM accel_tpch_q20(" + DateLiteral(RequireDate(arguments, 0)) +
           ", " + SqlString(RequireString(arguments, 1)) + ", " +
           SqlString(RequireString(arguments, 2)) + ")";
  default:
    throw std::runtime_error("FormatTpchKernelSql: unsupported query_id " +
                             std::to_string(query_id));
  }
}

std::string FormatTpchAuxSql(int query_id,
                             const core::QueryArguments &arguments) {
  std::string sql = FormatTpchKernelSql(query_id, arguments);
  const auto paren = sql.find('(');
  if (paren == std::string::npos || paren == 0) {
    throw std::runtime_error("FormatTpchAuxSql: malformed kernel sql");
  }
  // accel_tpch_qN(...) -> accel_tpch_qN_aux(...)
  sql.insert(paren, "_aux");
  return sql;
}

std::vector<TpchArgVariant> BuildTpchArgPool(
    int query_id, const catalog_ns::SharedQueryCatalogEntry &entry) {
  std::vector<TpchArgVariant> pool;
  if (!VaryArgsEnabled()) {
    TpchArgVariant variant;
    variant.arguments = catalog_ns::ToCoreArguments(entry.benchmark_arguments);
    variant.base_sql = entry.base_sql ? entry.base_sql : "";
    variant.kernel_sql =
        entry.postgres_base_kernel_sql ? entry.postgres_base_kernel_sql : "";
    variant.aux_sql = entry.duckdb_aux_sql ? entry.duckdb_aux_sql : "";
    pool.push_back(std::move(variant));
    return pool;
  }

  const std::size_t count = ArgPoolSize();
  pool.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    TpchArgVariant variant;
    variant.arguments = SampleTpchArgs(query_id, i);
    variant.base_sql = FormatTpchBaseSql(query_id, variant.arguments);
    variant.kernel_sql = FormatTpchKernelSql(query_id, variant.arguments);
    variant.aux_sql = FormatTpchAuxSql(query_id, variant.arguments);
    pool.push_back(std::move(variant));
  }
  return pool;
}

} // namespace accelerator_codegen::benchmarks::aux_sweep
