#include "accelerator_codegen/register.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "duckdb.hpp"

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database("/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb");
		{
			duckdb::Connection setup(database);
			accelerator_codegen::RegisterAcceleratorFunctions(setup);
			setup.Query("SELECT * FROM accel_q14_aux_build()");
		}

		std::atomic<bool> reader_done {false};
		std::atomic<bool> writer_done {false};
		std::string writer_error;

		std::thread reader_thread([&]() {
			duckdb::Connection connection(database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (int i = 0; i < 20; i++) {
				auto result = connection.Query("SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')");
				if (!result || result->HasError()) {
					throw std::runtime_error("reader query failed");
				}
			}
			reader_done.store(true);
		});

		std::thread writer_thread([&]() {
			duckdb::Connection connection(database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			auto result = connection.Query("UPDATE lineitem SET l_comment = l_comment WHERE l_orderkey = 1");
			if (!result || result->HasError()) {
				writer_error = result ? result->GetError() : "null result";
			}
			writer_done.store(true);
		});

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (reader_done.load() && writer_done.load()) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		if (!writer_done.load() || !reader_done.load()) {
			std::cerr << "concurrent write/read hung; reader_done=" << reader_done.load()
			          << " writer_done=" << writer_done.load() << std::endl;
			reader_thread.detach();
			writer_thread.detach();
			return EXIT_FAILURE;
		}

		reader_thread.join();
		writer_thread.join();
		Require(writer_error.empty(), "writer failed: " + writer_error);
		std::cout << "test_concurrent_write_read passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_concurrent_write_read failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
