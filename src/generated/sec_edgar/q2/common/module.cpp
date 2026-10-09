#include "sec_edgar/q2/duckdb/aux/module.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace accelerator_codegen::aux_state::query {
namespace {

struct BuildTrace {
  bool on = std::getenv("ACCEL_Q2_BUILD_TRACE") != nullptr;
  std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
  void Mark(const char *what) {
    if (!on) {
      return;
    }
    const auto n = std::chrono::steady_clock::now();
    std::fprintf(stderr, "q2 build: %-24s %8.1f ms\n", what,
                 std::chrono::duration<double, std::milli>(n - t).count());
    t = n;
  }
};

// ------------------------------------------------------------ row parsing

bool ReadString(const core::AuxRow &row, std::size_t index, std::string &value,
                bool &is_null) {
  if (index >= row.values.size()) {
    return false;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    is_null = true;
    return true;
  }
  if (!std::holds_alternative<std::string>(cell.payload)) {
    return false;
  }
  is_null = false;
  value = std::get<std::string>(cell.payload);
  return true;
}

// Returns a pointer to the string payload or nullptr (NULL / missing / wrong
// type).  `ok` reports whether the cell was readable at all.
const std::string *StringCell(const core::AuxRow &row, std::size_t index,
                              bool &ok) {
  ok = index < row.values.size();
  if (!ok) {
    return nullptr;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    return nullptr;
  }
  if (!std::holds_alternative<std::string>(cell.payload)) {
    ok = false;
    return nullptr;
  }
  return &std::get<std::string>(cell.payload);
}

bool ReadDouble(const core::AuxRow &row, std::size_t index, double &value,
                bool &is_null) {
  if (index >= row.values.size()) {
    return false;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    is_null = true;
    return true;
  }
  is_null = false;
  if (std::holds_alternative<double>(cell.payload)) {
    value = std::get<double>(cell.payload);
    return true;
  }
  if (std::holds_alternative<int64_t>(cell.payload)) {
    value = static_cast<double>(std::get<int64_t>(cell.payload));
    return true;
  }
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
    value = static_cast<double>(decimal.value);
    for (int32_t scale = 0; scale < decimal.scale; ++scale) {
      value /= 10.0;
    }
    return true;
  }
  return false;
}

bool ReadInt(const core::AuxRow &row, std::size_t index, int64_t &value,
             bool &is_null) {
  if (index >= row.values.size()) {
    return false;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    is_null = true;
    return true;
  }
  is_null = false;
  if (std::holds_alternative<int64_t>(cell.payload)) {
    value = std::get<int64_t>(cell.payload);
    return true;
  }
  if (std::holds_alternative<uint64_t>(cell.payload)) {
    value = static_cast<int64_t>(std::get<uint64_t>(cell.payload));
    return true;
  }
  return false;
}

// A parsed num mutation row.  `relevant` is false when the row can never be
// part of any group (NULL adsh / tag / uom / value).
struct NumRowView {
  const std::string *adsh = nullptr;
  const std::string *tag = nullptr;
  const std::string *uom = nullptr;
  int64_t key = 0;
  bool relevant = false;
};

bool ParseNumRow(const core::AuxRow &row, NumRowView &out) {
  bool ok_adsh = false, ok_tag = false, ok_uom = false;
  out.adsh = StringCell(row, kQ2NumAdsh, ok_adsh);
  out.tag = StringCell(row, kQ2NumTag, ok_tag);
  out.uom = StringCell(row, kQ2NumUom, ok_uom);
  double value = 0.0;
  bool value_null = false;
  if (!ok_adsh || !ok_tag || !ok_uom ||
      !ReadDouble(row, kQ2NumValue, value, value_null)) {
    return false;
  }
  out.relevant = out.adsh && out.tag && out.uom && !value_null;
  out.key = out.relevant ? Q2ValueKey(value) : 0;
  return true;
}

struct SubRowView {
  const std::string *adsh = nullptr;
  std::string name;
  bool name_is_null = true;
  int64_t fy = 0;
  bool fy_is_null = true;
};

bool ParseSubRow(const core::AuxRow &row, SubRowView &out) {
  bool ok = false;
  out.adsh = StringCell(row, kQ2SubAdsh, ok);
  if (!ok || out.adsh == nullptr) {
    return false;
  }
  if (!ReadString(row, kQ2SubName, out.name, out.name_is_null)) {
    return false;
  }
  return ReadInt(row, kQ2SubFy, out.fy, out.fy_is_null);
}

// ------------------------------------------------------------ varint codec

void PutStr(std::vector<uint8_t> &out, std::string_view s) {
  core::PutVarint(out, s.size());
  out.insert(out.end(), s.begin(), s.end());
}

void PutRaw64(std::vector<uint8_t> &out, uint64_t v) {
  for (int s = 0; s < 64; s += 8) {
    out.push_back(static_cast<uint8_t>(v >> s));
  }
}

struct Reader {
  const std::vector<uint8_t> &b;
  size_t off = 0;
  bool ok = true;
  uint64_t Var() {
    const uint8_t *p = b.data() + off;
    uint64_t v = 0;
    if (!core::GetVarint(p, b.data() + b.size(), v)) {
      ok = false;
      return 0;
    }
    off = static_cast<size_t>(p - b.data());
    return v;
  }
  uint64_t Raw64() {
    if (off > b.size() || b.size() - off < 8) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (int s = 0; s < 64; s += 8) {
      v |= static_cast<uint64_t>(b[off++]) << s;
    }
    return v;
  }
  uint8_t U8() {
    if (off >= b.size()) {
      ok = false;
      return 0;
    }
    return b[off++];
  }
  std::string_view Str() {
    const uint64_t n = Var();
    if (!ok || n > b.size() - off) {
      ok = false;
      return {};
    }
    std::string_view s(reinterpret_cast<const char *>(b.data() + off), n);
    off += n;
    return s;
  }
};

constexpr uint64_t kMagic = 0x33533251; // "Q2S3"

// The order-preserving key is an involution of the IEEE bits.  Doubles that
// are whole numbers (most monetary values) have long runs of zero low bytes;
// those are not stored (their count `tz` goes into the group's flag byte).
inline uint64_t KeyBits(int64_t key) {
  return static_cast<uint64_t>(key ^ ((key >> 63) & std::numeric_limits<int64_t>::max()));
}
inline unsigned KeyZeroBytes(uint64_t bits) { return bits == 0 ? 8u : unsigned(__builtin_ctzll(bits)) / 8u; }
void PutKeyTail(std::vector<uint8_t> &out, uint64_t bits, unsigned tz) {
  for (unsigned s = tz * 8; s < 64; s += 8) {
    out.push_back(static_cast<uint8_t>(bits >> s));
  }
}

// ------------------------------------------------------------ misc helpers

core::InvalidationPlan AllPartitions() {
  return core::AllPartitionsPlan();
}

core::InvalidationPlan StatePartition() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(static_cast<::accelerator_codegen::aux_state::PartitionKey>(
      SecEdgarQ2Module::kStatePartition));
  return plan;
}

std::shared_ptr<const SecEdgarQ2StateImage>
MakeImage(std::shared_ptr<const SecEdgarQ2State> state) {
  auto image = std::make_shared<SecEdgarQ2StateImage>();
  image->state = std::move(state);
  return image;
}

const SecEdgarQ2StateImage *AsImage(const core::StateImage &image) {
  const auto *typed = image.As<SecEdgarQ2StateImage>();
  return typed && typed->state ? typed : nullptr;
}

struct QueryParams {
  bool is_null = false;
  int64_t fy = kSecEdgarQ2DefaultFy;
  std::string uom = kSecEdgarQ2DefaultUom;
};

QueryParams ParseArguments(const core::QueryArguments &args) {
  QueryParams p;
  if (args.values.empty()) {
    return p;
  }
  if (args.values.size() != 2) {
    throw std::invalid_argument(
        "sec_edgar q2 aux expects () or (fy BIGINT, uom VARCHAR)");
  }
  const auto &fy = args.values[0];
  const auto &uom = args.values[1];
  if (fy.is_null || uom.is_null) {
    p.is_null = true;
    return p;
  }
  if (std::holds_alternative<int64_t>(fy.payload)) {
    p.fy = std::get<int64_t>(fy.payload);
  } else if (std::holds_alternative<uint64_t>(fy.payload)) {
    const uint64_t v = std::get<uint64_t>(fy.payload);
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      p.is_null = true; // no fiscal year (INTEGER) can equal it: empty result
      return p;
    }
    p.fy = static_cast<int64_t>(v);
  } else if (std::holds_alternative<double>(fy.payload)) {
    const double v = std::get<double>(fy.payload);
    if (!(v == std::floor(v)) || !(std::fabs(v) < 9.0e18)) {
      p.is_null = true; // fractional / out of range: equals no integer fy
      return p;
    }
    p.fy = static_cast<int64_t>(v);
  } else if (std::holds_alternative<std::string>(fy.payload)) {
    const std::string &text = std::get<std::string>(fy.payload);
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
      p.is_null = true; // not an int64: equals no fiscal year
      return p;
    }
    p.fy = v;
  } else {
    throw std::invalid_argument("sec_edgar q2 aux: unsupported fy argument");
  }
  if (!std::holds_alternative<std::string>(uom.payload)) {
    throw std::invalid_argument("sec_edgar q2 aux: uom must be a string");
  }
  p.uom = std::get<std::string>(uom.payload);
  return p;
}

void EmitViews(core::AuxRowSink &sink,
               const std::vector<SecEdgarQ2OutputView> &rows) {
  sink.Begin({{{"name", core::AuxType::String, true},
               {"tag", core::AuxType::String, false},
               {"value", core::AuxType::Double, false}}});
  core::AuxRow out;
  out.values.resize(3);
  out.values[0] = core::AuxValue::Of(core::AuxType::String, std::string());
  out.values[1] = core::AuxValue::Of(core::AuxType::String, std::string());
  out.values[2] = core::AuxValue::Of(core::AuxType::Double, 0.0);
  for (const auto &row : rows) {
    auto &name = out.values[0];
    name.is_null = row.name_is_null;
    std::get<std::string>(name.payload).assign(row.name);
    std::get<std::string>(out.values[1].payload).assign(row.tag);
    out.values[2].payload = row.value;
    sink.WriteRow(out);
  }
  sink.End();
}

void EmitRows(core::AuxRowSink &sink, const std::vector<SecEdgarQ2OutputRow> &rows) {
  sink.Begin({{{"name", core::AuxType::String, true},
               {"tag", core::AuxType::String, false},
               {"value", core::AuxType::Double, false}}});
  core::AuxRow out;
  out.values.resize(3);
  for (const auto &row : rows) {
    out.values[0] = row.name_is_null
                        ? core::AuxValue::Null(core::AuxType::String)
                        : core::AuxValue::Of(core::AuxType::String, row.name);
    out.values[1] = core::AuxValue::Of(core::AuxType::String, row.tag);
    out.values[2] = core::AuxValue::Of(core::AuxType::Double, row.value);
    sink.WriteRow(out);
  }
  sink.End();
}

[[noreturn]] void ThrowCountOverflow() {
  throw std::overflow_error(
      "sec_edgar q2: more than 2^32-1 num rows share one (adsh, tag, uom, value)");
}

} // namespace

SecEdgarQ2Stats &SecEdgarQ2GlobalStats() {
  static SecEdgarQ2Stats stats;
  return stats;
}

// ------------------------------------------- InitialScan default behaviours

void SecEdgarQ2InitialScan::ScanUomRowCounts(
    const std::function<void(const std::string &, uint64_t)> &consumer) {
  std::map<std::string, uint64_t> counts;
  ScanNum([&](const SecEdgarQ2NumInput &in) {
    if (!in.value_is_null) {
      ++counts[in.uom];
    }
  });
  for (const auto &[uom, rows] : counts) {
    consumer(uom, rows);
  }
}

void SecEdgarQ2InitialScan::ScanNumValueCounts(
    const std::vector<std::string> &uoms,
    const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) {
  const std::unordered_set<std::string> wanted(uoms.begin(), uoms.end());
  std::map<std::tuple<std::string, std::string, std::string, int64_t>, uint64_t>
      counts;
  ScanNum([&](const SecEdgarQ2NumInput &in) {
    if (in.value_is_null || wanted.count(in.uom) == 0) {
      return;
    }
    ++counts[{in.adsh, in.tag, in.uom, Q2ValueKey(in.value)}];
  });
  for (const auto &[key, rows] : counts) {
    consumer({std::get<0>(key), std::get<1>(key), std::get<2>(key),
              Q2KeyValue(std::get<3>(key)), rows});
  }
}

void SecEdgarQ2InitialScan::LookupNumValueCounts(
    const std::string &uom,
    const std::vector<std::pair<std::string, std::string>> &groups,
    const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) {
  std::set<std::pair<std::string, std::string>> wanted(groups.begin(),
                                                       groups.end());
  std::map<std::tuple<std::string, std::string, int64_t>, uint64_t> counts;
  ScanNum([&](const SecEdgarQ2NumInput &in) {
    if (in.value_is_null || in.uom != uom ||
        wanted.count({in.adsh, in.tag}) == 0) {
      return;
    }
    ++counts[{in.adsh, in.tag, Q2ValueKey(in.value)}];
  });
  for (const auto &[key, rows] : counts) {
    consumer({std::get<0>(key), std::get<1>(key), uom,
              Q2KeyValue(std::get<2>(key)), rows});
  }
}

void SecEdgarQ2InitialScan::LookupSub(
    const std::vector<std::string> &adshs,
    const std::function<void(const SecEdgarQ2SubInput &)> &consumer) {
  const std::unordered_set<std::string> wanted(adshs.begin(), adshs.end());
  ScanSub([&](const SecEdgarQ2SubInput &in) {
    if (wanted.count(in.adsh) != 0) {
      consumer(in);
    }
  });
}

bool SecEdgarQ2InitialScan::ScanNumKeys(
    const std::vector<std::string> &,
    const std::function<void(std::string_view)> &,
    const std::function<void(std::string_view)> &) {
  return false;
}

bool SecEdgarQ2InitialScan::ScanNumValueCountsParallel(
    const std::vector<std::string> &, unsigned,
    const std::function<void(unsigned, const SecEdgarQ2ValueCountRow *, size_t)> &) {
  return false;
}

void SecEdgarQ2InitialScan::ExecuteDirect(
    int64_t fy, const std::string &uom,
    const std::function<void(const SecEdgarQ2DirectRow &)> &consumer) {
  struct SubV {
    std::string name;
    bool name_is_null;
  };
  std::unordered_map<std::string, SubV> subs;
  ScanSub([&](const SecEdgarQ2SubInput &in) {
    if (!in.fy_is_null && in.fy == fy) {
      subs[in.adsh] = {in.name, in.name_is_null};
    }
  });
  struct Row {
    std::string adsh, tag;
    int64_t key;
  };
  std::vector<Row> rows;
  std::map<std::pair<std::string, std::string>, int64_t> max_key;
  ScanNum([&](const SecEdgarQ2NumInput &in) {
    if (in.value_is_null || in.uom != uom) {
      return;
    }
    const int64_t k = Q2ValueKey(in.value);
    auto it = max_key.find({in.adsh, in.tag});
    if (it == max_key.end() || k > it->second) {
      max_key[{in.adsh, in.tag}] = k;
    }
    rows.push_back({in.adsh, in.tag, k});
  });
  std::vector<Row> hits;
  for (auto &r : rows) {
    if (subs.count(r.adsh) != 0 && max_key[{r.adsh, r.tag}] == r.key) {
      hits.push_back(std::move(r));
    }
  }
  std::sort(hits.begin(), hits.end(), [&](const Row &a, const Row &b) {
    if (a.key != b.key) {
      return a.key > b.key;
    }
    const auto &sa = subs[a.adsh];
    const auto &sb = subs[b.adsh];
    if (sa.name_is_null != sb.name_is_null) {
      return sb.name_is_null;
    }
    const int c = sa.name.compare(sb.name);
    if (c != 0) {
      return c < 0;
    }
    return a.tag < b.tag;
  });
  if (hits.size() > kSecEdgarQ2ResultLimit) {
    hits.resize(kSecEdgarQ2ResultLimit);
  }
  for (const auto &h : hits) {
    const auto &s = subs[h.adsh];
    consumer({s.name, s.name_is_null, h.tag, Q2KeyValue(h.key)});
  }
}

// ------------------------------------------------------------ state codec

std::vector<uint8_t> EncodeSecEdgarQ2State(const SecEdgarQ2State &state) {
  std::vector<uint8_t> out;
  core::PutVarint(out, kMagic);
  const auto put_dict = [&](const Q2Dict &d) {
    core::PutVarint(out, d.Size());
    for (uint32_t i = 0; i < d.Size(); ++i) {
      PutStr(out, d.Get(i));
    }
  };
  put_dict(state.adsh);
  put_dict(state.tag);
  for (uint32_t i = 0; i < state.adsh.Size(); ++i) {
    const bool has_fy = state.FyOf(i) != kQ2NoFy;
    const bool has_name = state.HasName(i);
    out.push_back(static_cast<uint8_t>((has_fy ? 1 : 0) | (has_name ? 2 : 0)));
    if (has_fy) {
      core::PutVarint(out, core::ZigZagEncode(state.FyOf(i)));
    }
    if (has_name) {
      PutStr(out, state.Name(i));
    }
  }
  const Q2UomDir &dir = *state.uoms;
  core::PutVarint(out, dir.names.size());
  for (uint32_t u = 0; u < dir.names.size(); ++u) {
    PutStr(out, dir.names.Get(u));
    out.push_back(dir.excluded[u]);
    if (dir.excluded[u]) {
      continue;
    }
    auto groups = state.LiveGroups(u);
    std::sort(groups.begin(), groups.end(), [](const Q2Group &a, const Q2Group &b) {
      return a.adsh != b.adsh ? a.adsh < b.adsh : a.tag < b.tag;
    });
    core::PutVarint(out, groups.size());
    uint32_t prev_adsh = 0;
    for (const auto &g : groups) {
      core::PutVarint(out, g.adsh - prev_adsh);
      prev_adsh = g.adsh;
      core::PutVarint(out, g.tag);
      const uint64_t bits0 = KeyBits(g.k0);
      const unsigned tz = KeyZeroBytes(bits0);
      out.push_back(static_cast<uint8_t>((g.c1 > 0 ? 1 : 0) | (g.trunc ? 2 : 0) | (tz << 2)));
      PutKeyTail(out, bits0, tz);
      core::PutVarint(out, g.c0);
      if (g.c1 > 0) {
        PutRaw64(out, static_cast<uint64_t>(g.k1));
        core::PutVarint(out, g.c1);
      }
    }
  }
  return out;
}

bool DecodeSecEdgarQ2State(const std::vector<uint8_t> &bytes,
                           SecEdgarQ2State &state) {
  Reader r{bytes};
  if (r.Var() != kMagic || !r.ok) {
    return false;
  }
  SecEdgarQ2State next;
  const auto get_dict = [&](Q2Dict &d) {
    const uint64_t n = r.Var();
    for (uint64_t i = 0; r.ok && i < n; ++i) {
      d.GetOrAdd(r.Str());
    }
    d.Compact();
  };
  get_dict(next.adsh);
  get_dict(next.tag);
  if (!r.ok) {
    return false;
  }
  for (uint32_t i = 0; r.ok && i < next.adsh.Size(); ++i) {
    const uint8_t flags = r.U8();
    int64_t fy = 0;
    if (flags & 1) {
      fy = core::ZigZagDecode(r.Var());
    }
    std::string_view name;
    if (flags & 2) {
      name = r.Str();
    }
    if (r.ok) {
      next.SetSub(i, !(flags & 1), fy, !(flags & 2), name);
    }
  }
  auto dir = std::make_shared<Q2UomDir>();
  const uint64_t nuom = r.Var();
  std::vector<Q2Group> groups;
  for (uint64_t u = 0; r.ok && u < nuom; ++u) {
    dir->names.Intern(r.Str());
    const uint8_t excluded = r.U8();
    dir->excluded.push_back(excluded);
    if (excluded) {
      continue;
    }
    const uint64_t ng = r.Var();
    uint32_t prev_adsh = 0;
    for (uint64_t i = 0; r.ok && i < ng; ++i) {
      Q2Group g;
      g.uom = static_cast<uint32_t>(u);
      prev_adsh += static_cast<uint32_t>(r.Var());
      g.adsh = prev_adsh;
      g.tag = static_cast<uint32_t>(r.Var());
      const uint8_t flags = r.U8();
      const unsigned tz = (flags >> 2) & 15u;
      if (tz > 8) {
        return false;
      }
      uint64_t bits0 = 0;
      for (unsigned sh = tz * 8; sh < 64; sh += 8) {
        bits0 |= static_cast<uint64_t>(r.U8()) << sh;
      }
      g.k0 = static_cast<int64_t>(KeyBits(static_cast<int64_t>(bits0)));
      g.c0 = static_cast<uint32_t>(r.Var());
      g.trunc = (flags & 2) != 0;
      if (flags & 1) {
        g.k1 = static_cast<int64_t>(r.Raw64());
        g.c1 = static_cast<uint32_t>(r.Var());
      }
      if (g.adsh >= next.adsh.Size() || g.tag >= next.tag.Size()) {
        return false;
      }
      groups.push_back(g);
    }
  }
  if (!r.ok || r.off != bytes.size()) {
    return false;
  }
  next.base = Q2Base::Build(std::move(groups), static_cast<uint32_t>(dir->names.size()));
  next.uoms = std::move(dir);
  state = std::move(next);
  return true;
}

// ------------------------------------------------------------ txn buffer

void SecEdgarQ2TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  batch_counter = 0;
  pool.clear();
  nums.clear();
  subs.clear();
}

void SecEdgarQ2TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  pool.clear();
  nums.clear();
  subs.clear();
}

void SecEdgarQ2TxnBuffer::AddNum(std::string_view adsh, std::string_view tag,
                                 std::string_view uom, int64_t key, int8_t sign) {
  if (requires_rebuild) {
    return;
  }
  pending = true;
  NumDelta d;
  d.adsh_off = pool.size();
  d.adsh_len = static_cast<uint32_t>(adsh.size());
  pool.append(adsh);
  d.tag_off = pool.size();
  d.tag_len = static_cast<uint32_t>(tag.size());
  pool.append(tag);
  d.uom_off = pool.size();
  d.uom_len = static_cast<uint32_t>(uom.size());
  pool.append(uom);
  d.key = key;
  d.sign = sign;
  nums.push_back(d);
}

void SecEdgarQ2TxnBuffer::AddSub(SubDelta delta) {
  if (requires_rebuild) {
    return;
  }
  pending = true;
  subs.push_back(std::move(delta));
}

// ----------------------------------------------------------------- module

core::AcceleratorId SecEdgarQ2Module::Id() const { return AcceleratorName(); }

std::vector<core::TableDependency> SecEdgarQ2Module::Dependencies() const {
  return {{config.num_table,
           {kQ2NumAdsh, kQ2NumTag, kQ2NumUom, kQ2NumValue}},
          {config.sub_table, {kQ2SubAdsh, kQ2SubName, kQ2SubFy}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ2Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ2TxnBuffer>();
}

core::InvalidationPlan
SecEdgarQ2Module::InterpretMutation(const core::MutationBatch &mutation) const {
  const auto touches = [&](std::initializer_list<core::ColumnId> columns) {
    if (mutation.kind != core::MutationKind::Update ||
        mutation.updated_columns.empty()) {
      return true;
    }
    for (const auto c : mutation.updated_columns) {
      for (const auto w : columns) {
        if (c == w) {
          return true;
        }
      }
    }
    return false;
  };
  if (mutation.table == config.num_table &&
      touches({kQ2NumAdsh, kQ2NumTag, kQ2NumUom, kQ2NumValue})) {
    return StatePartition();
  }
  if (mutation.table == config.sub_table &&
      touches({kQ2SubAdsh, kQ2SubName, kQ2SubFy})) {
    return StatePartition();
  }
  return {};
}

void SecEdgarQ2Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = static_cast<SecEdgarQ2TxnBuffer &>(base_buffer);
  if (InterpretMutation(mutation).IsEmpty()) {
    return;
  }
  const auto row_count = [&](const std::vector<core::AuxRow> &rows) {
    return rows.size();
  };
  if (mutation.table == config.num_table) {
    const auto add = [&](const core::AuxRow &row, int8_t sign) {
      NumRowView v;
      if (!ParseNumRow(row, v)) {
        buffer.MarkRebuild();
        return;
      }
      if (v.relevant) {
        buffer.AddNum(*v.adsh, *v.tag, *v.uom, v.key, sign);
      }
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        add(row, 1);
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        add(row, -1);
      }
      break;
    case core::MutationKind::Update:
      if (row_count(mutation.old_rows) != row_count(mutation.new_rows)) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        NumRowView a, b;
        if (!ParseNumRow(mutation.old_rows[i], a) ||
            !ParseNumRow(mutation.new_rows[i], b)) {
          buffer.MarkRebuild();
          return;
        }
        if (a.relevant && b.relevant && a.key == b.key && *a.adsh == *b.adsh &&
            *a.tag == *b.tag && *a.uom == *b.uom) {
          continue; // key columns unchanged
        }
        if (a.relevant) {
          buffer.AddNum(*a.adsh, *a.tag, *a.uom, a.key, -1);
        }
        if (b.relevant) {
          buffer.AddNum(*b.adsh, *b.tag, *b.uom, b.key, 1);
        }
      }
      break;
    }
    return;
  }
  if (mutation.table == config.sub_table) {
    const uint64_t batch = buffer.NextBatch();
    const auto add = [&](const core::AuxRow &row, int8_t sign) {
      SubRowView v;
      if (!ParseSubRow(row, v)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddSub({*v.adsh, v.name, v.fy, batch, v.fy_is_null, v.name_is_null, sign});
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        add(row, 1);
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        add(row, -1);
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        add(mutation.old_rows[i], -1);
        add(mutation.new_rows[i], 1);
      }
      break;
    }
  }
}

// ----------------------------------------------------------------- build

namespace {

// Parallel aggregation of the initial scan.  Phase 1 builds read-only
// dictionaries from the distinct keys; phase 2 converts batches (on `threads`
// workers) into compact records bucketed by group hash; phase 3 merges each
// bucket into its own group table, one thread per bucket.  Returns false when
// the scan source does not support it or a row refers to a key the first phase
// did not see (a concurrent write): the caller then runs the serial path.
struct BuildRec {
  int64_t key;
  uint32_t uom, adsh, tag, cnt;
};

bool BuildGroupsParallel(SecEdgarQ2InitialScan &scan, const std::vector<std::string> &covered,
                         const Q2UomDir &dir, SecEdgarQ2State &state, unsigned threads,
                         Q2GroupTable &out) {
  if (!scan.ScanNumKeys(
          covered, [&](std::string_view a) { state.adsh.GetOrAdd(a); },
          [&](std::string_view t) { state.tag.GetOrAdd(t); })) {
    return false;
  }
  state.adsh.Compact();
  state.tag.Compact();
  BuildTrace trace;
  const unsigned parts = threads;
  std::vector<std::vector<std::vector<BuildRec>>> buckets(
      threads, std::vector<std::vector<BuildRec>>(parts));
  std::atomic<bool> bad{false};
  if (!scan.ScanNumValueCountsParallel(
          covered, threads,
          [&](unsigned worker, const SecEdgarQ2ValueCountRow *rows, size_t n) {
            auto &mine = buckets[worker];
            for (size_t i = 0; i < n; ++i) {
              const auto &row = rows[i];
              const uint32_t ui = dir.names.Find(row.uom);
              if (ui == core::StringDict::kNone || dir.excluded[ui]) {
                bad.store(true, std::memory_order_relaxed);
                continue;
              }
              const uint32_t a = state.adsh.Find(row.adsh);
              const uint32_t t = state.tag.Find(row.tag);
              if (a == kQ2None || t == kQ2None) {
                bad.store(true, std::memory_order_relaxed);
                continue;
              }
              const size_t p = (Q2GroupHash(ui, a, t) >> 40) % parts;
              mine[p].push_back({Q2ValueKey(row.value), ui, a, t,
                                 static_cast<uint32_t>(std::min<uint64_t>(row.rows, 0xFFFFFFFFu))});
              if (row.rows > 0xFFFFFFFFu) {
                bad.store(true, std::memory_order_relaxed);
              }
            }
          })) {
    return false;
  }
  trace.Mark(" par: scan+convert");
  if (bad.load()) {
    return false;
  }
  std::vector<Q2GroupTable> tables(parts);
  std::atomic<bool> overflow{false};
  std::vector<std::thread> pool;
  for (unsigned p = 0; p < parts; ++p) {
    pool.emplace_back([&, p] {
      Q2GroupTable &acc = tables[p];
      for (unsigned w = 0; w < threads; ++w) {
        for (const BuildRec &r : buckets[w][p]) {
          const int32_t e = acc.Find(r.uom, r.adsh, r.tag);
          if (e >= 0) {
            if (!Q2GroupInsert(acc.ents[e], r.key, r.cnt)) {
              overflow.store(true);
            }
            continue;
          }
          Q2Group g;
          g.uom = r.uom;
          g.adsh = r.adsh;
          g.tag = r.tag;
          if (!Q2GroupInsert(g, r.key, r.cnt)) {
            overflow.store(true);
          }
          acc.Append(g);
        }
        std::vector<BuildRec>().swap(buckets[w][p]);
      }
      std::vector<uint32_t>().swap(acc.slots);
    });
  }
  for (auto &t : pool) {
    t.join();
  }
  if (overflow.load()) {
    ThrowCountOverflow();
  }
  size_t total = 0;
  for (const auto &t : tables) {
    total += t.ents.size();
  }
  out.ents.reserve(total);
  for (auto &t : tables) {
    out.ents.insert(out.ents.end(), t.ents.begin(), t.ents.end());
    std::vector<Q2Group>().swap(t.ents);
  }
  return true;
}

} // namespace

std::shared_ptr<const SecEdgarQ2State> SecEdgarQ2Module::BuildState() const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q2 requires a typed initial scan source");
  }
  BuildTrace trace;
  // 1. Decide which units of measure are materialised.
  std::vector<std::pair<std::string, uint64_t>> counts;
  initial_scan->ScanUomRowCounts([&](const std::string &uom, uint64_t rows) {
    counts.emplace_back(uom, rows);
  });
  std::sort(counts.begin(), counts.end());
  auto dir = std::make_shared<Q2UomDir>();
  std::vector<std::string> covered;
  for (const auto &[uom, rows] : counts) {
    dir->names.Intern(uom);
    const bool cover = uom == kSecEdgarQ2DefaultUom || rows <= config.max_uom_rows;
    dir->excluded.push_back(cover ? 0 : 1);
    if (cover) {
      covered.push_back(uom);
    }
  }

  trace.Mark("uom counts");
  // 2. Aggregate (group -> top-2 values) for the covered units.
  auto state = std::make_shared<SecEdgarQ2State>();
  Q2GroupTable acc;
  unsigned threads = config.build_threads;
  if (threads == 0) {
    threads = std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
  }
  bool parallel_done = false;
  if (!covered.empty() && threads > 1) {
    parallel_done = BuildGroupsParallel(*initial_scan, covered, *dir, *state, threads, acc);
    if (!parallel_done) {
      state = std::make_shared<SecEdgarQ2State>(); // unsupported or inconsistent: serial
      acc = Q2GroupTable();
    }
  }
  if (!covered.empty() && !parallel_done) {
    initial_scan->ScanNumValueCounts(
        covered, [&](const SecEdgarQ2ValueCountRow &row) {
          const uint32_t ui = dir->names.Find(row.uom);
          if (ui == core::StringDict::kNone || dir->excluded[ui]) {
            return;
          }
          const uint32_t a = state->adsh.GetOrAdd(row.adsh);
          const uint32_t t = state->tag.GetOrAdd(row.tag);
          const int32_t e = acc.Find(ui, a, t);
          if (e >= 0) {
            if (!Q2GroupInsert(acc.ents[e], Q2ValueKey(row.value), row.rows)) {
              ThrowCountOverflow();
            }
            return;
          }
          Q2Group g;
          g.uom = ui;
          g.adsh = a;
          g.tag = t;
          if (!Q2GroupInsert(g, Q2ValueKey(row.value), row.rows)) {
            ThrowCountOverflow();
          }
          acc.Append(g);
        });
  }
  trace.Mark("aggregate");
  state->adsh.Compact();
  state->tag.Compact();

  // 3. Subscription info of the adsh values that occur in the groups.
  initial_scan->ScanSub([&](const SecEdgarQ2SubInput &in) {
    const uint32_t id = state->adsh.Find(in.adsh);
    if (id != kQ2None) {
      state->SetSub(id, in.fy_is_null, in.fy, in.name_is_null, in.name);
    }
  });
  state->names.Shrink();
  trace.Mark("sub scan");

  // 4. Sorted base.
  acc.slots = std::vector<uint32_t>();
  const uint32_t uom_count = static_cast<uint32_t>(dir->names.size());
  state->base = Q2Base::Build(std::move(acc.ents), uom_count);
  state->uoms = std::move(dir);
  trace.Mark("base build");
  return state;
}

void SecEdgarQ2Module::Build(core::AuxEngineServices &,
                             core::StateBuilder &builder) {
  builder.Write(core::StateImage::Make(Id(), kStatePartition, 0,
                                       MakeImage(BuildState())));
  builder.Finish();
}

// ----------------------------------------------------------- maintenance

namespace {

// Applies the buffered deltas to a copy of `old` (every component is shared
// and copied on write).  Returns nullptr when the delta cannot be applied
// incrementally (caller rebuilds).
std::shared_ptr<const SecEdgarQ2State>
ApplyDeltas(const SecEdgarQ2State &old, const SecEdgarQ2TxnBuffer &buf,
            SecEdgarQ2InitialScan *scan, uint64_t overlay_limit) {
  auto ns = std::make_shared<SecEdgarQ2State>(old);
  std::shared_ptr<Q2UomDir> dir_w; // copied on first new unit of measure
  const auto cur_dir = [&]() -> const Q2UomDir & {
    return dir_w ? *dir_w : *ns->uoms;
  };

  // --- sub table, per mutation batch (statement): the batch's removed rows
  // first, then its added rows (so a statement that permutes keys is exact);
  // batches in chronological order.  An adsh no group mentions is skipped: it
  // is read from the table when its first group appears.
  const auto &subs = buf.Subs();
  for (std::size_t i = 0; i < subs.size();) {
    std::size_t j = i;
    while (j < subs.size() && subs[j].batch == subs[i].batch) {
      ++j;
    }
    for (std::size_t k = i; k < j; ++k) {
      if (subs[k].sign < 0) {
        const uint32_t id = ns->adsh.Find(subs[k].adsh);
        if (id != kQ2None) {
          ns->ClearSub(id);
        }
      }
    }
    for (std::size_t k = i; k < j; ++k) {
      if (subs[k].sign > 0) {
        const uint32_t id = ns->adsh.Find(subs[k].adsh);
        if (id != kQ2None) {
          ns->SetSub(id, subs[k].fy_is_null, subs[k].fy, subs[k].name_is_null,
                     subs[k].name);
        }
      }
    }
    i = j;
  }

  // --- num table.
  std::vector<uint32_t> new_adsh;
  struct Lost {
    uint32_t uom, adsh, tag;
  };
  std::vector<Lost> lost;
  // Returns the uom id, kQ2None for "not materialised".  `create` adds units
  // of measure that were never seen (they have no rows yet).
  const auto uom_id = [&](std::string_view uom, bool create) -> uint32_t {
    uint32_t id = cur_dir().names.Find(uom);
    if (id == core::StringDict::kNone) {
      if (!create) {
        return kQ2None;
      }
      if (!dir_w) {
        dir_w = std::make_shared<Q2UomDir>(*ns->uoms);
      }
      id = dir_w->names.Intern(uom);
      dir_w->excluded.push_back(0);
    }
    return cur_dir().excluded[id] ? kQ2None : id;
  };

  for (const auto &d : buf.Nums()) {
    if (d.sign < 0) {
      continue;
    }
    const uint32_t uid = uom_id(buf.Str(d.uom_off, d.uom_len), true);
    if (uid == kQ2None) {
      continue; // uom not materialised
    }
    const std::string_view a = buf.Str(d.adsh_off, d.adsh_len);
    const std::string_view t = buf.Str(d.tag_off, d.tag_len);
    uint32_t aid = ns->adsh.Find(a);
    if (aid == kQ2None) {
      aid = ns->adsh.GetOrAdd(a);
      new_adsh.push_back(aid);
    }
    const uint32_t tid = ns->tag.GetOrAdd(t);
    Q2Group g;
    g.uom = uid;
    g.adsh = aid;
    g.tag = tid;
    ns->Get(uid, aid, tid, g);
    if (!Q2GroupInsert(g, d.key)) {
      return nullptr;
    }
    // The group may be new to the overlay's dir-sized base (uom created now):
    // base slices only cover the uoms present at the last compaction.
    ns->Put(g);
  }
  for (const auto &d : buf.Nums()) {
    if (d.sign > 0) {
      continue;
    }
    const std::string_view uom = buf.Str(d.uom_off, d.uom_len);
    if (cur_dir().names.Find(uom) == core::StringDict::kNone) {
      return nullptr; // delete from a uom we never saw: state is inconsistent
    }
    const uint32_t uid = uom_id(uom, false);
    if (uid == kQ2None) {
      continue; // uom not materialised
    }
    const uint32_t aid = ns->adsh.Find(buf.Str(d.adsh_off, d.adsh_len));
    const uint32_t tid = ns->tag.Find(buf.Str(d.tag_off, d.tag_len));
    if (aid == kQ2None || tid == kQ2None) {
      return nullptr;
    }
    Q2Group g;
    if (!ns->Get(uid, aid, tid, g)) {
      return nullptr;
    }
    switch (Q2GroupRemove(g, d.key)) {
    case Q2Removal::Ok:
      ns->Put(g);
      break;
    case Q2Removal::Lost:
      ns->Put(g);
      lost.push_back({uid, aid, tid});
      break;
    case Q2Removal::Inconsistent:
      SecEdgarQ2GlobalStats().inconsistent_deltas.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
  }
  if (dir_w) {
    ns->uoms = dir_w;
  }

  // --- adsh values seen for the first time need their sub row.
  if (!new_adsh.empty()) {
    if (!scan) {
      return nullptr;
    }
    SecEdgarQ2GlobalStats().new_adsh_lookups.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::string> names;
    names.reserve(new_adsh.size());
    for (const uint32_t id : new_adsh) {
      names.emplace_back(ns->adsh.Get(id));
    }
    scan->LookupSub(names, [&](const SecEdgarQ2SubInput &in) {
      const uint32_t id = ns->adsh.Find(in.adsh);
      if (id != kQ2None) {
        ns->SetSub(id, in.fy_is_null, in.fy, in.name_is_null, in.name);
      }
    });
  }

  // --- groups whose listed values were all deleted are re-read.
  if (!lost.empty()) {
    if (!scan) {
      return nullptr;
    }
    SecEdgarQ2GlobalStats().lost_group_lookups.fetch_add(1, std::memory_order_relaxed);
    std::map<uint32_t, std::vector<const Lost *>> by_uom;
    for (const auto &l : lost) {
      by_uom[l.uom].push_back(&l);
    }
    for (const auto &[uid, items] : by_uom) {
      std::vector<std::pair<std::string, std::string>> groups;
      groups.reserve(items.size());
      for (const Lost *l : items) {
        groups.emplace_back(std::string(ns->adsh.Get(l->adsh)),
                            std::string(ns->tag.Get(l->tag)));
      }
      Q2GroupTable fresh;
      bool overflow = false;
      scan->LookupNumValueCounts(
          std::string(ns->uoms->names.Get(uid)), groups,
          [&](const SecEdgarQ2ValueCountRow &row) {
            const uint32_t a = ns->adsh.Find(row.adsh);
            const uint32_t t = ns->tag.Find(row.tag);
            if (a == kQ2None || t == kQ2None) {
              return;
            }
            const int32_t e = fresh.Find(uid, a, t);
            Q2Group g;
            if (e >= 0) {
              g = fresh.ents[e];
            } else {
              g.uom = uid;
              g.adsh = a;
              g.tag = t;
            }
            if (!Q2GroupInsert(g, Q2ValueKey(row.value), row.rows)) {
              overflow = true;
            }
            fresh.Put(g);
          });
      if (overflow) {
        return nullptr;
      }
      for (const Lost *l : items) {
        Q2Group g;
        g.uom = uid;
        g.adsh = l->adsh;
        g.tag = l->tag;
        const int32_t e = fresh.Find(uid, l->adsh, l->tag);
        if (e >= 0) {
          g = fresh.ents[e];
        }
        ns->Put(g);
      }
    }
  }

  // --- keep the shared parts small.
  if (ns->adsh.NeedsCompaction()) {
    ns->adsh.Compact();
  }
  if (ns->tag.NeedsCompaction()) {
    ns->tag.Compact();
  }
  if (ns->NeedsCompaction(static_cast<size_t>(overlay_limit))) {
    SecEdgarQ2GlobalStats().compactions.fetch_add(1, std::memory_order_relaxed);
    ns->Compact();
  }
  return ns;
}

core::StagedMaintenanceOutcome RebuildOutcome(core::AuxEngineServices &services,
                                              bool whole_state) {
  core::StagedMaintenanceOutcome outcome;
  const auto decision = services.ChooseMaintenance(
      {0, 0, false, false, true, whole_state, false,
       core::MaintenanceAction::NoOp, services.ExpectedQueriesBeforeNextWrite()});
  outcome.action = decision.action == core::MaintenanceAction::ApplyDelta ||
                           decision.action == core::MaintenanceAction::NoOp
                       ? core::MaintenanceAction::RebuildAll
                       : decision.action;
  outcome.invalidations = AllPartitions();
  outcome.pending_rebuild = outcome.invalidations;
  SecEdgarQ2GlobalStats().rebuild.fetch_add(1, std::memory_order_relaxed);
  return outcome;
}

} // namespace

core::StagedMaintenanceOutcome
SecEdgarQ2Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = static_cast<SecEdgarQ2TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (!buffer.HasPendingChanges()) {
    return outcome;
  }
  if (buffer.RequiresRebuild()) {
    return RebuildOutcome(services, true);
  }
  const uint64_t rows = buffer.Nums().size() + buffer.Subs().size();
  if (rows == 0) {
    return outcome;
  }
  const auto decision = services.ChooseMaintenance(
      {rows, 1, true, false, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    return RebuildOutcome(services, false);
  }

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  const SecEdgarQ2StateImage *typed = nullptr;
  core::Version version = 0;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == kStatePartition) {
      typed = AsImage(image);
      version = image.version;
    }
  }
  if (!snapshot.available || !typed) {
    return RebuildOutcome(services, true);
  }
  std::shared_ptr<const SecEdgarQ2State> next;
  try {
    next = ApplyDeltas(*typed->state, buffer, initial_scan, config.overlay_limit);
  } catch (const std::exception &) {
    next = nullptr;
  }
  if (!next) {
    return RebuildOutcome(services, true);
  }
  SecEdgarQ2GlobalStats().apply_delta.fetch_add(1, std::memory_order_relaxed);
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      Id(), kStatePartition, version, MakeImage(std::move(next))));
  outcome.expected_version = 0;
  return outcome;
}

// ------------------------------------------------------------------ read

bool SecEdgarQ2Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ2Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const QueryParams params = ParseArguments(arguments);
  if (params.is_null) {
    EmitRows(sink, {});
    return;
  }
  // One observation: validity plus the partition's handle.
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = kStatePartition;
  const core::QueryKey hit_key{Id(), {}};
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), hit_key, wanted);
  const SecEdgarQ2StateImage *typed = nullptr;
  if (snapshot.available) {
    for (const auto &image : snapshot.partitions) {
      if (image.partition == kStatePartition) {
        typed = AsImage(image);
      }
    }
  }
  if (!typed) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  thread_local std::vector<SecEdgarQ2OutputView> rows;
  if (!FinalizeSecEdgarQ2Views(*typed->state, params.fy, params.uom, rows)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  SecEdgarQ2GlobalStats().state_reads.fetch_add(1, std::memory_order_relaxed);
  EmitViews(sink, rows);
}

void SecEdgarQ2Module::ExecuteFallback(core::AuxEngineServices &,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const QueryParams params = ParseArguments(arguments);
  if (params.is_null) {
    EmitRows(sink, {});
    return;
  }
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q2 requires a typed initial scan source");
  }
  SecEdgarQ2GlobalStats().direct_reads.fetch_add(1, std::memory_order_relaxed);
  std::vector<SecEdgarQ2OutputRow> rows;
  initial_scan->ExecuteDirect(params.fy, params.uom,
                              [&](const SecEdgarQ2DirectRow &r) {
                                rows.push_back({std::string(r.name),
                                                r.name_is_null,
                                                std::string(r.tag), r.value});
                              });
  EmitRows(sink, rows);
}

} // namespace accelerator_codegen::aux_state::query
