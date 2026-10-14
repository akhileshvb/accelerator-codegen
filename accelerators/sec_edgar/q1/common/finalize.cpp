#include "sec_edgar/q1/common/module.hpp"

#include <algorithm>

#include "prims/pg_numeric.hpp"

// Finalize payload: turns the maintained state into ordered result rows.
namespace accelerator_codegen::aux_state::query {
namespace q1 {

std::vector<OutputRow> BuildOutput(const State &state) {
	std::vector<OutputRow> out;
	out.reserve(state.groups.size());
	for (const auto &g : state.groups) {
		if (g.cnt <= 0) {
			continue;
		}
		OutputRow row;
		row.stmt = g.stmt;
		row.rfile = g.rfile;
		row.rfile_is_null = g.rfile_is_null;
		row.cnt = g.cnt;
		row.num_filings = g.distinct;
		row.line_sum = g.line_sum;
		row.line_cnt = g.line_cnt;
		if (g.line_cnt > 0) {
			row.avg_line_num = AvgLine(g.line_sum, g.line_cnt);
		} else {
			row.avg_is_null = true;
		}
		out.push_back(std::move(row));
	}
	std::sort(out.begin(), out.end(), [](const OutputRow &l, const OutputRow &r) {
		if (l.cnt != r.cnt) {
			return l.cnt > r.cnt;
		}
		if (l.stmt != r.stmt) {
			return l.stmt < r.stmt;
		}
		if (l.rfile_is_null != r.rfile_is_null) {
			return l.rfile_is_null;
		}
		return l.rfile < r.rfile;
	});
	return out;
}

} // namespace q1

std::vector<SecEdgarQ1OutputRow> BuildSecEdgarQ1Output(const SecEdgarQ1State &state) {
	return q1::BuildOutput(state);
}

void SecEdgarQ1Module::EmitOutput(const SecEdgarQ1State &state, core::AuxRowSink &sink) {
	sink.Begin({{{"stmt", core::AuxType::String, false},
	             {"rfile", core::AuxType::String, true},
	             {"cnt", core::AuxType::Int64, false},
	             {"num_filings", core::AuxType::Int64, false},
	             {"avg_line_num", core::AuxType::Double, true}}});
	for (auto &row : BuildSecEdgarQ1Output(state)) {
		core::AuxRow out;
		out.values.reserve(5);
		out.values.push_back(core::AuxValue::Of(core::AuxType::String, std::move(row.stmt)));
		out.values.push_back(row.rfile_is_null ? core::AuxValue::Null(core::AuxType::String)
		                                       : core::AuxValue::Of(core::AuxType::String, std::move(row.rfile)));
		out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.cnt));
		out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.num_filings));
		out.values.push_back(row.avg_is_null ? core::AuxValue::Null(core::AuxType::Double)
		                                     : core::AuxValue::Of(core::AuxType::Double, row.avg_line_num));
		sink.WriteRow(out);
	}
	sink.End();
}

// PostgreSQL: AVG(integer) is numeric, sum / count at PostgreSQL's result scale
// (pg_numeric::AvgOfScaledSum), emitted as numeric text.
void SecEdgarQ1Module::EmitOutputNumeric(const SecEdgarQ1State &state, core::AuxRowSink &sink) {
	sink.Begin({{{"stmt", core::AuxType::String, false},
	             {"rfile", core::AuxType::String, true},
	             {"cnt", core::AuxType::Int64, false},
	             {"num_filings", core::AuxType::Int64, false},
	             {"avg_line_num", core::AuxType::String, true}}});
	for (auto &row : BuildSecEdgarQ1Output(state)) {
		core::AuxRow out;
		out.values.reserve(5);
		out.values.push_back(core::AuxValue::Of(core::AuxType::String, std::move(row.stmt)));
		out.values.push_back(row.rfile_is_null ? core::AuxValue::Null(core::AuxType::String)
		                                       : core::AuxValue::Of(core::AuxType::String, std::move(row.rfile)));
		out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.cnt));
		out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.num_filings));
		if (row.avg_is_null) {
			out.values.push_back(core::AuxValue::Null(core::AuxType::String));
		} else {
			const auto avg = ::accelerator_codegen::pg_numeric::AvgOfScaledSum(
			    static_cast<::accelerator_codegen::pg_numeric::Int128>(row.line_sum), 0, row.line_cnt);
			out.values.push_back(core::AuxValue::Of(core::AuxType::String, ::accelerator_codegen::pg_numeric::ToText(avg)));
		}
		sink.WriteRow(out);
	}
	sink.End();
}

} // namespace accelerator_codegen::aux_state::query
