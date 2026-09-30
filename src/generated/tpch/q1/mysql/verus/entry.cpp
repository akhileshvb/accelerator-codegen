// mysql_accel_tpch_q1_verus(day_offset): stateless TPC-H Q1 on MySQL,
// aggregated by the Verus-verified kernel (src/verus/q1).  Returns the result
// as text, one `|`-separated row per line (like the other MySQL UDFs).
//
// TRUSTED GLUE.  A UDF has no storage-level scan API, so the rows come from a
// client scan over the mysql CLI (the same mechanism as the Q21 Verus scans;
// committed data only):
//   SELECT TO_DAYS(l_shipdate), l_quantity, l_extendedprice, l_discount, l_tax,
//          l_returnflag, l_linestatus
//   FROM lineitem WHERE l_shipdate <= <cutoff>
// DECIMAL(15,2) text is parsed exactly into hundredths (no floating point);
// TO_DAYS gives the day numbers and the cutoff uses the same encoding.  The
// output is formatted as MySQL does: SUM columns at scales 2, 2, 4, 6 and
// AVG(DECIMAL(15,2)) at scale 2 + div_precision_increment (4), rounded half
// away from zero.  The kernel groups and orders by bytes; the table's
// collation is utf8mb4_0900_ai_ci, which agrees with byte order and byte
// equality on upper-case ASCII letters, so the entry point requires every
// result flag to be one byte in 'A'..'Z'.  Any row outside that, text that is
// not an exact two-decimal number, or a kernel refusal falls back to the
// catalog SQL.
#include "accelerator_codegen/mysql/plugin_minimal.h"
#include "accelerator_codegen/stateful/core/pg_numeric.hpp"
#include "accelerator_codegen/verus/q1_verus.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace pgn = accelerator_codegen::pg_numeric;
using accelerator_codegen::verus_q1::Q1Batch;
using accelerator_codegen::verus_q1::Q1Kernel;

constexpr unsigned long kMaxResult = 1u << 20;
constexpr int32_t kToDays19981201 = 730089; // TO_DAYS('1998-12-01')

std::string CliPrefix() {
  const char *host = std::getenv("MYSQL_HOST");
  const char *port = std::getenv("MYSQL_PORT");
  const char *user = std::getenv("MYSQL_USER");
  const char *password = std::getenv("MYSQL_PWD");
  const char *db = std::getenv("TPCH_MYSQL_DB");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3306";
  if (!user || !*user) user = "root";
  if (!password) password = "mysql";
  if (!db || !*db) db = "accel_mysql_test";
  return std::string("MYSQL_PWD=") + password + " mysql -h " + host + " -P " + port + " -u" + user + " -D " + db +
         " --batch --raw --skip-column-names -e ";
}

std::string Q1Sql(long long off) {
  return "SELECT l_returnflag, l_linestatus, sum(l_quantity), sum(l_extendedprice), "
         "sum(l_extendedprice * (1 - l_discount)), sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)), "
         "avg(l_quantity), avg(l_extendedprice), avg(l_discount), count(*) FROM lineitem "
         "WHERE l_shipdate <= DATE_SUB(DATE '1998-12-01', INTERVAL " +
         std::to_string(off) + " DAY) GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";
}

// Exact "[-]digits[.d[d]]" -> hundredths.
bool ParseHundredths(const char *s, const char *e, int64_t *out) {
  bool neg = false;
  if (s < e && *s == '-') {
    neg = true;
    ++s;
  }
  if (s == e) {
    return false;
  }
  int64_t v = 0;
  int ndig = 0;
  while (s < e && *s >= '0' && *s <= '9') {
    if (++ndig > 16) {
      return false;
    }
    v = v * 10 + (*s++ - '0');
  }
  int frac = 0;
  if (s < e && *s == '.') {
    ++s;
    while (s < e && *s >= '0' && *s <= '9') {
      if (frac == 2) {
        if (*s != '0') {
          return false;
        }
        ++s;
        continue;
      }
      v = v * 10 + (*s++ - '0');
      ++frac;
    }
  }
  if (s != e) {
    return false;
  }
  for (; frac < 2; ++frac) {
    v *= 10;
  }
  *out = neg ? -v : v;
  return true;
}

bool ParseInt(const char *s, const char *e, int32_t *out) {
  bool neg = false;
  if (s < e && *s == '-') {
    neg = true;
    ++s;
  }
  if (s == e || e - s > 9) {
    return false;
  }
  int32_t v = 0;
  for (; s < e; ++s) {
    if (*s < '0' || *s > '9') {
      return false;
    }
    v = v * 10 + (*s - '0');
  }
  *out = neg ? -v : v;
  return true;
}

// Scan lineitem through the CLI into the kernel.  False: use SQL.
bool RunKernel(long long off, std::vector<q1v_group> &groups) {
  const int32_t cutoff = kToDays19981201 - static_cast<int32_t>(off);
  const std::string sql = "SELECT TO_DAYS(l_shipdate), l_quantity, l_extendedprice, l_discount, l_tax, "
                          "l_returnflag, l_linestatus FROM lineitem WHERE l_shipdate <= DATE_SUB(DATE '1998-12-01', "
                          "INTERVAL " +
                          std::to_string(off) + " DAY)";
  const std::string cmd = CliPrefix() + "\"" + sql + "\" 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return false;
  }
  Q1Kernel kernel(cutoff);
  Q1Batch batch(2048);
  size_t n = 0;
  bool ok = true;
  std::vector<char> buf(1 << 20);
  std::string carry;
  size_t got;
  auto row = [&](const char *s, const char *e) {
    const char *f[8];
    int nf = 0;
    f[nf++] = s;
    for (const char *p = s; p < e && nf < 8; ++p) {
      if (*p == '\t') {
        f[nf++] = p + 1;
      }
    }
    if (nf != 7) {
      ok = false;
      return;
    }
    auto end_of = [&](int i) { return i + 1 < nf ? f[i + 1] - 1 : e; };
    ok = ParseInt(f[0], end_of(0), &batch.shipdate[n]) && ParseHundredths(f[1], end_of(1), &batch.quantity[n]) &&
         ParseHundredths(f[2], end_of(2), &batch.extendedprice[n]) &&
         ParseHundredths(f[3], end_of(3), &batch.discount[n]) && ParseHundredths(f[4], end_of(4), &batch.tax[n]) &&
         end_of(5) - f[5] == 1 && end_of(6) - f[6] == 1;
    if (!ok) {
      return;
    }
    batch.returnflag[n] = static_cast<uint8_t>(*f[5]);
    batch.linestatus[n] = static_cast<uint8_t>(*f[6]);
    if (++n == batch.Capacity()) {
      batch.n = n;
      ok = kernel.Consume(batch);
      n = 0;
    }
  };
  while (ok && (got = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
    const char *p = buf.data();
    const char *end = p + got;
    while (ok) {
      const char *nl = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
      if (!nl) {
        carry.append(p, end);
        break;
      }
      if (!carry.empty()) {
        carry.append(p, nl);
        row(carry.data(), carry.data() + carry.size());
        carry.clear();
      } else {
        row(p, nl);
      }
      p = nl + 1;
    }
  }
  const int status = pclose(pipe);
  if (!ok || status != 0 || !carry.empty()) {
    return false;
  }
  if (n > 0) {
    batch.n = n;
    if (!kernel.Consume(batch)) {
      return false;
    }
  }
  if (!kernel.Finish(groups)) {
    return false;
  }
  for (const auto &g : groups) {
    if (g.returnflag < 'A' || g.returnflag > 'Z' || g.linestatus < 'A' || g.linestatus > 'Z') {
      return false;
    }
  }
  return true;
}

// sum / count at scale 2 + 4, rounded half away from zero.
std::string MysqlAvg(__int128 sum, int64_t count) {
  const bool neg = sum < 0;
  unsigned __int128 a = static_cast<unsigned __int128>(neg ? -sum : sum) * 10000;
  unsigned __int128 q = a / static_cast<unsigned __int128>(count);
  unsigned __int128 r = a % static_cast<unsigned __int128>(count);
  if (2 * r >= static_cast<unsigned __int128>(count)) {
    q += 1;
  }
  const __int128 coeff = neg ? -static_cast<__int128>(q) : static_cast<__int128>(q);
  return pgn::ToText(coeff, 6);
}

std::string KernelText(const std::vector<q1v_group> &groups) {
  std::string out;
  for (const auto &g : groups) {
    out += static_cast<char>(g.returnflag);
    out += '|';
    out += static_cast<char>(g.linestatus);
    out += '|' + pgn::ToText(g.sum_qty, 2) + '|' + pgn::ToText(g.sum_base_price, 2) + '|' +
           pgn::ToText(g.sum_disc_price, 4) + '|' + pgn::ToText(g.sum_charge, 6) + '|' + MysqlAvg(g.sum_qty, g.count) +
           '|' + MysqlAvg(g.sum_base_price, g.count) + '|' + MysqlAvg(g.sum_disc, g.count) + '|' +
           std::to_string(g.count) + '\n';
  }
  return out;
}

bool SqlText(long long off, std::string &out) {
  const std::string cmd = CliPrefix() + "\"" + Q1Sql(off) + "\" 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return false;
  }
  char buf[4096];
  size_t got;
  while ((got = std::fread(buf, 1, sizeof buf, pipe)) > 0) {
    out.append(buf, got);
  }
  std::replace(out.begin(), out.end(), '\t', '|');
  return pclose(pipe) == 0;
}

} // namespace

extern "C" {

__attribute__((visibility("default"))) bool mysql_accel_tpch_q1_verus_init(UDF_INIT *initid, UDF_ARGS *args,
                                                                            char *message) {
  if (args->arg_count != 1) {
    std::strcpy(message, "mysql_accel_tpch_q1_verus(day_offset) takes one argument");
    return true;
  }
  args->arg_type[0] = INT_RESULT;
  initid->maybe_null = 0;
  initid->max_length = kMaxResult;
  initid->ptr = static_cast<char *>(std::malloc(kMaxResult));
  return initid->ptr == nullptr;
}

__attribute__((visibility("default"))) void mysql_accel_tpch_q1_verus_deinit(UDF_INIT *initid) {
  std::free(initid->ptr);
  initid->ptr = nullptr;
}

__attribute__((visibility("default"))) char *mysql_accel_tpch_q1_verus(UDF_INIT *initid, UDF_ARGS *args, char *,
                                                                        unsigned long *length, unsigned char *,
                                                                        unsigned char *error) {
  const long long off = args->args[0] ? *reinterpret_cast<long long *>(args->args[0]) : 0;
  std::string text;
  std::vector<q1v_group> groups;
  const bool kernel_ok = RunKernel(off, groups);
  if (!kernel_ok) {
    std::fprintf(stderr, "mysql_accel_tpch_q1_verus(%lld): answering with SQL\n", off);
  }
  if (kernel_ok) {
    text = KernelText(groups);
  } else if (!SqlText(off, text)) {
    *error = 1;
    *length = 0;
    return nullptr;
  }
  const size_t n = std::min(text.size(), static_cast<size_t>(kMaxResult - 1));
  std::memcpy(initid->ptr, text.data(), n);
  *length = static_cast<unsigned long>(n);
  return initid->ptr;
}

} // extern "C"
