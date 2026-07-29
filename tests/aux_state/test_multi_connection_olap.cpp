#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"

#include "test_support/q14_aux_helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

struct StartBarrier {
	explicit StartBarrier(int participants) : threshold(participants), arrived(0) {
	}

	void ArriveAndWait() {
		std::unique_lock<std::mutex> lock(mutex);
		if (++arrived == threshold) {
			cv.notify_all();
			return;
		}
		cv.wait(lock, [&] { return arrived >= threshold; });
	}

private:
	const int threshold;
	int arrived;
	std::mutex mutex;
	std::condition_variable cv;
};

void ConcurrentAuxReads() {
	duckdb::DuckDB database(":memory:");
	duckdb::Connection setup(database);
	accelerator_codegen::RegisterAcceleratorFunctions(setup);
	setup.Query("CREATE TABLE lineitem AS SELECT 1::BIGINT l_orderkey, 1::BIGINT l_partkey, 1::BIGINT l_suppkey, "
	            "1::INTEGER l_linenumber, 1.0::DECIMAL(15,2) l_quantity, 1.0::DECIMAL(15,2) l_extendedprice, "
	            "1.0::DECIMAL(15,2) l_discount, 1.0::DECIMAL(15,2) l_tax, 'O' l_returnflag, 'O' l_linestatus, "
	            "DATE '1995-06-01' l_shipdate, DATE '1995-06-01' l_commitdate, DATE '1995-06-01' l_receiptdate, "
	            "'DELIVER' l_shipinstruct, 'TRUCK' l_shipmode, 'x' l_comment");
	setup.Query("CREATE TABLE part AS SELECT 1::BIGINT p_partkey, 'n' p_name, 'm' p_mfgr, 'b' p_brand, "
	            "'t' p_type, 1 p_size, 'c' p_container, 1 p_retailprice, 'd' p_comment");
	setup.Query("SELECT * FROM accel_q14_aux_build()");

	constexpr int kReaders = 4;
	StartBarrier start_barrier(kReaders + 1);
	std::vector<std::thread> readers;
	std::atomic<int> successes {0};

	for (int i = 0; i < kReaders; i++) {
		readers.emplace_back([&]() {
			duckdb::Connection connection(database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			start_barrier.ArriveAndWait();
			auto result = connection.Query(accelerator_codegen::test_support::AcceleratorQ14AuxSql("1995-06-01"));
			if (result && !result->HasError()) {
				successes.fetch_add(1);
			}
		});
	}
	start_barrier.ArriveAndWait();
	for (auto &reader : readers) {
		reader.join();
	}
	Require(successes.load() == kReaders, "all concurrent readers should succeed");
}

} // namespace

int main() {
	try {
		ConcurrentAuxReads();
		std::cout << "test_multi_connection_olap passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_multi_connection_olap failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
