//! Stateless TPC-H accelerator matrix: for every query and manifest seed, check
//! accel_tpch_qN (single-threaded) and accel_tpch_qN_parallel against the
//! reference SQL, and time both against DuckDB.
//!
//!   stateless_matrix [--queries 1,6,14] [--seeds 5] [--reps 5] [--warmup 2]
//!                    [--threads 8] [--mode check|bench|both|sweep] [--threads-list 16,32]
//!                    [--dump DIR] [--emit-instances FILE] [--scale-factor 1]
//!
//! The database is opened READ_ONLY so several runs can share one file.
//! --dump writes each accelerator result (rendered rows) to DIR/qN_<seed>_{st,par}.txt
//! so two builds can be diffed exactly.

#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/result_compare.hpp"
#include "tpch/q1/tests/q1_manifest.hpp"
#include "tpch/q2/tests/q2_manifest.hpp"
#include "tpch/q3/tests/q3_manifest.hpp"
#include "tpch/q4/tests/q4_manifest.hpp"
#include "tpch/q5/tests/q5_manifest.hpp"
#include "tpch/q6/tests/q6_manifest.hpp"
#include "tpch/q7/tests/q7_manifest.hpp"
#include "tpch/q8/tests/q8_manifest.hpp"
#include "tpch/q9/tests/q9_manifest.hpp"
#include "tpch/q10/tests/q10_manifest.hpp"
#include "tpch/q11/tests/q11_manifest.hpp"
#include "tpch/q12/tests/q12_manifest.hpp"
#include "tpch/q13/tests/q13_manifest.hpp"
#include "tpch/q14/tests/q14_manifest.hpp"
#include "tpch/q15/tests/q15_manifest.hpp"
#include "tpch/q16/tests/q16_manifest.hpp"
#include "tpch/q17/tests/q17_manifest.hpp"
#include "tpch/q17/tests/q17_nonzero_manifest.hpp"
#include "tpch/q18/tests/q18_manifest.hpp"
#include "tpch/q19/tests/q19_manifest.hpp"
#include "tpch/q20/tests/q20_manifest.hpp"
#include "tpch/q21/tests/q21_manifest.hpp"
#include "tpch/q22/tests/q22_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

namespace ts = accelerator_codegen::test_support;
using Clock = std::chrono::steady_clock;

struct Instance {
	std::string label;
	std::string ref_sql;
	std::string args; // "(...)" argument list for accel_tpch_qN / _parallel
};

std::string Esc(const std::string &value) {
	std::string out;
	for (char c : value) {
		out += c;
		if (c == '\'') {
			out += '\'';
		}
	}
	return "'" + out + "'";
}

std::string StripSemicolon(std::string sql) {
	while (!sql.empty() && (sql.back() == ';' || std::isspace(static_cast<unsigned char>(sql.back())))) {
		sql.pop_back();
	}
	return sql;
}

void ReplaceOnce(std::string &s, const std::string &from, const std::string &to) {
	const auto pos = s.find(from);
	ts::Require(pos != std::string::npos, "reference SQL template missing: " + from);
	s.replace(pos, from.size(), to);
}

//! Shortest decimal that round-trips (so 0.0001/20 prints as 5e-06, an exact SQL literal).
std::string FormatDouble(double v) {
	for (int precision = 1; precision <= 17; precision++) {
		std::ostringstream out;
		out.precision(precision);
		out << v;
		if (std::stod(out.str()) == v) {
			return out.str();
		}
	}
	std::ostringstream out;
	out.precision(17);
	out << v;
	return out.str();
}

std::string Date(const char *d) {
	return std::string("DATE '") + d + "'";
}

template <class M, class F>
std::vector<Instance> Build(const M &manifest, F &&format) {
	std::vector<Instance> out;
	for (const auto &e : manifest) {
		out.push_back({e.label, ts::ReadFile(e.sql_path), "(" + format(e) + ")"});
	}
	return out;
}

//! TPC-H scale factor of the database (only Q11's FRACTION depends on it).
double g_scale_factor = 1.0;

std::vector<Instance> InstancesFor(int q) {
	using std::to_string;
	switch (q) {
	case 1:
		return Build(ts::kQ1Manifest, [](auto &e) { return to_string(e.day_offset); });
	case 2: {
		// The generated Q2 instances lost the spec's leading wildcard (p_type LIKE '%BRASS'
		// became LIKE 'BRASS', which matches nothing); restore it in both the reference and
		// the accelerator argument (a LIKE pattern).
		auto out = Build(ts::kQ2Manifest, [](auto &e) {
			return to_string(e.part_size) + ", " + Esc(std::string("%") + e.part_type_pattern) + ", " +
			       Esc(e.region_name);
		});
		for (size_t i = 0; i < out.size(); i++) {
			const std::string t = ts::kQ2Manifest[i].part_type_pattern;
			ReplaceOnce(out[i].ref_sql, "LIKE '" + t + "'", "LIKE '%" + t + "'");
		}
		return out;
	}
	case 3:
		return Build(ts::kQ3Manifest,
		             [](auto &e) { return Esc(e.market_segment) + ", " + Date(e.cutoff_date); });
	case 4:
		return Build(ts::kQ4Manifest, [](auto &e) { return Date(e.start_date); });
	case 5:
		return Build(ts::kQ5Manifest, [](auto &e) { return Esc(e.region_name) + ", " + Date(e.start_date); });
	case 6:
		return Build(ts::kQ6Manifest, [](auto &e) {
			return Date(e.start_date) + ", " + e.discount + ", " + e.quantity;
		});
	case 7:
		return Build(ts::kQ7Manifest, [](auto &e) { return Esc(e.nation1) + ", " + Esc(e.nation2); });
	case 8:
		return Build(ts::kQ8Manifest, [](auto &e) {
			return Esc(e.nation_name) + ", " + Esc(e.region_name) + ", " + Esc(e.part_type);
		});
	case 9:
		return Build(ts::kQ9Manifest, [](auto &e) { return Esc(e.color); });
	case 10: {
		// The generated Q10 instances omit the spec's LIMIT 20.
		auto out = Build(ts::kQ10Manifest, [](auto &e) { return Date(e.start_date); });
		for (auto &inst : out) {
			inst.ref_sql = StripSemicolon(inst.ref_sql) + "\nLIMIT 20";
		}
		return out;
	}
	case 11: {
		// The generated Q11 instances render FRACTION as "* 1"; substitute the manifest value
		// (the SF1 FRACTION 0.0001), divided by the scale factor as the TPC-H spec prescribes.
		auto fraction = [](const auto &e) { return FormatDouble(e.fraction / g_scale_factor); };
		auto out = Build(ts::kQ11Manifest, [&](auto &e) { return Esc(e.nation_name) + ", " + fraction(e); });
		for (size_t i = 0; i < out.size(); i++) {
			ReplaceOnce(out[i].ref_sql, "ps_availqty) * 1\r\n", "ps_availqty) * " + fraction(ts::kQ11Manifest[i]) + "\n");
			// ORDER BY value DESC leaves equal values unordered (they occur at SF20); the
			// accelerators break ties by ps_partkey, so make the reference do the same.
			ReplaceOnce(out[i].ref_sql, "value DESC;", "value DESC, ps_partkey;");
		}
		return out;
	}
	case 12:
		return Build(ts::kQ12Manifest, [](auto &e) {
			return Esc(e.shipmode1) + ", " + Esc(e.shipmode2) + ", " + Date(e.start_date);
		});
	case 13:
		return Build(ts::kQ13Manifest, [](auto &e) { return Esc(e.comment_pattern); });
	case 14:
		return Build(ts::kQ14Manifest, [](auto &e) { return Date(e.start_date); });
	case 15:
		return Build(ts::kQ15Manifest, [](auto &e) { return Date(e.start_date); });
	case 16:
		return Build(ts::kQ16Manifest, [](auto &e) {
			std::string s = Esc(e.brand) + ", " + Esc(e.type_pattern);
			for (auto size : e.sizes) {
				s += ", " + to_string(size);
			}
			return s;
		});
	case 17: {
		// Most generated Q17 instances select no parts (NULL result).  Interleave the nonzero
		// brand/container pairs, with reference SQL made from the seed_1 template.
		auto format = [](auto &e) { return Esc(e.brand) + ", " + Esc(e.container); };
		auto base = Build(ts::kQ17Manifest, format);
		const auto &tmpl_entry = ts::kQ17Manifest[0];
		std::vector<Instance> out;
		for (size_t i = 0; i < base.size(); i++) {
			if (i < ts::kQ37NonzeroManifest.size()) {
				const auto &e = ts::kQ37NonzeroManifest[i];
				auto sql = base[0].ref_sql;
				ReplaceOnce(sql, Esc(tmpl_entry.brand), Esc(e.brand));
				ReplaceOnce(sql, Esc(tmpl_entry.container), Esc(e.container));
				out.push_back({e.label, sql, "(" + format(e) + ")"});
			}
			out.push_back(base[i]);
		}
		return out;
	}
	case 18:
		return Build(ts::kQ18Manifest, [](auto &e) { return to_string(e.quantity_threshold); });
	case 19:
		return Build(ts::kQ19Manifest, [](auto &e) {
			return Esc(e.brand1) + ", " + to_string(e.qty_low1) + ", " + Esc(e.brand2) + ", " +
			       to_string(e.qty_low2) + ", " + Esc(e.brand3) + ", " + to_string(e.qty_low3);
		});
	case 20:
		return Build(ts::kQ20Manifest, [](auto &e) {
			return Date(e.start_date) + ", " + Esc(e.part_name_pattern) + ", " + Esc(e.nation_name);
		});
	case 21:
		return Build(ts::kQ21Manifest, [](auto &e) { return Esc(e.nation_name); });
	case 22: {
		auto codes = [](auto &e) {
			std::string s;
			for (size_t i = 0; i < e.country_codes.size(); i++) {
				s += (i ? ", " : "") + Esc(e.country_codes[i]);
			}
			return s;
		};
		// The generated instances compare SUBSTRING(c_phone ...) to integer literals, which
		// PostgreSQL rejects; quote the country codes (same result in DuckDB).
		auto out = Build(ts::kQ22Manifest, codes);
		for (size_t i = 0; i < out.size(); i++) {
			std::string bare;
			for (size_t k = 0; k < ts::kQ22Manifest[i].country_codes.size(); k++) {
				bare += (k ? ", " : "") + std::string(ts::kQ22Manifest[i].country_codes[k]);
			}
			const auto quoted = codes(ts::kQ22Manifest[i]);
			for (int occurrence = 0; occurrence < 2; occurrence++) {
				ReplaceOnce(out[i].ref_sql, "IN (" + bare + ")", "IN (" + quoted + ")");
			}
		}
		return out;
	}
	default:
		throw std::runtime_error("unknown query " + std::to_string(q));
	}
}

std::unique_ptr<duckdb::MaterializedQueryResult> Run(duckdb::Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	ts::Require(result != nullptr, "null result: " + sql);
	ts::Require(!result->HasError(), result->GetError() + "\n  sql: " + sql.substr(0, 200));
	return std::move(result);
}

void SetThreads(duckdb::Connection &con, int threads) {
	Run(con, "PRAGMA threads=" + std::to_string(threads));
}

// Empty string = equal; otherwise a description of the first difference.
std::string Diff(duckdb::MaterializedQueryResult &expected, duckdb::MaterializedQueryResult &actual) {
	if (expected.ColumnCount() != actual.ColumnCount()) {
		return "column count " + std::to_string(expected.ColumnCount()) + " vs " +
		       std::to_string(actual.ColumnCount());
	}
	if (expected.RowCount() != actual.RowCount()) {
		return "row count " + std::to_string(expected.RowCount()) + " vs " + std::to_string(actual.RowCount());
	}
	for (duckdb::idx_t row = 0; row < expected.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < expected.ColumnCount(); col++) {
			auto ev = expected.GetValue(col, row);
			auto av = actual.GetValue(col, row);
			if (ev.IsNull() || av.IsNull() ? ev.IsNull() != av.IsNull() : !ts::ValuesEqualCell(ev, av)) {
				return "row " + std::to_string(row) + " col " + std::to_string(col) + ": " + ev.ToString() +
				       " vs " + av.ToString();
			}
		}
	}
	return "";
}

std::string Render(duckdb::MaterializedQueryResult &result) {
	std::ostringstream out;
	for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < result.ColumnCount(); col++) {
			out << (col ? " | " : "") << result.GetValue(col, row).ToString();
		}
		out << '\n';
	}
	return out.str();
}

double MedianUs(duckdb::Connection &con, const std::string &sql, int warmup, int reps) {
	for (int i = 0; i < warmup; i++) {
		Run(con, sql);
	}
	std::vector<double> samples;
	for (int i = 0; i < reps; i++) {
		auto start = Clock::now();
		Run(con, sql);
		samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
	}
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

std::string JsonEscape(const std::string &s) {
	std::string out;
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out;
}

double TimeOnceUs(duckdb::Connection &con, const std::string &sql) {
	auto start = Clock::now();
	Run(con, sql);
	return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

double Median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v.empty() ? 0 : (v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2);
}

//! Read-only sweep: every timed repetition is a different parametrization of
//! the query template (one manifest seed per rep).  Per query: one untimed
//! warm-up of every variant on the first seed, then for each seed: DuckDB and
//! accel_tpch_qN at 1 thread, and for each T in thread_list DuckDB at T threads
//! and accel_tpch_qN_parallel at T threads.  Long-format CSV on stdout.
int RunSweep(const std::vector<int> &queries, int seeds, const std::vector<int> &thread_list);

std::vector<int> ParseList(const std::string &s) {
	std::vector<int> out;
	std::stringstream stream(s);
	std::string item;
	while (std::getline(stream, item, ',')) {
		if (!item.empty()) {
			out.push_back(std::stoi(item));
		}
	}
	return out;
}

} // namespace

int main(int argc, char **argv) {
	std::vector<int> queries;
	for (int q = 1; q <= 22; q++) {
		queries.push_back(q);
	}
	int seeds = 5, reps = 5, warmup = 2, threads = 8;
	std::string mode = "both", dump_dir, emit_path;
	std::vector<int> thread_list;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string flag = argv[i], value = argv[i + 1];
		if (flag == "--queries") {
			queries = ParseList(value);
		} else if (flag == "--seeds") {
			seeds = std::stoi(value);
		} else if (flag == "--reps") {
			reps = std::stoi(value);
		} else if (flag == "--warmup") {
			warmup = std::stoi(value);
		} else if (flag == "--threads") {
			threads = std::stoi(value);
		} else if (flag == "--mode") {
			mode = value;
		} else if (flag == "--dump") {
			dump_dir = value;
		} else if (flag == "--threads-list") {
			thread_list = ParseList(value);
		} else if (flag == "--scale-factor") {
			g_scale_factor = std::stod(value);
		} else if (flag == "--emit-instances") {
			emit_path = value;
		} else {
			std::cerr << "unknown flag " << flag << "\n";
			return 2;
		}
	}
	if (!emit_path.empty()) {
		// One JSON object per line: query, label, args ("(...)" for accel_tpch_qN), reference SQL.
		std::ofstream out(emit_path);
		for (int q : queries) {
			for (const auto &inst : InstancesFor(q)) {
				out << "{\"query\": " << q << ", \"label\": \"" << JsonEscape(inst.label) << "\", \"args\": \""
				    << JsonEscape(inst.args) << "\", \"ref_sql\": \"" << JsonEscape(inst.ref_sql) << "\"}\n";
			}
		}
		return 0;
	}
	if (thread_list.empty()) {
		thread_list = {threads};
	}
	if (mode == "sweep") {
		return RunSweep(queries, seeds, thread_list);
	}
	const bool check = mode != "bench", bench = mode != "check";

	duckdb::DBConfig config;
	config.options.access_mode = duckdb::AccessMode::READ_ONLY;
	duckdb::DuckDB db(accelerator_codegen::test_support::TpchDbPath()
	                      .c_str(),
	                  &config);
	duckdb::Connection con(db);
	accelerator_codegen::RegisterAcceleratorFunctions(con);

	int failures = 0;
	std::map<int, std::vector<std::array<double, 4>>> times; // duck1, st, duckT, par
	std::cout << "query,seed,check,duck_1t_us,accel_st_us,st_speedup,duck_" << threads << "t_us,accel_par_us,par_speedup\n";
	for (int q : queries) {
		const auto fn = "accel_tpch_q" + std::to_string(q);
		auto instances = InstancesFor(q);
		for (int i = 0; i < seeds && i < static_cast<int>(instances.size()); i++) {
			const auto &inst = instances[i];
			const auto st_sql = "SELECT * FROM " + fn + inst.args;
			const auto par_sql = "SELECT * FROM " + fn + "_parallel" + inst.args;
			std::string status = "skip";
			try {
				if (check) {
					SetThreads(con, 1);
					auto ref = Run(con, inst.ref_sql);
					auto st = Run(con, st_sql);
					SetThreads(con, threads);
					auto par = Run(con, par_sql);
					auto par2 = Run(con, par_sql);
					std::string d;
					if (!(d = Diff(*ref, *st)).empty()) {
						status = "FAIL st-vs-ref: " + d;
					} else if (!(d = Diff(*ref, *par)).empty()) {
						status = "FAIL par-vs-ref: " + d;
					} else if (Render(*par) != Render(*par2)) {
						status = "FAIL par-nondeterministic";
					} else {
						status = "ok";
					}
					if (!dump_dir.empty()) {
						const auto base = dump_dir + "/q" + std::to_string(q) + "_" + inst.label;
						std::ofstream(base + "_st.txt") << Render(*st);
						std::ofstream(base + "_par.txt") << Render(*par);
					}
				}
			} catch (const std::exception &ex) {
				status = std::string("FAIL error: ") + ex.what();
			}
			if (status.rfind("FAIL", 0) == 0) {
				failures++;
			}
			std::array<double, 4> t {0, 0, 0, 0};
			if (bench && status.rfind("FAIL", 0) != 0) {
				SetThreads(con, 1);
				t[0] = MedianUs(con, inst.ref_sql, warmup, reps);
				t[1] = MedianUs(con, st_sql, warmup, reps);
				SetThreads(con, threads);
				t[2] = MedianUs(con, inst.ref_sql, warmup, reps);
				t[3] = MedianUs(con, par_sql, warmup, reps);
				times[q].push_back(t);
			}
			std::string clean = status;
			std::replace(clean.begin(), clean.end(), ',', ';');
			std::replace(clean.begin(), clean.end(), '\n', ' ');
			std::cout << "Q" << q << ',' << inst.label << ',' << clean << ',' << t[0] << ',' << t[1] << ','
			          << (t[1] > 0 ? t[0] / t[1] : 0) << ',' << t[2] << ',' << t[3] << ','
			          << (t[3] > 0 ? t[2] / t[3] : 0) << std::endl;
		}
	}
	if (bench) {
		std::cout << "# per-query medians across seeds (us): query,accel_st,accel_par,st_speedup_vs_duck1,"
		             "par_speedup_vs_duckT\n";
		for (auto &[q, rows] : times) {
			auto med = [&](int k) {
				std::vector<double> v;
				for (auto &r : rows) {
					v.push_back(r[k]);
				}
				std::sort(v.begin(), v.end());
				return v[v.size() / 2];
			};
			std::cout << "#Q" << q << ',' << med(1) << ',' << med(3) << ',' << med(0) / med(1) << ','
			          << med(2) / med(3) << '\n';
		}
	}
	std::cout << "# failures=" << failures << std::endl;
	return failures == 0 ? 0 : 1;
}

namespace {
int RunSweep(const std::vector<int> &queries, int seeds, const std::vector<int> &thread_list) {
	duckdb::DBConfig config;
	config.options.access_mode = duckdb::AccessMode::READ_ONLY;
	duckdb::DuckDB db(accelerator_codegen::test_support::TpchDbPath()
	                      .c_str(),
	                  &config);
	duckdb::Connection con(db);
	accelerator_codegen::RegisterAcceleratorFunctions(con);
	std::cout << "engine,query,seed,variant,threads,us\n";
	struct Key {
		std::string variant;
		int threads;
		bool operator<(const Key &o) const {
			return variant != o.variant ? variant < o.variant : threads < o.threads;
		}
	};
	std::map<int, std::map<Key, std::vector<double>>> all;
	for (int q : queries) {
		const auto fn = "accel_tpch_q" + std::to_string(q);
		auto instances = InstancesFor(q);
		const int n = std::min<int>(seeds, static_cast<int>(instances.size()));
		auto st_sql = [&](const Instance &i) { return "SELECT * FROM " + fn + i.args; };
		auto par_sql = [&](const Instance &i) { return "SELECT * FROM " + fn + "_parallel" + i.args; };
		// Warm-up (untimed): every variant once on the first instance.
		SetThreads(con, 1);
		Run(con, instances[0].ref_sql);
		Run(con, st_sql(instances[0]));
		for (int t : thread_list) {
			SetThreads(con, t);
			Run(con, instances[0].ref_sql);
			Run(con, par_sql(instances[0]));
		}
		auto record = [&](const Instance &inst, const char *variant, int threads, double us) {
			all[q][{variant, threads}].push_back(us);
			std::cout << "duckdb,Q" << q << ',' << inst.label << ',' << variant << ',' << threads << ',' << us
			          << std::endl;
		};
		for (int i = 0; i < n; i++) {
			const auto &inst = instances[i];
			SetThreads(con, 1);
			record(inst, "duckdb", 1, TimeOnceUs(con, inst.ref_sql));
			record(inst, "accel_st", 1, TimeOnceUs(con, st_sql(inst)));
			for (int t : thread_list) {
				SetThreads(con, t);
				record(inst, "duckdb", t, TimeOnceUs(con, inst.ref_sql));
				record(inst, "accel_par", t, TimeOnceUs(con, par_sql(inst)));
			}
		}
	}
	std::cout << "# medians over parametrizations (ms): query, duckdb@1, accel_st@1, st_speedup";
	for (int t : thread_list) {
		std::cout << ", duckdb@" << t << ", accel_par@" << t << ", par_speedup@" << t;
	}
	std::cout << "\n";
	for (auto &[q, m] : all) {
		const double d1 = Median(m[{"duckdb", 1}]) / 1e3, s1 = Median(m[{"accel_st", 1}]) / 1e3;
		std::cout << "#Q" << q << ',' << d1 << ',' << s1 << ',' << d1 / s1;
		for (int t : thread_list) {
			const double dt = Median(m[{"duckdb", t}]) / 1e3, pt = Median(m[{"accel_par", t}]) / 1e3;
			std::cout << ',' << dt << ',' << pt << ',' << dt / pt;
		}
		std::cout << "\n";
	}
	return 0;
}
} // namespace
