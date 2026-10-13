#include "tpch/q11/mysql/aux/module.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/mysql_collation.hpp"
#include "accelerator_codegen/stateful/prims/pg_numeric.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using U128 = unsigned __int128;

// 256-bit unsigned product, so `value * 10^scale` and `total * coeff` compare
// exactly whatever the data / fraction.
struct U256 {
  U128 hi = 0, lo = 0;
  bool operator>(const U256 &o) const {
    return hi != o.hi ? hi > o.hi : lo > o.lo;
  }
};

U256 Mul(U128 a, U128 b) {
  const uint64_t a0 = static_cast<uint64_t>(a), a1 = static_cast<uint64_t>(a >> 64);
  const uint64_t b0 = static_cast<uint64_t>(b), b1 = static_cast<uint64_t>(b >> 64);
  const U128 p00 = static_cast<U128>(a0) * b0, p01 = static_cast<U128>(a0) * b1;
  const U128 p10 = static_cast<U128>(a1) * b0, p11 = static_cast<U128>(a1) * b1;
  const U128 mid = (p00 >> 64) + static_cast<uint64_t>(p01) + static_cast<uint64_t>(p10);
  U256 out;
  out.lo = static_cast<uint64_t>(p00) | (mid << 64);
  out.hi = p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
  return out;
}

U128 Abs(Q11AccT v) { return v < 0 ? static_cast<U128>(-(v + 1)) + 1 : static_cast<U128>(v); }
int Sign(Q11AccT v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }

U128 Pow10(int n) {
  U128 p = 1;
  while (n-- > 0)
    p *= 10;
  return p;
}

// HAVING value > total * fraction, evaluated exactly in decimal:
//   value * 10^scale  >  total * coeff.
class ThresholdTest {
public:
  ThresholdTest(Q11AccT total, const TpchQ11Fraction &f) : total_(total), f_(f) {
    if (!f.nan && f.infinity == 0) {
      scale_pow_ = Pow10(f.scale);
      rhs_mag_ = Mul(Abs(total), Abs(f.coeff));
      rhs_sign_ = rhs_mag_.hi == 0 && rhs_mag_.lo == 0
                      ? 0
                      : Sign(total) * Sign(f.coeff);
    }
  }
  bool Above(Q11AccT value) const {
    if (f_.nan)
      return false;
    if (f_.infinity != 0) {
      // total * +/-Infinity is +/-Infinity (NaN for a zero total).
      const int t = Sign(total_) * f_.infinity;
      return t < 0;
    }
    const U256 lhs = Mul(Abs(value), scale_pow_);
    const int lhs_sign = lhs.hi == 0 && lhs.lo == 0 ? 0 : Sign(value);
    if (lhs_sign != rhs_sign_)
      return lhs_sign > rhs_sign_;
    if (lhs_sign == 0)
      return false;
    return lhs_sign > 0 ? lhs > rhs_mag_ : rhs_mag_ > lhs;
  }

private:
  Q11AccT total_;
  TpchQ11Fraction f_;
  U128 scale_pow_ = 1;
  U256 rhs_mag_;
  int rhs_sign_ = 0;
};

core::AuxRow MakeRow(int64_t partkey, Q11AccT value) {
  core::AuxRow row;
  row.values.reserve(2);
  row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, partkey));
  if (value >= std::numeric_limits<int64_t>::min() &&
      value <= std::numeric_limits<int64_t>::max()) {
    row.values.push_back(core::AuxValue::Of(
        core::AuxType::ScaledDecimal,
        core::ScaledDecimal{static_cast<int64_t>(value), 2}));
  } else {
    // Beyond int64: exact decimal text.
    row.values.push_back(core::AuxValue::Of(
        core::AuxType::String, pg_numeric::ToText(value, 2)));
  }
  return row;
}

} // namespace

bool TpchQ11ParseFraction(std::string_view text, TpchQ11Fraction &out) {
  out = TpchQ11Fraction{};
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  bool negative = false;
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  const auto ieq = [&](const char *word) {
    std::string_view w(word);
    if (text.size() != w.size())
      return false;
    for (std::size_t i = 0; i < w.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(text[i])) != w[i])
        return false;
    return true;
  };
  if (ieq("nan")) {
    out.nan = true;
    return true;
  }
  if (ieq("infinity") || ieq("inf")) {
    out.infinity = negative ? -1 : 1;
    return true;
  }
  std::string digits;
  int frac_digits = 0;
  bool seen_point = false, seen_digit = false;
  std::size_t i = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '.' && !seen_point) {
      seen_point = true;
    } else if (c >= '0' && c <= '9') {
      seen_digit = true;
      digits.push_back(c);
      frac_digits += seen_point;
    } else {
      break;
    }
  }
  if (!seen_digit)
    return false;
  long exponent = 0;
  if (i < text.size()) {
    if (text[i] != 'e' && text[i] != 'E')
      return false;
    char *end = nullptr;
    const std::string tail(text.substr(i + 1));
    exponent = std::strtol(tail.c_str(), &end, 10);
    if (tail.empty() || *end != '\0' || exponent < -400 || exponent > 400)
      return false;
  }
  // value = digits * 10^-(frac_digits - exponent)
  long scale = frac_digits - exponent;
  digits.erase(0, std::min(digits.find_first_not_of('0'), digits.size()));
  while (scale > 0 && !digits.empty() && digits.back() == '0') {
    digits.pop_back();
    --scale;
  }
  if (digits.empty()) {
    out.coeff = 0;
    out.scale = 0;
    return true;
  }
  if (scale < 0) {
    digits.append(static_cast<std::size_t>(-scale), '0');
    scale = 0;
  }
  // Keep at most 38 significant digits (round half up), and scale <= 38.
  const auto drop = [&](std::size_t n) {
    const bool up = n <= digits.size() && digits[digits.size() - n] >= '5';
    digits.resize(digits.size() > n ? digits.size() - n : 0);
    scale -= static_cast<long>(n);
    if (up) { // add one unit in the last place
      std::size_t k = digits.size();
      while (k > 0 && digits[k - 1] == '9')
        digits[--k] = '0';
      if (k == 0)
        digits.insert(digits.begin(), '1');
      else
        ++digits[k - 1];
    }
  };
  if (digits.size() > 38)
    drop(digits.size() - 38);
  if (scale > 38)
    drop(static_cast<std::size_t>(scale - 38));
  if (scale < 0) {
    digits.append(static_cast<std::size_t>(-scale), '0');
    scale = 0;
    if (digits.size() > 38)
      return false; // coefficient does not fit (fraction >= 1e38)
  }
  Q11AccT coeff = 0;
  for (const char c : digits)
    coeff = coeff * 10 + (c - '0');
  out.coeff = negative ? -coeff : coeff;
  out.scale = static_cast<int>(scale);
  return true;
}

void TpchQ11FinalizeAndEmit(const TpchQ11State &state, const TpchQ11Args &args,
                            core::AuxRowSink &sink) {
  sink.Begin(core::OutputSchema{});
  const auto finish = [&] { sink.End(); };
  if (!args.nation || !args.fraction) {
    finish();
    return;
  }

  // Groups of every nation named :nation (normally exactly one).
  std::vector<const TpchQ11Group *> groups;
  for (const auto &nation : *state.nations)
    if (mysql_collation::EqualsCi(nation.name, *args.nation))
      if (const auto *group = state.FindGroup(nation.key))
        if (group->parts != 0)
          groups.push_back(group);
  if (groups.empty()) {
    finish();
    return;
  }

  if (groups.size() == 1) {
    // Walk the ordered entries (wide+, narrow, wide-) while they exceed the
    // threshold: values only decrease, so the first miss ends the scan.
    const TpchQ11Group &group = *groups[0];
    const ThresholdTest test(group.total, *args.fraction);
    bool more = true;
    const auto emit = [&](int64_t partkey, Q11AccT value) {
      if (!test.Above(value))
        return more = false;
      sink.WriteRow(MakeRow(partkey, value));
      return true;
    };
    for (const auto &key : group.wide_positive)
      if (!emit(key.partkey, key.value))
        break;
    if (more)
      group.narrow.ForEach([&](const TpchQ11Key &key) {
        return emit(key.partkey, key.value);
      });
    if (more)
      for (const auto &key : group.wide_negative)
        if (!emit(key.partkey, key.value))
          break;
    finish();
    return;
  }

  // Several nations share the name: a part's value is its sum over all of them.
  Q11AccT total = 0;
  std::map<int64_t, Q11AccT> by_part;
  for (const auto *group : groups) {
    total += group->total;
    const auto add = [&](int64_t partkey, Q11AccT value) {
      by_part[partkey] += value;
      return true;
    };
    for (const auto &key : group->wide_positive)
      add(key.partkey, key.value);
    group->narrow.ForEach(
        [&](const TpchQ11Key &key) { return add(key.partkey, key.value); });
    for (const auto &key : group->wide_negative)
      add(key.partkey, key.value);
  }
  std::vector<TpchQ11OutputRow> rows;
  const ThresholdTest test(total, *args.fraction);
  for (const auto &[partkey, value] : by_part)
    if (test.Above(value))
      rows.push_back({partkey, value});
  std::sort(rows.begin(), rows.end(),
            [](const TpchQ11OutputRow &a, const TpchQ11OutputRow &b) {
              return a.value != b.value ? a.value > b.value : a.partkey < b.partkey;
            });
  for (const auto &row : rows)
    sink.WriteRow(MakeRow(row.partkey, row.value));
  finish();
}

} // namespace accelerator_codegen::aux_state::query
