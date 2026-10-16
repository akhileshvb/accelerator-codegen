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
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "substrate/aux/core/parallel_slices.hpp"

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <sys/mman.h>

namespace accelerator_codegen::aux_state::query {
namespace {

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

bool SecEdgarQ2InitialScan::ScanNumIds(
    SecEdgarQ2KeyMap &, const std::function<void(const SecEdgarQ2IdBatch &)> &) {
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
  for (uint64_t u = 0; r.ok && u < nuom; ++u) {
    dir->names.Intern(r.Str());
    const uint8_t excluded = r.U8();
    dir->excluded.push_back(excluded);
    next.slices.emplace_back();
    if (excluded) {
      continue;
    }
    std::vector<Q2Group> groups;
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
    if (!r.ok) {
      return false;
    }
    next.slices.back() = Q2Slice::Make(static_cast<uint32_t>(u), Q2Base::Build(std::move(groups)));
  }
  if (!r.ok || r.off != bytes.size() || dir->names.size() != nuom) {
    return false;
  }
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

void SecEdgarQ2TxnBuffer::AddNumMove(std::string_view adsh, std::string_view tag,
                                     std::string_view uom, int64_t old_key,
                                     int64_t new_key) {
  if (requires_rebuild) {
    return;
  }
  AddNum(adsh, tag, uom, old_key, -1);
  NumDelta d = nums.back();
  d.key = new_key;
  d.sign = 1;
  nums.push_back(d);
}

void SecEdgarQ2TxnBuffer::Absorb(SecEdgarQ2TxnBuffer &&from) {
  if (requires_rebuild) {
    return;
  }
  if (from.requires_rebuild) {
    MarkRebuild();
    return;
  }
  pending = pending || from.pending;
  const uint64_t shift = pool.size();
  pool.append(from.pool);
  for (auto d : from.nums) {
    d.adsh_off += shift;
    d.tag_off += shift;
    d.uom_off += shift;
    nums.push_back(d);
  }
  subs.insert(subs.end(), std::make_move_iterator(from.subs.begin()),
              std::make_move_iterator(from.subs.end()));
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

namespace {

// Rows per slice of a mutation's row loop (parsing and key extraction).
constexpr std::size_t kAccumulateGrain = 4096;

// Buffers the deltas of rows [begin, end) of a num mutation into `buffer`.
void AccumulateNumRows(const core::MutationBatch &mutation, std::size_t begin,
                       std::size_t end, SecEdgarQ2TxnBuffer &buffer) {
  const auto image_rows = [&](const std::vector<core::AuxRow> &rows, int8_t sign) {
    for (std::size_t i = begin; i < end; ++i) {
      NumRowView v;
      if (!ParseNumRow(rows[i], v)) {
        buffer.MarkRebuild();
        return;
      }
      if (v.relevant) {
        buffer.AddNum(*v.adsh, *v.tag, *v.uom, v.key, sign);
      }
    }
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    image_rows(mutation.new_rows, 1);
    return;
  case core::MutationKind::Delete:
    image_rows(mutation.old_rows, -1);
    return;
  case core::MutationKind::Update:
    break;
  }
  if (!mutation.updated_columns.empty() &&
      std::none_of(mutation.updated_columns.begin(), mutation.updated_columns.end(),
                   [](core::ColumnId c) {
                     return c == kQ2NumAdsh || c == kQ2NumTag || c == kQ2NumUom;
                   })) {
    // Only `value` (or irrelevant columns) changed: the key strings of the
    // new row are those of the old row, read them once.
    for (std::size_t i = begin; i < end; ++i) {
      NumRowView a;
      double value = 0.0;
      bool value_null = false;
      if (!ParseNumRow(mutation.old_rows[i], a) ||
          !ReadDouble(mutation.new_rows[i], kQ2NumValue, value, value_null)) {
        buffer.MarkRebuild();
        return;
      }
      const bool keyed = a.adsh && a.tag && a.uom;
      const bool b_relevant = keyed && !value_null;
      const int64_t b_key = b_relevant ? Q2ValueKey(value) : 0;
      if (a.relevant && b_relevant) {
        if (a.key != b_key) {
          buffer.AddNumMove(*a.adsh, *a.tag, *a.uom, a.key, b_key);
        }
      } else if (a.relevant) {
        buffer.AddNum(*a.adsh, *a.tag, *a.uom, a.key, -1);
      } else if (b_relevant) {
        buffer.AddNum(*a.adsh, *a.tag, *a.uom, b_key, 1);
      }
    }
    return;
  }
  for (std::size_t i = begin; i < end; ++i) {
    NumRowView a, b;
    if (!ParseNumRow(mutation.old_rows[i], a) || !ParseNumRow(mutation.new_rows[i], b)) {
      buffer.MarkRebuild();
      return;
    }
    if (a.relevant && b.relevant && a.key == b.key && *a.adsh == *b.adsh &&
        *a.tag == *b.tag && *a.uom == *b.uom) {
      continue; // key columns unchanged
    }
    if (a.relevant && b.relevant && *a.adsh == *b.adsh && *a.tag == *b.tag &&
        *a.uom == *b.uom) {
      buffer.AddNumMove(*a.adsh, *a.tag, *a.uom, a.key, b.key);
      continue;
    }
    if (a.relevant) {
      buffer.AddNum(*a.adsh, *a.tag, *a.uom, a.key, -1);
    }
    if (b.relevant) {
      buffer.AddNum(*b.adsh, *b.tag, *b.uom, b.key, 1);
    }
  }
}

// Buffers rows [begin, end) of a sub mutation into `buffer` as batch `batch`.
void AccumulateSubRows(const core::MutationBatch &mutation, std::size_t begin,
                       std::size_t end, uint64_t batch, SecEdgarQ2TxnBuffer &buffer) {
  const auto add = [&](const core::AuxRow &row, int8_t sign) {
    SubRowView v;
    if (!ParseSubRow(row, v)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddSub({*v.adsh, v.name, v.fy, batch, v.fy_is_null, v.name_is_null, sign});
  };
  for (std::size_t i = begin; i < end; ++i) {
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      add(mutation.new_rows[i], 1);
      break;
    case core::MutationKind::Delete:
      add(mutation.old_rows[i], -1);
      break;
    case core::MutationKind::Update:
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
      break;
    }
  }
}

} // namespace

void SecEdgarQ2Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &services,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = static_cast<SecEdgarQ2TxnBuffer &>(base_buffer);
  if (InterpretMutation(mutation).IsEmpty()) {
    return;
  }
  if (mutation.kind == core::MutationKind::Update &&
      mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }
  const bool is_num = mutation.table == config.num_table;
  if (!is_num && mutation.table != config.sub_table) {
    return;
  }
  const uint64_t batch = is_num ? 0 : buffer.NextBatch();
  const auto &rows =
      mutation.kind == core::MutationKind::Insert ? mutation.new_rows : mutation.old_rows;
  core::ParallelReduce(
      services, rows.size(), kAccumulateGrain, buffer,
      [&](SecEdgarQ2TxnBuffer &out, std::size_t begin, std::size_t end) {
        is_num ? AccumulateNumRows(mutation, begin, end, out)
               : AccumulateSubRows(mutation, begin, end, batch, out);
      },
      [](SecEdgarQ2TxnBuffer &into, SecEdgarQ2TxnBuffer &&from) {
        into.Absorb(std::move(from));
      });
}

// ----------------------------------------------------------------- build

namespace {

// Aggregation of the initial scan, cache-conscious:
//   1. every row becomes a 24-byte record appended to one of 1024 partitions
//      chosen by the high bits of Q2PairHash(adsh, tag) (sequential writes);
//   2. each partition is aggregated on its own into a small hash table keyed
//      by (uom, adsh, tag) that stays in the L2 cache, and its groups are
//      appended to their unit of measure's group list;
//   3. per unit of measure the groups are radix-sorted by value (desc) into
//      the Q2Base arrays and the (adsh, tag) index is built.
// No step touches a large table at random, which is what made a one-pass
// hash aggregation over ~8.6M groups memory-latency bound.
struct BuildEnt {
  int64_t k0;
  int64_t k1;
  uint32_t adsh;
  uint32_t tag;
  uint32_t c0; // 0 = empty slot
  uint32_t c1; // bit 31 = trunc
};
static_assert(sizeof(BuildEnt) == 32, "BuildEnt is half a cache line");

constexpr uint32_t kBuildTrunc = 0x80000000u;

inline Q2Group FromBuild(const BuildEnt &e) {
  Q2Group g;
  g.k0 = e.k0;
  g.k1 = e.k1;
  g.adsh = e.adsh;
  g.tag = e.tag;
  g.c0 = e.c0;
  g.c1 = e.c1 & ~kBuildTrunc;
  g.trunc = (e.c1 & kBuildTrunc) != 0;
  return g;
}

// Adds `n` rows of `key` to a non-empty group.
template <typename Ent> inline void BuildMerge(Ent &e, int64_t key, uint64_t n) {
  if (n == 1 && key == e.k0 && e.c0 != 0xFFFFFFFFu) {
    ++e.c0; // the common duplicate-top case
    return;
  }
  Q2Group g;
  g.k0 = e.k0;
  g.k1 = e.k1;
  g.c0 = e.c0;
  g.c1 = e.c1 & ~kBuildTrunc;
  g.trunc = (e.c1 & kBuildTrunc) != 0;
  if (!Q2GroupInsert(g, key, n) || g.c1 >= kBuildTrunc) {
    ThrowCountOverflow();
  }
  e.k0 = g.k0;
  e.k1 = g.k1;
  e.c0 = g.c0;
  e.c1 = g.c1 | (g.trunc ? kBuildTrunc : 0);
}

// Anonymous memory advised to use transparent huge pages: the build writes
// about a gigabyte of fresh records / groups, and 4 KB page faults would cost
// more than the writes themselves.  Zero-filled, populated lazily.
class HugeRegion {
public:
  HugeRegion() = default;
  explicit HugeRegion(size_t bytes) : bytes_(bytes) {
    if (bytes == 0) {
      return;
    }
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      throw std::bad_alloc();
    }
#ifdef MADV_HUGEPAGE
    if (bytes >= (size_t{2} << 20)) {
      madvise(p, bytes, MADV_HUGEPAGE);
    }
#endif
    data_ = static_cast<char *>(p);
  }
  HugeRegion(const HugeRegion &) = delete;
  HugeRegion &operator=(const HugeRegion &) = delete;
  HugeRegion(HugeRegion &&o) noexcept
      : data_(std::exchange(o.data_, nullptr)), bytes_(std::exchange(o.bytes_, 0)) {}
  HugeRegion &operator=(HugeRegion &&o) noexcept {
    if (this != &o) {
      Release();
      data_ = std::exchange(o.data_, nullptr);
      bytes_ = std::exchange(o.bytes_, 0);
    }
    return *this;
  }
  ~HugeRegion() { Release(); }
  char *data() const { return data_; }
  size_t bytes() const { return bytes_; }

private:
  void Release() {
    if (data_) {
      munmap(data_, bytes_);
      data_ = nullptr;
    }
  }
  char *data_ = nullptr;
  size_t bytes_ = 0;
};

// Append-only array with a fixed capacity in a HugeRegion (trivial T).
template <typename T> class HugeVec {
public:
  HugeVec() = default;
  explicit HugeVec(size_t capacity) : region_(capacity * sizeof(T)), cap_(capacity) {}
  void push_back(const T &v) {
    if (n_ == cap_) {
      Regrow(std::max<size_t>(16, cap_ * 2));
    }
    reinterpret_cast<T *>(region_.data())[n_++] = v;
  }
  size_t size() const { return n_; }
  const T *data() const { return reinterpret_cast<const T *>(region_.data()); }
  const T &operator[](size_t i) const { return data()[i]; }

private:
  void Regrow(size_t cap) {
    HugeRegion fresh(cap * sizeof(T));
    if (n_ != 0) {
      std::memcpy(fresh.data(), region_.data(), n_ * sizeof(T));
    }
    region_ = std::move(fresh);
    cap_ = cap;
  }
  HugeRegion region_;
  size_t cap_ = 0;
  size_t n_ = 0;
};

// A group key packed into 64 bits: uom (12 bits) | adsh (26) | tag (26).
// Rows whose ids do not fit (never on SEC data: ~200 units, ~90k filings,
// ~1M tags), and the pre-counted rows of the generic path, take the "wide"
// path instead.
constexpr unsigned kPackTagBits = 26;
constexpr unsigned kPackAdshBits = 26;
constexpr unsigned kPackUomBits = 12;

inline bool Packable(uint32_t u, uint32_t a, uint32_t t) {
  return u < (1u << kPackUomBits) && a < (1u << kPackAdshBits) && t < (1u << kPackTagBits);
}
inline uint64_t Pack(uint32_t u, uint32_t a, uint32_t t) {
  return (static_cast<uint64_t>(u) << (kPackAdshBits + kPackTagBits)) |
         (static_cast<uint64_t>(a) << kPackTagBits) | t;
}
inline uint64_t PackHash(uint64_t k) { // murmur3 finaliser: high and low bits both mixed
  k ^= k >> 33;
  k *= 0xFF51AFD7ED558CCDull;
  k ^= k >> 33;
  k *= 0xC4CEB9FE1A85EC53ull;
  return k ^ (k >> 33);
}

struct PackedRec {
  int64_t key;
  uint64_t gk;
};
static_assert(sizeof(PackedRec) == 16, "four records per cache line");

// Same layout as BuildEnt with (adsh, tag) replaced by the packed key.
struct PackedEnt {
  int64_t k0;
  int64_t k1;
  uint64_t gk;
  uint32_t c0; // 0 = empty slot
  uint32_t c1; // bit 31 = trunc
};
static_assert(sizeof(PackedEnt) == 32, "PackedEnt is half a cache line");

struct WideRec {
  int64_t key;
  uint32_t uom;
  uint32_t adsh;
  uint32_t tag;
  uint32_t cnt;
};

class BuildAggregator final : public SecEdgarQ2KeyMap {
public:
  static constexpr unsigned kPartBits = 12;
  static constexpr size_t kParts = size_t{1} << kPartBits;
  static constexpr size_t kBlock = 1024; // records per buffer block (16 KB)
  static constexpr size_t kLine = 4;     // records per write-combining line

  explicit BuildAggregator(SecEdgarQ2State &state)
      : state_(state), parts_(kParts), cur_(kParts, nullptr), end_(kParts, nullptr),
        wc_(new Line[kParts]), wcn_(kParts, 0) {}

  uint32_t Uom(std::string_view uom) override {
    const uint32_t id = dir_.names.Intern(uom);
    if (id >= uom_rows_.size()) {
      uom_rows_.resize(id + 1, 0);
      dir_.excluded.resize(id + 1, 0);
    }
    return id;
  }
  uint32_t Adsh(std::string_view adsh) override { return state_.adsh.GetOrAdd(adsh); }
  uint32_t Tag(std::string_view tag) override { return state_.tag.GetOrAdd(tag); }

  // One batch of single rows.
  void AddBatch(const SecEdgarQ2IdBatch &b) {
    for (size_t i = 0; i < b.n; ++i) {
      const uint32_t u = b.uom[i], a = b.adsh[i], t = b.tag[i];
      ++uom_rows_[u];
      if (Packable(u, a, t)) {
        PushPacked(b.key[i], Pack(u, a, t));
      } else {
        wide_.push_back({b.key[i], u, a, t, 1});
      }
    }
  }
  // The rows of the generic path are counted up front (NoteRows).
  void NoteRows(uint32_t u, uint64_t rows) { uom_rows_[u] += rows; }
  // One distinct (uom, adsh, tag, value) with its row count (generic path).
  void AddCounted(uint32_t u, uint32_t a, uint32_t t, int64_t key, uint64_t rows) {
    if (rows == 0 || rows > 0xFFFFFFFFull) {
      ThrowCountOverflow();
    }
    wide_.push_back({key, u, a, t, static_cast<uint32_t>(rows)});
  }

  // Applies the max_uom_rows opt-out (the default unit is always kept),
  // aggregates every partition and builds the sorted bases.
  void Finish(const core::AuxEngineServices &services, uint64_t max_uom_rows) {
    for (uint32_t u = 0; u < uom_rows_.size(); ++u) {
      dir_.excluded[u] =
          dir_.names.Get(u) != kSecEdgarQ2DefaultUom && uom_rows_[u] > max_uom_rows ? 1 : 0;
    }
    const size_t nuom = uom_rows_.size();
    // A unit has at most as many groups as rows: the capacity is only
    // reserved address space (pages are populated as groups are appended).
    Groups groups(nuom);
    for (size_t u = 0; u < nuom; ++u) {
      if (!dir_.excluded[u]) {
        groups[u] = HugeVec<BuildEnt>(std::max<uint64_t>(16, uom_rows_[u]));
      }
    }
    // Packed rows: partition by partition.
#if defined(__SSE2__)
    _mm_sfence(); // the streamed lines are visible to the loads below
#endif
    // Slices of partitions aggregate independently; their groups are appended
    // in slice order, so each unit's group order is the serial one.
    core::ParallelReduce(
        services, kParts, kPartGrain, groups,
        [&](Groups &out, size_t begin, size_t end) {
          out.resize(nuom);
          AggregatePartitions(begin, end, out);
        },
        [&](Groups &into, Groups &&from) {
          for (size_t u = 0; u < nuom; ++u) {
            for (size_t i = 0; i < from[u].size(); ++i) {
              into[u].push_back(from[u][i]);
            }
          }
        });
    parts_.clear();
    std::vector<HugeRegion>().swap(arena_);
    // Wide rows (generic path / ids beyond the packing): sort and fold.
    std::sort(wide_.begin(), wide_.end(), [](const WideRec &x, const WideRec &y) {
      return std::tie(x.uom, x.adsh, x.tag) < std::tie(y.uom, y.adsh, y.tag);
    });
    for (size_t i = 0; i < wide_.size();) {
      const WideRec &r = wide_[i];
      BuildEnt e{r.key, 0, r.adsh, r.tag, r.cnt, 0};
      size_t j = i + 1;
      for (; j < wide_.size() && wide_[j].uom == r.uom && wide_[j].adsh == r.adsh &&
             wide_[j].tag == r.tag;
           ++j) {
        BuildMerge(e, wide_[j].key, wide_[j].cnt);
      }
      if (!dir_.excluded[r.uom]) {
        groups[r.uom].push_back(e);
      }
      i = j;
    }
    std::vector<WideRec>().swap(wide_);
    state_.slices.assign(nuom, nullptr);
    services.ParallelFor(nuom, [&](size_t u) {
      if (!dir_.excluded[u]) {
        state_.slices[u] = Q2Slice::Make(static_cast<uint32_t>(u), Seal(groups[u]));
      }
      groups[u] = HugeVec<BuildEnt>();
    });
    state_.uoms = std::make_shared<const Q2UomDir>(std::move(dir_));
  }

private:
  using Groups = std::vector<HugeVec<BuildEnt>>;
  // Partitions per slice: each is 1/4096 of the packed rows, so a slice is
  // at least 1/64 of the table.
  static constexpr size_t kPartGrain = 64;

  struct Part {
    std::vector<PackedRec *> blocks;
  };
  struct alignas(64) Line {
    PackedRec r[kLine];
  };

  // Aggregates partitions [begin, end) and appends their groups to `groups`.
  void AggregatePartitions(size_t begin, size_t end, Groups &groups) const {
    PartTable table;
    for (size_t pi = begin; pi < end; ++pi) {
      table.Reset();
      const Part &part = parts_[pi];
      for (size_t b = 0; b < part.blocks.size(); ++b) {
        const size_t n = b + 1 == part.blocks.size()
                             ? static_cast<size_t>(cur_[pi] - part.blocks[b])
                             : kBlock;
        const PackedRec *recs = part.blocks[b];
        constexpr size_t kAhead = 8;
        for (size_t i = 0; i < n; ++i) {
          if (i + kAhead < n) {
            table.Prefetch(recs[i + kAhead].gk);
          }
          table.Add(recs[i], 1);
        }
      }
      for (unsigned i = 0; i < wcn_[pi]; ++i) {
        table.Add(wc_[pi].r[i], 1);
      }
      table.Emit(groups, dir_.excluded);
    }
  }

  // Rows are staged in a cache-line buffer per partition and written to the
  // partition's block a full line at a time with streaming stores, so the
  // 4096 output streams neither thrash the cache nor read before writing.
  void PushPacked(int64_t key, uint64_t gk) {
    const size_t p = PackHash(gk) >> (64 - kPartBits);
    Line &line = wc_[p];
    unsigned n = wcn_[p];
    line.r[n] = {key, gk};
    if (++n == kLine) {
      if (cur_[p] == end_[p]) {
        PackedRec *b = NewBlock();
        parts_[p].blocks.push_back(b);
        cur_[p] = b;
        end_[p] = b + kBlock;
      }
#if defined(__SSE2__)
      const __m128i *src = reinterpret_cast<const __m128i *>(line.r);
      __m128i *dst = reinterpret_cast<__m128i *>(cur_[p]);
      _mm_stream_si128(dst + 0, _mm_load_si128(src + 0));
      _mm_stream_si128(dst + 1, _mm_load_si128(src + 1));
      _mm_stream_si128(dst + 2, _mm_load_si128(src + 2));
      _mm_stream_si128(dst + 3, _mm_load_si128(src + 3));
#else
      std::memcpy(cur_[p], line.r, sizeof(line.r));
#endif
      cur_[p] += kLine;
      n = 0;
    }
    wcn_[p] = static_cast<uint8_t>(n);
  }

  // Record blocks are carved from 64 MB huge-page regions (64-byte aligned).
  PackedRec *NewBlock() {
    constexpr size_t kBlockBytes = kBlock * sizeof(PackedRec);
    constexpr size_t kRegionBytes = size_t{64} << 20;
    if (arena_.empty() || arena_used_ + kBlockBytes > arena_.back().bytes()) {
      arena_.emplace_back(kRegionBytes);
      arena_used_ = 0;
    }
    PackedRec *b = reinterpret_cast<PackedRec *>(arena_.back().data() + arena_used_);
    arena_used_ += kBlockBytes;
    return b;
  }

  // Hash table of one partition: packed key -> group, grows in place.
  class PartTable {
  public:
    void Reset() {
      if (ents_.size() != kInitial) {
        ents_.assign(kInitial, PackedEnt{0, 0, 0, 0, 0});
      } else if (size_ != 0) {
        std::fill(ents_.begin(), ents_.end(), PackedEnt{0, 0, 0, 0, 0});
      }
      size_ = 0;
    }
    void Prefetch(uint64_t gk) const {
      __builtin_prefetch(ents_.data() + (PackHash(gk) & (ents_.size() - 1)), 1, 3);
    }
    void Add(const PackedRec &r, uint32_t cnt) {
      if ((size_ + 1) * 10 > ents_.size() * 7) {
        Grow();
      }
      const size_t mask = ents_.size() - 1;
      size_t pos = PackHash(r.gk) & mask;
      while (true) {
        PackedEnt &e = ents_[pos];
        if (e.c0 == 0) {
          e = PackedEnt{r.key, 0, r.gk, cnt, 0};
          ++size_;
          return;
        }
        if (e.gk == r.gk) {
          BuildMerge(e, r.key, cnt);
          return;
        }
        pos = (pos + 1) & mask;
      }
    }
    void Emit(std::vector<HugeVec<BuildEnt>> &groups,
              const std::vector<uint8_t> &excluded) const {
      constexpr uint64_t kTagMask = (uint64_t{1} << kPackTagBits) - 1;
      constexpr uint64_t kAdshMask = (uint64_t{1} << kPackAdshBits) - 1;
      for (const PackedEnt &e : ents_) {
        if (e.c0 == 0) {
          continue;
        }
        const uint32_t u = static_cast<uint32_t>(e.gk >> (kPackAdshBits + kPackTagBits));
        if (excluded[u]) {
          continue;
        }
        groups[u].push_back(BuildEnt{e.k0, e.k1,
                                     static_cast<uint32_t>((e.gk >> kPackTagBits) & kAdshMask),
                                     static_cast<uint32_t>(e.gk & kTagMask), e.c0, e.c1});
      }
    }

  private:
    static constexpr size_t kInitial = 4096;
    void Grow() {
      std::vector<PackedEnt> ents(ents_.size() * 2, PackedEnt{0, 0, 0, 0, 0});
      const size_t mask = ents.size() - 1;
      for (const PackedEnt &e : ents_) {
        if (e.c0 == 0) {
          continue;
        }
        size_t pos = PackHash(e.gk) & mask;
        while (ents[pos].c0 != 0) {
          pos = (pos + 1) & mask;
        }
        ents[pos] = e;
      }
      ents_.swap(ents);
    }

    std::vector<PackedEnt> ents_;
    size_t size_ = 0;
  };

  struct SortItem {
    uint64_t key; // ascending order == k0 descending
    uint64_t idx;
  };

  static void SortItems(std::vector<SortItem> &items) {
    const size_t n = items.size();
    if (n < 4096) {
      std::sort(items.begin(), items.end(),
                [](const SortItem &a, const SortItem &b) { return a.key < b.key; });
      return;
    }
    // LSD radix sort, 11 bits per pass (the histograms of all passes come
    // from one read of the keys), passes with a single bucket skipped.
    constexpr unsigned kBits = 11;
    constexpr unsigned kPasses = (64 + kBits - 1) / kBits;
    constexpr size_t kBuckets = size_t{1} << kBits;
    std::vector<SortItem> tmp(n);
    std::vector<size_t> hist(kPasses * kBuckets, 0);
    for (size_t i = 0; i < n; ++i) {
      const uint64_t k = items[i].key;
      for (unsigned pass = 0; pass < kPasses; ++pass) {
        ++hist[pass * kBuckets + ((k >> (pass * kBits)) & (kBuckets - 1))];
      }
    }
    SortItem *src = items.data();
    SortItem *dst = tmp.data();
    for (unsigned pass = 0; pass < kPasses; ++pass) {
      const unsigned sh = pass * kBits;
      size_t *count = hist.data() + pass * kBuckets;
      if (count[(src[0].key >> sh) & (kBuckets - 1)] == n) {
        continue;
      }
      size_t off = 0;
      for (size_t b = 0; b < kBuckets; ++b) {
        const size_t v = count[b];
        count[b] = off;
        off += v;
      }
      for (size_t i = 0; i < n; ++i) {
        dst[count[(src[i].key >> sh) & (kBuckets - 1)]++] = src[i];
      }
      std::swap(src, dst);
    }
    if (src != items.data()) {
      std::copy(src, src + n, items.data());
    }
  }

  static std::shared_ptr<const Q2Base> Seal(const HugeVec<BuildEnt> &g) {
    if (g.size() >= std::numeric_limits<uint32_t>::max() - 1) {
      throw std::length_error("sec_edgar q2: more than 2^32-2 groups in one unit of measure");
    }
    std::vector<SortItem> items(g.size());
    for (size_t i = 0; i < g.size(); ++i) {
      items[i] = {~(static_cast<uint64_t>(g[i].k0) ^ (uint64_t{1} << 63)), i};
    }
    SortItems(items);
    auto base = std::make_shared<Q2Base>();
    const size_t n = items.size();
    base->n = static_cast<uint32_t>(n);
    base->k0.resize(n);
    base->k1.resize(n);
    base->adsh.resize(n);
    base->tag.resize(n);
    base->c0b.resize(n);
    base->c1b.resize(n);
    constexpr size_t kAhead = 16;
    for (size_t i = 0; i < n; ++i) {
      if (i + kAhead < n) {
        __builtin_prefetch(g.data() + items[i + kAhead].idx, 0, 0);
      }
      const BuildEnt &e = g[items[i].idx];
      const uint32_t c1 = e.c1 & ~kBuildTrunc;
      base->k0[i] = e.k0;
      base->k1[i] = e.k1;
      base->adsh[i] = e.adsh;
      base->tag[i] = e.tag;
      const bool esc0 = e.c0 >= Q2Base::kC0Esc;
      const bool esc1 = c1 >= Q2Base::kC1Esc;
      base->c0b[i] = esc0 ? Q2Base::kC0Esc : static_cast<uint8_t>(e.c0);
      base->c1b[i] = static_cast<uint8_t>((esc1 ? Q2Base::kC1Esc : c1) |
                                          ((e.c1 & kBuildTrunc) ? Q2Base::kTruncBit : 0));
      if (esc0 || esc1) {
        base->e_pos.push_back(static_cast<uint32_t>(i));
        base->e_c0.push_back(e.c0);
        base->e_c1.push_back(c1);
      }
    }
    base->Finish();
    return base;
  }

  SecEdgarQ2State &state_;
  Q2UomDir dir_;
  std::vector<uint64_t> uom_rows_;
  std::vector<Part> parts_;
  std::vector<PackedRec *> cur_, end_; // write cursor of each partition's last block
  std::unique_ptr<Line[]> wc_;         // staged line of each partition
  std::vector<uint8_t> wcn_;           // records in it
  std::vector<HugeRegion> arena_;
  size_t arena_used_ = 0;
  std::vector<WideRec> wide_;
};

} // namespace

std::shared_ptr<const SecEdgarQ2State>
SecEdgarQ2Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q2 requires a typed initial scan source");
  }
  auto state = std::make_shared<SecEdgarQ2State>();
  BuildAggregator agg(*state);
  // The default unit of measure always exists (an empty slice answers it).
  agg.Uom(kSecEdgarQ2DefaultUom);

  // 1. Aggregate (group -> top-2 values) for every unit of measure: one native
  //    pass when the scan source supports it, else distinct value counts.
  bool native = initial_scan->ScanNumIds(
      agg, [&](const SecEdgarQ2IdBatch &batch) { agg.AddBatch(batch); });
  if (!native) {
    // Units over the max_uom_rows opt-out are not aggregated at all.
    std::vector<std::string> wanted;
    initial_scan->ScanUomRowCounts([&](const std::string &uom, uint64_t rows) {
      agg.NoteRows(agg.Uom(uom), rows);
      if (uom == kSecEdgarQ2DefaultUom || rows <= config.max_uom_rows) {
        wanted.push_back(uom);
      }
    });
    if (!wanted.empty()) {
      initial_scan->ScanNumValueCounts(wanted, [&](const SecEdgarQ2ValueCountRow &row) {
        const uint32_t u = agg.Uom(row.uom);
        const uint32_t a = state->adsh.GetOrAdd(row.adsh);
        const uint32_t t = state->tag.GetOrAdd(row.tag);
        agg.AddCounted(u, a, t, Q2ValueKey(row.value), row.rows);
      });
    }
  }
  state->adsh.Compact();
  state->tag.Compact();

  // 2. Subscription info of the adsh values that occur in the groups.
  initial_scan->ScanSub([&](const SecEdgarQ2SubInput &in) {
    const uint32_t id = state->adsh.Find(in.adsh);
    if (id != kQ2None) {
      state->SetSub(id, in.fy_is_null, in.fy, in.name_is_null, in.name);
    }
  });
  state->names.Shrink();

  // 3. Aggregation and sorted bases.
  agg.Finish(services, config.max_uom_rows);
  return state;
}

void SecEdgarQ2Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  builder.Write(core::StateImage::Make(Id(), kStatePartition, 0,
                                       MakeImage(BuildState(services))));
  builder.Finish();
}

// ----------------------------------------------------------- maintenance

namespace {

// The groups one write batch touches, each read from the state once and
// written back once.  Keyed by (uom, adsh, tag).
class BatchGroups {
public:
  struct Work {
    Q2Group g;
    int32_t base_pos; // -2 = in the overlay / unknown, -1 = absent from the base
  };

  BatchGroups(const SecEdgarQ2State &state, size_t expected) : state_(state) {
    size_t cap = 16;
    while (cap < expected * 2) {
      cap <<= 1;
    }
    slots_.assign(cap, 0);
    work_.reserve(expected);
  }

  // Index of the working copy of a group (loaded from the state on first use).
  uint32_t At(uint32_t uom, uint32_t adsh, uint32_t tag) {
    if ((work_.size() + 1) * 2 > slots_.size()) {
      Grow();
    }
    const size_t mask = slots_.size() - 1;
    size_t pos = Hash(uom, adsh, tag) & mask;
    while (true) {
      const uint32_t v = slots_[pos];
      if (v == 0) {
        break;
      }
      const Work &w = work_[v - 1];
      if (w.g.adsh == adsh && w.g.tag == tag && w.g.uom == uom) {
        return v - 1;
      }
      pos = (pos + 1) & mask;
    }
    Work w;
    w.g.uom = uom;
    w.g.adsh = adsh;
    w.g.tag = tag;
    w.base_pos = -1;
    if (const Q2Slice *s = state_.Slice(uom)) {
      const uint64_t h = Q2PairHash(adsh, tag);
      const auto &shard = s->shards[s->ShardOf(h)];
      const int32_t e = shard ? shard->Find(adsh, tag) : -1;
      if (e >= 0) {
        w.g = shard->ents[e];
        w.base_pos = -2;
      } else {
        w.base_pos = s->base->Find(adsh, tag);
        if (w.base_pos >= 0) {
          w.g = s->base->Get(static_cast<uint32_t>(w.base_pos), uom);
        }
      }
    }
    work_.push_back(w);
    slots_[pos] = static_cast<uint32_t>(work_.size());
    return static_cast<uint32_t>(work_.size() - 1);
  }
  Work &operator[](uint32_t i) { return work_[i]; }

  std::vector<Work> &All() { return work_; }

private:
  static uint64_t Hash(uint32_t uom, uint32_t adsh, uint32_t tag) {
    return Q2PairHash(adsh, tag) ^ (static_cast<uint64_t>(uom) * 0xC2B2AE3D27D4EB4Full);
  }
  void Grow() {
    slots_.assign(slots_.size() * 2, 0);
    const size_t mask = slots_.size() - 1;
    for (uint32_t i = 0; i < work_.size(); ++i) {
      size_t pos = Hash(work_[i].g.uom, work_[i].g.adsh, work_[i].g.tag) & mask;
      while (slots_[pos] != 0) {
        pos = (pos + 1) & mask;
      }
      slots_[pos] = i + 1;
    }
  }

  const SecEdgarQ2State &state_;
  std::vector<uint32_t> slots_;
  std::vector<Work> work_;
};

// Applies the buffered deltas to a copy of `old` (every component is shared
// and copied on write: the cost is O(batch + touched overlay shards), plus an
// amortised slice compaction).  Returns nullptr when the delta cannot be
// applied incrementally (caller rebuilds).
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

  // --- num table.  Groups are independent, so applying every insert of the
  // batch before any delete (no spurious "lost" groups when a row's value
  // moves) is done per group on the batch's working copies.
  const auto &nums = buf.Nums();
  BatchGroups work(*ns, nums.size());
  std::vector<uint32_t> new_adsh;
  struct Lost {
    uint32_t uom, adsh, tag;
  };
  std::vector<Lost> lost;
  // Returns the uom id, kQ2None for "not materialised".  `create` adds units
  // of measure that were never seen (they have no rows yet).
  std::string_view last_uom;
  uint32_t last_uid = kQ2None;
  bool have_last = false;
  const auto uom_id = [&](std::string_view uom, bool create, bool &known) -> uint32_t {
    if (have_last && uom == last_uom) {
      known = true;
      return last_uid;
    }
    uint32_t id = cur_dir().names.Find(uom);
    if (id == core::StringDict::kNone) {
      if (!create) {
        known = false;
        return kQ2None;
      }
      if (!dir_w) {
        dir_w = std::make_shared<Q2UomDir>(*ns->uoms);
      }
      id = dir_w->names.Intern(uom);
      dir_w->excluded.push_back(0);
    }
    known = true;
    last_uom = uom;
    last_uid = cur_dir().excluded[id] ? kQ2None : id;
    have_last = true;
    return last_uid;
  };

  // Work index of each insert delta: the delete of a value move (AddNumMove,
  // stored right before its insert with the same strings) reuses it instead
  // of resolving the strings again.
  std::vector<uint32_t> work_of(nums.size(), kQ2None);
  for (size_t di = 0; di < nums.size(); ++di) {
    const auto &d = nums[di];
    if (d.sign < 0) {
      continue;
    }
    bool known = false;
    const uint32_t uid = uom_id(buf.Str(d.uom_off, d.uom_len), true, known);
    if (uid == kQ2None) {
      continue; // uom not materialised
    }
    const std::string_view a = buf.Str(d.adsh_off, d.adsh_len);
    uint32_t aid = ns->adsh.Find(a);
    if (aid == kQ2None) {
      aid = ns->adsh.GetOrAdd(a);
      new_adsh.push_back(aid);
    }
    const uint32_t tid = ns->tag.GetOrAdd(buf.Str(d.tag_off, d.tag_len));
    const uint32_t wi = work.At(uid, aid, tid);
    work_of[di] = wi;
    if (!Q2GroupInsert(work[wi].g, d.key)) {
      return nullptr;
    }
  }
  for (size_t di = 0; di < nums.size(); ++di) {
    const auto &d = nums[di];
    if (d.sign > 0) {
      continue;
    }
    uint32_t wi = kQ2None;
    if (di + 1 < nums.size() && work_of[di + 1] != kQ2None) {
      const auto &n = nums[di + 1];
      if (n.adsh_off == d.adsh_off && n.tag_off == d.tag_off && n.uom_off == d.uom_off &&
          n.adsh_len == d.adsh_len && n.tag_len == d.tag_len && n.uom_len == d.uom_len) {
        wi = work_of[di + 1];
      }
    }
    if (wi == kQ2None) {
      bool known = false;
      const uint32_t uid = uom_id(buf.Str(d.uom_off, d.uom_len), false, known);
      if (!known) {
        return nullptr; // delete from a uom we never saw: state is inconsistent
      }
      if (uid == kQ2None) {
        continue; // uom not materialised
      }
      const uint32_t aid = ns->adsh.Find(buf.Str(d.adsh_off, d.adsh_len));
      const uint32_t tid = ns->tag.Find(buf.Str(d.tag_off, d.tag_len));
      if (aid == kQ2None || tid == kQ2None) {
        return nullptr;
      }
      wi = work.At(uid, aid, tid);
    }
    auto &w = work[wi];
    if (w.g.c0 == 0) {
      return nullptr;
    }
    switch (Q2GroupRemove(w.g, d.key)) {
    case Q2Removal::Ok:
      break;
    case Q2Removal::Lost:
      lost.push_back({w.g.uom, w.g.adsh, w.g.tag});
      break;
    case Q2Removal::Inconsistent:
      SecEdgarQ2GlobalStats().inconsistent_deltas.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
  }
  if (dir_w) {
    ns->uoms = dir_w;
  }
  Q2SliceWriter writer(*ns);
  for (const auto &w : work.All()) {
    writer.Put(w.g, w.base_pos);
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
            const int32_t e = fresh.Find(a, t);
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
        const int32_t e = fresh.Find(l->adsh, l->tag);
        if (e >= 0) {
          g = fresh.ents[e];
        }
        writer.Put(g);
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
  if (const unsigned c = writer.CompactIfNeeded(overlay_limit)) {
    SecEdgarQ2GlobalStats().compactions.fetch_add(c, std::memory_order_relaxed);
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
