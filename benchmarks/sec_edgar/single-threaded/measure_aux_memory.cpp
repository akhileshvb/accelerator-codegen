#include "accelerator_codegen/register.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

constexpr const char *kDefaultDb = "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";

struct QuerySpec {
	int query_num = 0;
	const char *reference_sql = nullptr;
	const char *build_sql = nullptr;
};

uint64_t CurrentRssBytes() {
	std::ifstream status("/proc/self/status");
	if (!status.is_open()) {
		return 0;
	}
	std::string line;
	while (std::getline(status, line)) {
		if (line.rfind("VmRSS:", 0) != 0) {
			continue;
		}
		uint64_t kb = 0;
		if (std::sscanf(line.c_str(), "VmRSS: %" SCNu64 " kB", &kb) == 1) {
			return kb * 1024ULL;
		}
	}
	return 0;
}

double ToMb(uint64_t bytes) {
	return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void ConsumeQuery(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result == nullptr || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "null query result");
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			(void)materialized.GetValue(col, row);
		}
	}
}

uint64_t MeasureQueryPeakAboveBaseline(duckdb::Connection &connection, const std::string &sql, int iterations,
                                       uint64_t baseline_rss) {
	uint64_t peak_rss = baseline_rss;
	for (int i = 0; i < iterations; i++) {
		ConsumeQuery(connection, sql);
		peak_rss = std::max(peak_rss, CurrentRssBytes());
	}
	return peak_rss > baseline_rss ? peak_rss - baseline_rss : 0;
}

const QuerySpec &GetQuerySpec(int query_num) {
	static const QuerySpec kSpecs[] = {
	    {1,
	     "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, AVG(line) AS avg_line_num "
	     "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC",
	     "SELECT * FROM accel_sec_edgar_q1_aux_build()"},
	    {2,
	     "SELECT s.name, n.tag, n.value "
	     "FROM num n "
	     "JOIN sub s ON n.adsh = s.adsh "
	     "JOIN ("
	     "    SELECT adsh, tag, MAX(value) AS max_value "
	     "    FROM num "
	     "    WHERE uom = 'pure' AND value IS NOT NULL "
	     "    GROUP BY adsh, tag"
	     ") m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
	     "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
	     "ORDER BY n.value DESC, s.name, n.tag "
	     "LIMIT 100",
	     "SELECT * FROM accel_sec_edgar_q2_aux_build()"},
	    {3,
	     "SELECT s.name, s.cik, SUM(n.value) AS total_value "
	     "FROM num n "
	     "JOIN sub s ON n.adsh = s.adsh "
	     "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
	     "GROUP BY s.name, s.cik "
	     "HAVING SUM(n.value) > ("
	     "    SELECT AVG(sub_total) FROM ("
	     "        SELECT SUM(n2.value) AS sub_total "
	     "        FROM num n2 "
	     "        JOIN sub s2 ON n2.adsh = s2.adsh "
	     "        WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL "
	     "        GROUP BY s2.cik"
	     "    ) avg_sub"
	     ") "
	     "ORDER BY total_value DESC "
	     "LIMIT 100",
	     "SELECT * FROM accel_sec_edgar_q3_aux_build()"},
	    {4,
	     "SELECT s.sic, t.tlabel, p.stmt, "
	     "COUNT(DISTINCT s.cik) AS num_companies, "
	     "SUM(n.value) AS total_value, "
	     "AVG(n.value) AS avg_value "
	     "FROM num n "
	     "JOIN sub s ON n.adsh = s.adsh "
	     "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
	     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
	     "WHERE n.uom = 'USD' AND p.stmt = 'EQ' "
	     "AND s.sic BETWEEN 4000 AND 4999 "
	     "AND n.value IS NOT NULL AND t.abstract = 0 "
	     "GROUP BY s.sic, t.tlabel, p.stmt "
	     "HAVING COUNT(DISTINCT s.cik) >= 2 "
	     "ORDER BY total_value DESC "
	     "LIMIT 500",
	     "SELECT * FROM accel_sec_edgar_q4_aux_build()"},
	    {6,
	     "SELECT s.name, p.stmt, n.tag, p.plabel, "
	     "SUM(n.value) AS total_value, COUNT(*) AS cnt "
	     "FROM num n "
	     "JOIN sub s ON n.adsh = s.adsh "
	     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
	     "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
	     "AND n.value IS NOT NULL "
	     "GROUP BY s.name, p.stmt, n.tag, p.plabel "
	     "ORDER BY total_value DESC, s.name, n.tag, p.plabel "
	     "LIMIT 200",
	     "SELECT * FROM accel_sec_edgar_q6_aux_build()"},
	    {24,
	     "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
	     "FROM num n "
	     "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh "
	     "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
	     "AND n.value IS NOT NULL AND p.adsh IS NULL "
	     "GROUP BY n.tag, n.version "
	     "HAVING COUNT(*) > 10 "
	     "ORDER BY cnt DESC "
	     "LIMIT 100",
	     "SELECT * FROM accel_sec_edgar_q24_aux_build()"},
	};
	for (const auto &spec : kSpecs) {
		if (spec.query_num == query_num) {
			return spec;
		}
	}
	throw std::runtime_error("unsupported query number");
}

std::string GetDbPath() {
	const char *env_path = std::getenv("ACCEL_SEC_EDGAR_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return kDefaultDb;
}

enum class Mode { Duckdb, Aux };

Mode ParseMode(const std::string &mode) {
	if (mode == "duckdb") {
		return Mode::Duckdb;
	}
	if (mode == "aux") {
		return Mode::Aux;
	}
	throw std::runtime_error("mode must be 'duckdb' or 'aux'");
}

} // namespace

int main(int argc, char **argv) {
	try {
		if (argc != 3) {
			std::cerr << "usage: measure_sec_edgar_aux_memory <duckdb|aux> <query_num>\n";
			return EXIT_FAILURE;
		}
		const auto mode = ParseMode(argv[1]);
		const int query_num = std::stoi(argv[2]);
		const auto &spec = GetQuerySpec(query_num);

		duckdb::DBConfig config;
		config.options.maximum_threads = 1;
		duckdb::DuckDB database(GetDbPath(), &config);
		duckdb::Connection connection(database);
		connection.Query("PRAGMA threads=1");

		const auto rss_baseline = CurrentRssBytes();
		uint64_t measured_bytes = 0;
		if (mode == Mode::Duckdb) {
			measured_bytes = MeasureQueryPeakAboveBaseline(connection, spec.reference_sql, 3, rss_baseline);
		} else {
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const auto rss_before_aux = CurrentRssBytes();
			ConsumeQuery(connection, spec.build_sql);
			const auto rss_after_aux = CurrentRssBytes();
			measured_bytes = rss_after_aux > rss_before_aux ? rss_after_aux - rss_before_aux : 0;
		}

		std::cout << "query,mode,baseline_rss_mb,measured_mb\n";
		std::cout << "Q" << query_num << ','
		          << (mode == Mode::Duckdb ? "duckdb" : "aux") << ','
		          << ToMb(rss_baseline) << ','
		          << ToMb(measured_bytes) << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "measure_sec_edgar_aux_memory failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
