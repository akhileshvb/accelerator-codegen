// DuckDB column-chunk entry of the SEC EDGAR Q1 build: vectorised, run-based
// aggregation of (stmt, rfile, adsh, line) DataChunks into a q1::Builder.
#include "sec_edgar/q1/common/builder_impl.hpp"
#include "sec_edgar/q1/duckdb/aux/duckdb_adsh_key.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen::aux_state::query {
namespace q1 {

void Builder::ConsumeChunk(::duckdb::DataChunk &chunk) {
	using namespace ::duckdb;
	auto &im = *impl;
	const idx_t n = chunk.size();
	if (n == 0) {
		return;
	}
	UnifiedVectorFormat u_stmt, u_rfile, u_adsh, u_line;
	chunk.data[0].ToUnifiedFormat(n, u_stmt);
	chunk.data[1].ToUnifiedFormat(n, u_rfile);
	chunk.data[2].ToUnifiedFormat(n, u_adsh);
	chunk.data[3].ToUnifiedFormat(n, u_line);
	const auto *stmt_data = UnifiedVectorFormat::GetData<string_t>(u_stmt);
	const auto *rfile_data = UnifiedVectorFormat::GetData<string_t>(u_rfile);
	const auto *adsh_data = UnifiedVectorFormat::GetData<string_t>(u_adsh);
	const auto *line_data = UnifiedVectorFormat::GetData<int32_t>(u_line);
	const bool line_all_valid = u_line.validity.AllValid();

	// Pass 1: split the chunk into runs of rows that share (stmt, rfile, adsh)
	// *vector positions*. Storage hands out dictionary vectors with long runs
	// (about 12 rows per accession), so per-run work (group lookup, adsh
	// parse, hash probe) is amortized over the run.
	auto &runs = im.runs;
	runs.clear();
	{
		idx_t ps = u_stmt.sel->get_index(0), pr = u_rfile.sel->get_index(0), pa = u_adsh.sel->get_index(0);
		uint32_t start = 0;
		for (idx_t i = 1; i < n; ++i) {
			const idx_t si = u_stmt.sel->get_index(i);
			const idx_t ri = u_rfile.sel->get_index(i);
			const idx_t ai = u_adsh.sel->get_index(i);
			if (si != ps || ri != pr || ai != pa) {
				runs.push_back({start, static_cast<uint32_t>(i - start), static_cast<uint32_t>(ps),
				                static_cast<uint32_t>(pr), static_cast<uint32_t>(pa)});
				start = static_cast<uint32_t>(i);
				ps = si;
				pr = ri;
				pa = ai;
			}
		}
		runs.push_back({start, static_cast<uint32_t>(n - start), static_cast<uint32_t>(ps), static_cast<uint32_t>(pr),
		                static_cast<uint32_t>(pa)});
	}

	// Pass 2: SIMD-parse the accession of every run that counts (stmt and adsh
	// non-null) in one batch.
	im.adsh_sel.clear();
	for (const auto &run : runs) {
		if (u_stmt.validity.RowIsValid(run.si) && u_adsh.validity.RowIsValid(run.ai)) {
			im.adsh_sel.push_back(run.ai);
		}
	}
	im.adsh_keys.resize(im.adsh_sel.size());
	ParseAdshKeyBatch(adsh_data, im.adsh_sel.data(), im.adsh_sel.size(), im.adsh_keys.data());

	// Pass 3: aggregate runs.
	std::size_t kp = 0;
	Impl::G *g = nullptr;
	idx_t gs = ~idx_t(0), gr = ~idx_t(0);
	uint64_t last_key = kInvalidAdshKey;
	uint32_t last_id = 0;
	for (const auto &run : runs) {
		if (!u_stmt.validity.RowIsValid(run.si)) {
			continue;
		}
		const bool rfile_null = !u_rfile.validity.RowIsValid(run.ri);
		if (g == nullptr || run.si != gs || run.ri != gr) {
			const string_t &stmt = stmt_data[run.si];
			const string_t rfile = rfile_null ? string_t() : rfile_data[run.ri];
			const bool same = g != nullptr && g->rfile_null == rfile_null && g->stmt.size() == stmt.GetSize() &&
			                  std::memcmp(g->stmt.data(), stmt.GetData(), stmt.GetSize()) == 0 &&
			                  (rfile_null || (g->rfile.size() == rfile.GetSize() &&
			                                  std::memcmp(g->rfile.data(), rfile.GetData(), rfile.GetSize()) == 0));
			if (!same) {
				const int gi = im.GroupFor(stmt.GetData(), stmt.GetSize(), rfile.GetData(), rfile.GetSize(), rfile_null);
				g = &im.groups[static_cast<std::size_t>(gi)];
			}
			gs = run.si;
			gr = run.ri;
		}
		g->cnt += run.len;
		const uint32_t end = run.start + run.len;
		if (line_all_valid) {
			int64_t sum = 0;
			for (uint32_t i = run.start; i < end; ++i) {
				sum += line_data[u_line.sel->get_index(i)];
			}
			g->line_sum += sum;
			g->line_cnt += run.len;
		} else {
			for (uint32_t i = run.start; i < end; ++i) {
				const idx_t li = u_line.sel->get_index(i);
				if (u_line.validity.RowIsValid(li)) {
					g->line_sum += line_data[li];
					g->line_cnt++;
				}
			}
		}
		if (!u_adsh.validity.RowIsValid(run.ai)) {
			continue;
		}
		uint64_t key = im.adsh_keys[kp++];
		if (key == kInvalidAdshKey) {
			const string_t &adsh = adsh_data[run.ai];
			key = im.OtherKey(adsh.GetData(), adsh.GetSize());
		}
		if (key != last_key) {
			last_key = key;
			last_id = im.IdFor(key);
		}
		im.Bump(*g, last_id, run.len);
	}
}


} // namespace q1
} // namespace accelerator_codegen::aux_state::query
