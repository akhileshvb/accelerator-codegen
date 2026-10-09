#include "sec_edgar/q1/common/module.hpp"

#include "accelerator_codegen/stateful/core/byte_stream.hpp"
#include "sec_edgar/q1/common/adsh_key.hpp"

#include "sec_edgar/q1/common/builder_impl.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace accelerator_codegen::aux_state::query {
namespace q1 {
namespace {

constexpr uint32_t kStateMagic = 0x31515345; // "ESQ1"
constexpr uint32_t kStateVersion = 4;
constexpr std::size_t kOtherOverlayMax = 1024;

// Overlay of new accession ids is merged into the sorted base once it exceeds
// ~4*sqrt(base): the per-write copy of the overlay and the amortized merge
// cost are both O(sqrt(n)) per inserted id.
std::size_t OverlayLimit(std::size_t base) {
	return std::max<std::size_t>(2048, static_cast<std::size_t>(4.0 * std::sqrt(static_cast<double>(base))));
}

inline uint32_t PageCountFor(Id ids) {
	return static_cast<uint32_t>((ids + kPageSize - 1) >> kPageBits);
}

void PutInt128(core::ByteWriter &w, Int128 v) {
	const auto u = static_cast<unsigned __int128>(v);
	w.U64(static_cast<uint64_t>(u));
	w.U64(static_cast<uint64_t>(u >> 64));
}

bool GetInt128(core::ByteReader &r, Int128 &v) {
	uint64_t lo = 0, hi = 0;
	if (!r.U64(lo) || !r.U64(hi)) {
		return false;
	}
	v = static_cast<Int128>((static_cast<unsigned __int128>(hi) << 64) | lo);
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// GroupRefs
// ---------------------------------------------------------------------------

uint64_t GroupRefs::Get(Id id) const {
	const uint8_t *p = low.Find(static_cast<int64_t>(id));
	if (p == nullptr || *p == 0) {
		return 0;
	}
	if (*p != kEscape) {
		return *p;
	}
	return ext.Get(static_cast<int64_t>(id));
}

void GroupRefs::Set(Id id, uint64_t count) {
	const auto key = static_cast<int64_t>(id);
	if (count < kEscape) {
		low.Mutable(key) = static_cast<uint8_t>(count);
	} else {
		low.Mutable(key) = kEscape;
		ext.Mutable(key) = count;
	}
}

uint64_t GroupRefs::MemoryBytes() const {
	return sizeof(GroupRefs) + low.MemoryBytes() + ext.MemoryBytes();
}

// ---------------------------------------------------------------------------
// OtherIds
// ---------------------------------------------------------------------------

uint64_t OtherIds::size() const {
	return (base_ ? base_->size() : 0) + (overlay_ ? overlay_->size() : 0);
}

uint64_t OtherIds::Find(std::string_view text) const {
	if (base_) {
		const auto id = base_->Find(text);
		if (id != core::StringDict::kNone) {
			return id;
		}
	}
	if (overlay_) {
		const uint64_t base_n = base_ ? base_->size() : 0;
		for (std::size_t i = 0; i < overlay_->size(); ++i) {
			if ((*overlay_)[i] == text) {
				return base_n + i;
			}
		}
	}
	return kNone;
}

uint64_t OtherIds::Intern(std::string_view text) {
	const uint64_t existing = Find(text);
	if (existing != kNone) {
		return existing;
	}
	const uint64_t base_n = base_ ? base_->size() : 0;
	// Copy-on-write: a published overlay is shared, so clone it before growing.
	auto grown = overlay_ ? std::make_shared<std::vector<std::string>>(*overlay_)
	                      : std::make_shared<std::vector<std::string>>();
	grown->emplace_back(text);
	const uint64_t index = base_n + grown->size() - 1;
	if (grown->size() >= kOtherOverlayMax) {
		auto merged = base_ ? std::make_shared<core::StringDict>(*base_) : std::make_shared<core::StringDict>();
		for (const auto &s : *grown) {
			merged->Intern(s);
		}
		base_ = std::move(merged);
		overlay_.reset();
	} else {
		overlay_ = std::move(grown);
	}
	return index;
}

uint64_t OtherIds::MemoryBytes() const {
	uint64_t bytes = 0;
	if (base_) {
		bytes += base_->MemoryBytes();
	}
	if (overlay_) {
		for (const auto &s : *overlay_) {
			bytes += sizeof(std::string) + s.capacity();
		}
	}
	return bytes;
}

void OtherIds::ForEach(const std::function<void(std::string_view, uint64_t)> &fn) const {
	uint64_t index = 0;
	if (base_) {
		for (; index < base_->size(); ++index) {
			fn(base_->Get(static_cast<uint32_t>(index)), index);
		}
	}
	if (overlay_) {
		for (const auto &s : *overlay_) {
			fn(s, index++);
		}
	}
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

Id State::FindId(uint64_t key) const {
	if (dict && !dict->keys.empty()) {
		const auto &keys = dict->keys;
		const auto it = std::lower_bound(keys.begin(), keys.end(), key);
		if (it != keys.end() && *it == key) {
			return dict->IdAt(static_cast<std::size_t>(it - keys.begin()));
		}
	}
	if (overlay && !overlay->empty()) {
		const auto it = std::lower_bound(overlay->begin(), overlay->end(), key,
		                                 [](const std::pair<uint64_t, Id> &entry, uint64_t k) { return entry.first < k; });
		if (it != overlay->end() && it->first == key) {
			return it->second;
		}
	}
	return kNoId;
}

Id State::GetOrCreateId(uint64_t key) {
	const Id existing = FindId(key);
	if (existing != kNoId) {
		return existing;
	}
	const Id id = next_id++;
	// Copy-on-write: clone the overlay only if a published state still shares it.
	if (!overlay) {
		overlay = std::make_shared<std::vector<std::pair<uint64_t, Id>>>();
	} else if (overlay.use_count() > 1) {
		overlay = std::make_shared<std::vector<std::pair<uint64_t, Id>>>(*overlay);
	}
	auto &entries = *std::const_pointer_cast<std::vector<std::pair<uint64_t, Id>>>(overlay);
	const auto it = std::lower_bound(entries.begin(), entries.end(), key,
	                                 [](const std::pair<uint64_t, Id> &entry, uint64_t k) { return entry.first < k; });
	entries.insert(it, {key, id});
	if (entries.size() >= OverlayLimit(dict ? dict->keys.size() : 0)) {
		CompactDictionary();
	}
	return id;
}

void State::CompactDictionary() {
	if (!overlay || overlay->empty()) {
		return;
	}
	auto merged = std::make_shared<DictBase>();
	const std::size_t base_n = dict ? dict->keys.size() : 0;
	const auto &add = *overlay;
	merged->keys.reserve(base_n + add.size());
	merged->ids.reserve(base_n + add.size());
	std::size_t i = 0;
	std::size_t j = 0;
	while (i < base_n || j < add.size()) {
		if (j >= add.size() || (i < base_n && dict->keys[i] < add[j].first)) {
			merged->keys.push_back(dict->keys[i]);
			merged->ids.push_back(dict->IdAt(i));
			++i;
		} else {
			merged->keys.push_back(add[j].first);
			merged->ids.push_back(add[j].second);
			++j;
		}
	}
	dict = std::move(merged);
	overlay.reset();
}

int State::FindGroup(const std::string &stmt, const std::string &rfile, bool rfile_is_null) const {
	for (std::size_t i = 0; i < groups.size(); ++i) {
		const auto &g = groups[i];
		if (g.rfile_is_null == rfile_is_null && g.stmt == stmt && (rfile_is_null || g.rfile == rfile)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int State::GetOrCreateGroup(const std::string &stmt, const std::string &rfile, bool rfile_is_null) {
	const int existing = FindGroup(stmt, rfile, rfile_is_null);
	if (existing >= 0) {
		return existing;
	}
	GroupData group;
	group.stmt = stmt;
	group.rfile = rfile_is_null ? std::string() : rfile;
	group.rfile_is_null = rfile_is_null;
	groups.push_back(std::move(group));
	return static_cast<int>(groups.size() - 1);
}

uint64_t State::MemoryBytes() const {
	uint64_t bytes = sizeof(State) + groups.capacity() * sizeof(GroupData);
	for (const auto &g : groups) {
		bytes += g.stmt.capacity() + g.rfile.capacity();
		if (g.refs) {
			bytes += g.refs->MemoryBytes();
		}
	}
	if (dict) {
		bytes += dict->keys.capacity() * sizeof(uint64_t) + dict->ids.capacity() * sizeof(Id);
	}
	if (overlay) {
		bytes += overlay->capacity() * sizeof(std::pair<uint64_t, Id>);
	}
	return bytes + others.MemoryBytes();
}

// ---------------------------------------------------------------------------
// Encode / Decode (sparse in zero chunks)
// ---------------------------------------------------------------------------

std::vector<uint8_t> Encode(const State &input) {
	State state = input;
	state.CompactDictionary();
	core::ByteWriter w;
	w.U32(kStateMagic);
	w.U32(kStateVersion);
	w.U64(state.next_id);
	w.U64(state.ids_at_build);
	w.U32(static_cast<uint32_t>(state.groups.size()));
	for (const auto &g : state.groups) {
		w.String(g.stmt);
		w.String(g.rfile);
		w.U8(g.rfile_is_null ? 1 : 0);
		w.I64(g.cnt);
		PutInt128(w, g.line_sum);
		w.I64(g.line_cnt);
		w.I64(g.distinct);
		// Live chunks of the byte array, then the exact counts of escaped slots.
		std::vector<uint64_t> live;
		std::vector<std::pair<uint64_t, uint64_t>> escaped;
		if (g.refs) {
			const auto &low = g.refs->low;
			for (std::size_t ci = 0; ci < low.NumChunks(); ++ci) {
				const auto *chunk = low.ChunkAt(ci);
				if (!chunk) {
					continue;
				}
				bool any = false;
				for (uint32_t i = 0; i < kPageSize; ++i) {
					const uint8_t v = (*chunk)[i];
					any |= v != 0;
					if (v == kEscape) {
						const Id id = (Id(ci) << kPageBits) + i;
						escaped.emplace_back(id, g.refs->ext.Get(static_cast<int64_t>(id)));
					}
				}
				if (any) {
					live.push_back(ci);
				}
			}
		}
		w.U64(live.size());
		for (const uint64_t ci : live) {
			w.U64(ci);
			const auto *chunk = g.refs->low.ChunkAt(static_cast<std::size_t>(ci));
			for (uint32_t i = 0; i < kPageSize; ++i) {
				w.U8((*chunk)[i]);
			}
		}
		w.U64(escaped.size());
		for (const auto &entry : escaped) {
			w.U64(entry.first);
			w.U64(entry.second);
		}
	}
	const uint64_t dict_n = state.dict ? state.dict->keys.size() : 0;
	const bool identity = !state.dict || state.dict->ids.empty();
	w.U64(dict_n);
	w.U8(identity ? 1 : 0);
	for (uint64_t i = 0; i < dict_n; ++i) {
		w.U64(state.dict->keys[i]);
	}
	if (!identity) {
		for (uint64_t i = 0; i < dict_n; ++i) {
			w.U64(state.dict->ids[i]);
		}
	}
	w.U64(state.others.size());
	state.others.ForEach([&](std::string_view text, uint64_t) { w.String(std::string(text)); });
	return std::move(w.bytes);
}

bool Decode(const std::vector<uint8_t> &bytes, State &state) {
	core::ByteReader r{bytes};
	uint32_t magic = 0, version = 0, group_count = 0;
	state = State();
	if (!r.U32(magic) || magic != kStateMagic || !r.U32(version) || version != kStateVersion ||
	    !r.U64(state.next_id) || !r.U64(state.ids_at_build) || !r.U32(group_count)) {
		return false;
	}
	for (uint32_t gi = 0; gi < group_count; ++gi) {
		GroupData g;
		uint8_t null_flag = 0;
		uint64_t live = 0;
		if (!r.String(g.stmt) || !r.String(g.rfile) || !r.U8(null_flag) || !r.I64(g.cnt) || !GetInt128(r, g.line_sum) ||
		    !r.I64(g.line_cnt) || !r.I64(g.distinct) || !r.U64(live)) {
			return false;
		}
		g.rfile_is_null = null_flag != 0;
		auto refs = std::make_shared<GroupRefs>();
		for (uint64_t k = 0; k < live; ++k) {
			uint64_t ci = 0;
			if (!r.U64(ci)) {
				return false;
			}
			for (uint32_t i = 0; i < kPageSize; ++i) {
				uint8_t v = 0;
				if (!r.U8(v)) {
					return false;
				}
				if (v != 0) {
					refs->low.Mutable(static_cast<int64_t>((ci << kPageBits) + i)) = v;
				}
			}
		}
		uint64_t escaped = 0;
		if (!r.U64(escaped)) {
			return false;
		}
		for (uint64_t k = 0; k < escaped; ++k) {
			uint64_t id = 0, count = 0;
			if (!r.U64(id) || !r.U64(count)) {
				return false;
			}
			refs->ext.Mutable(static_cast<int64_t>(id)) = count;
		}
		g.refs = std::move(refs);
		state.groups.push_back(std::move(g));
	}
	uint64_t dict_n = 0;
	uint8_t identity = 0;
	if (!r.U64(dict_n) || !r.U8(identity)) {
		return false;
	}
	auto dict = std::make_shared<DictBase>();
	dict->keys.resize(dict_n);
	for (uint64_t i = 0; i < dict_n; ++i) {
		if (!r.U64(dict->keys[i])) {
			return false;
		}
	}
	if (!identity) {
		dict->ids.resize(dict_n);
		for (uint64_t i = 0; i < dict_n; ++i) {
			if (!r.U64(dict->ids[i])) {
				return false;
			}
		}
	}
	state.dict = std::move(dict);
	uint64_t others = 0;
	if (!r.U64(others)) {
		return false;
	}
	for (uint64_t i = 0; i < others; ++i) {
		std::string s;
		if (!r.String(s)) {
			return false;
		}
		if (state.others.Intern(s) != i) {
			return false;
		}
	}
	return r.offset == bytes.size();
}

double AvgLine(Int128 sum, int64_t count) {
	return static_cast<double>(static_cast<long double>(sum) / static_cast<long double>(count));
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

Builder::Builder() : impl(new Impl()) {
}
Builder::~Builder() = default;
Builder::Builder(Builder &&) noexcept = default;
Builder &Builder::operator=(Builder &&) noexcept = default;

void Builder::ConsumeRow(const SecEdgarQ1PreInput &row) {
	ConsumeRowView(row.stmt.data(), row.stmt.size(), row.stmt_is_null, row.rfile.data(), row.rfile.size(),
	               row.rfile_is_null, row.adsh.data(), row.adsh.size(), row.adsh_is_null, row.line,
	               row.line_is_null);
}

void Builder::ConsumeRowView(const char *stmt, std::size_t stmt_len, bool stmt_null, const char *rfile,
                             std::size_t rfile_len, bool rfile_null, const char *adsh, std::size_t adsh_len,
                             bool adsh_null, int32_t line, bool line_null) {
	auto &im = *impl;
	if (stmt_null) {
		return;
	}
	Impl::G *g = im.memo_group >= 0 ? &im.groups[static_cast<std::size_t>(im.memo_group)] : nullptr;
	if (g == nullptr || g->rfile_null != rfile_null || g->stmt.size() != stmt_len ||
	    std::memcmp(g->stmt.data(), stmt, stmt_len) != 0 ||
	    (!rfile_null && (g->rfile.size() != rfile_len || std::memcmp(g->rfile.data(), rfile, rfile_len) != 0))) {
		im.memo_group = im.GroupFor(stmt, stmt_len, rfile, rfile_len, rfile_null);
		g = &im.groups[static_cast<std::size_t>(im.memo_group)];
	}
	g->cnt++;
	if (!line_null) {
		g->line_sum += line;
		g->line_cnt++;
	}
	if (adsh_null) {
		return;
	}
	uint32_t id;
	if (adsh_len == im.memo_adsh_len && std::memcmp(adsh, im.memo_adsh, adsh_len) == 0) {
		id = im.memo_id; // consecutive rows of one filing
	} else {
		uint64_t key = 0;
		if (!ParseAdshKey(adsh, adsh_len, key)) {
			key = im.OtherKey(adsh, adsh_len);
		}
		id = im.IdFor(key);
		if (adsh_len <= Impl::kMemoAdshMax) {
			std::memcpy(im.memo_adsh, adsh, adsh_len);
			im.memo_adsh_len = adsh_len;
			im.memo_id = id;
		} else {
			im.memo_adsh_len = ~std::size_t(0);
		}
	}
	im.Bump(*g, id);
}

void Builder::Merge(Builder &&other_builder) {
	auto &im = *impl;
	auto &ot = *other_builder.impl;
	std::vector<std::string_view> other_strings(ot.others.size());
	for (uint32_t i = 0; i < ot.others.size(); ++i) {
		other_strings[i] = ot.others.Get(i);
	}
	std::vector<uint32_t> remap(ot.id_keys.size());
	for (std::size_t id = 0; id < ot.id_keys.size(); ++id) {
		uint64_t key = ot.id_keys[id];
		if (key & kOtherKeyBit) {
			const auto s = other_strings[key & ~kOtherKeyBit];
			key = im.OtherKey(s.data(), s.size());
		}
		remap[id] = im.IdFor(key);
	}
	for (auto &og : ot.groups) {
		const int gi = im.GroupFor(og.stmt.data(), og.stmt.size(), og.rfile.data(), og.rfile.size(), og.rfile_null);
		auto &g = im.groups[static_cast<std::size_t>(gi)];
		g.cnt += og.cnt;
		g.line_sum += og.line_sum;
		g.line_cnt += og.line_cnt;
		if (g.counts.size() < im.id_keys.size()) {
			g.counts.resize(im.id_keys.size(), 0);
		}
		for (std::size_t id = 0; id < og.counts.size(); ++id) {
			if (og.counts[id]) {
				im.Bump(g, remap[id], og.counts[id]);
			}
		}
	}
}

State Builder::Finish() {
	auto &im = *impl;
	State state;
	const uint32_t n = static_cast<uint32_t>(im.id_keys.size());
	state.next_id = n;
	state.ids_at_build = n;

	// Final ids are the rank of the key in sorted order, so the dictionary needs
	// no id array (DictBase::ids stays empty == identity).
	std::vector<uint32_t> local_of_rank(n);
	auto dict = std::make_shared<DictBase>();
	{
		std::vector<std::pair<uint64_t, uint32_t>> sorted(n);
		for (uint32_t id = 0; id < n; ++id) {
			sorted[id] = {im.id_keys[id], id};
		}
		std::sort(sorted.begin(), sorted.end());
		dict->keys.resize(n);
		for (uint32_t i = 0; i < n; ++i) {
			dict->keys[i] = sorted[i].first;
			local_of_rank[i] = sorted[i].second;
		}
	}
	state.dict = std::move(dict);
	for (uint32_t i = 0; i < im.others.size(); ++i) {
		state.others.Intern(im.others.Get(i)); // same order => same indexes
	}

	std::vector<std::size_t> order(im.groups.size());
	for (std::size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
		const auto &l = im.groups[a];
		const auto &r = im.groups[b];
		if (l.stmt != r.stmt) {
			return l.stmt < r.stmt;
		}
		if (l.rfile_null != r.rfile_null) {
			return l.rfile_null;
		}
		return l.rfile < r.rfile;
	});
	const uint32_t page_count = PageCountFor(n);
	for (const auto gi : order) {
		auto &src = im.groups[gi];
		if (src.cnt == 0) {
			continue;
		}
		GroupData g;
		g.stmt = src.stmt;
		g.rfile = src.rfile;
		g.rfile_is_null = src.rfile_null;
		g.cnt = src.cnt;
		g.line_sum = src.line_sum;
		g.line_cnt = src.line_cnt;
		auto refs = std::make_shared<GroupRefs>();
		int64_t distinct = 0;
		uint8_t work[kPageSize];
		for (uint32_t p = 0; p < page_count; ++p) {
			const Id base = Id(p) << kPageBits;
			bool any = false;
			for (uint32_t i = 0; i < kPageSize; ++i) {
				const Id rank = base + i;
				uint32_t v = 0;
				if (rank < n) {
					const uint32_t local = local_of_rank[rank];
					v = local < src.counts.size() ? src.counts[local] : 0;
				}
				if (v == 0) {
					work[i] = 0;
					continue;
				}
				any = true;
				++distinct;
				if (v >= kEscape) {
					work[i] = kEscape;
					refs->ext.Mutable(static_cast<int64_t>(rank)) = v;
				} else {
					work[i] = static_cast<uint8_t>(v);
				}
			}
			if (any) {
				std::memcpy(&refs->low.Mutable(static_cast<int64_t>(base)), work, kPageSize);
			}
		}
		g.distinct = distinct;
		g.refs = std::move(refs);
		state.groups.push_back(std::move(g));
	}
	return state;
}

} // namespace q1

std::vector<uint8_t> EncodeSecEdgarQ1State(const SecEdgarQ1State &state) {
	return q1::Encode(state);
}

bool DecodeSecEdgarQ1State(const std::vector<uint8_t> &bytes, SecEdgarQ1State &state) {
	return q1::Decode(bytes, state);
}

} // namespace accelerator_codegen::aux_state::query
