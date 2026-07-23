//! Scale-proxy microbenchmarks for SF10 scalability audit.
//! Exercises state amplification, cache transitions, and merge stress without SF10 database scans.
//!
//! Modes (ACCEL_SCALE_PROXY):
//!   state_amplification  — dense array probe/clear at projected SF10 sizes
//!   cache_stress         — throughput vs state size (L2/LLC/exceed-LLC)
//!   merge_stress         — worker reduction at amplified state sizes
//!   materialization      — intermediate buffer append/concat/sort at amplified cardinalities

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

double Median(std::vector<double> samples) {
	if (samples.empty()) {
		return 0.0;
	}
	std::sort(samples.begin(), samples.end());
	const auto mid = samples.size() / 2;
	return samples.size() % 2 == 0 ? (samples[mid - 1] + samples[mid]) / 2.0 : samples[mid];
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	return value && *value ? std::stoi(value) : fallback;
}

std::string GetEnvString(const char *name, const std::string &fallback) {
	const char *value = std::getenv(name);
	return value && *value ? value : fallback;
}

// SF1 dense order domain ~6M slots; SF10 ~60M.
constexpr size_t kSf1OrderSlots = 6'000'001;
constexpr size_t kSf10OrderSlots = 60'000'001;
constexpr size_t kSf1CustomerSlots = 150'001;
constexpr size_t kSf10CustomerSlots = 1'500'001;
constexpr size_t kSf1PartSlots = 200'001;
constexpr size_t kSf10PartSlots = 2'000'001;

//! Proxy 2: state-amplification — dense uint8_t class store + random probes.
void RunStateAmplification(int iterations, size_t slot_count, size_t probe_count, size_t worker_count) {
	std::cerr << "scale_proxy state_amplification slot_count=" << slot_count << " probe_count=" << probe_count
	          << " worker_count=" << worker_count << " allocated_bytes=" << (slot_count * sizeof(uint8_t)) << '\n';

	std::vector<std::vector<uint8_t>> worker_state(worker_count);
	for (auto &ws : worker_state) {
		ws.assign(slot_count, 0);
	}

	std::mt19937_64 rng(42);
	std::uniform_int_distribution<size_t> key_dist(1, slot_count > 1 ? slot_count - 1 : 1);

	std::vector<double> init_us;
	std::vector<double> probe_us;
	std::vector<double> clear_us;
	std::vector<double> reduce_us;

	for (int iter = 0; iter < iterations; iter++) {
		// Init
		{
			const auto t0 = Clock::now();
			for (auto &ws : worker_state) {
				std::fill(ws.begin(), ws.end(), 0);
			}
			init_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}

		// Probe (random access pattern)
		{
			const auto t0 = Clock::now();
			for (size_t w = 0; w < worker_count; w++) {
				auto &ws = worker_state[w];
				for (size_t p = 0; p < probe_count / worker_count; p++) {
					ws[key_dist(rng)] = 1;
				}
			}
			probe_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}

		// Clear
		{
			const auto t0 = Clock::now();
			for (auto &ws : worker_state) {
				std::fill(ws.begin(), ws.end(), 0);
			}
			clear_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}

		// Reduce (OR across workers)
		{
			std::vector<uint8_t> merged(slot_count, 0);
			const auto t0 = Clock::now();
			for (size_t w = 0; w < worker_count; w++) {
				for (size_t i = 0; i < slot_count; i++) {
					merged[i] |= worker_state[w][i];
				}
			}
			reduce_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}
	}

	std::cerr << "scale_proxy_result mode=state_amplification slot_count=" << slot_count
	          << " worker_count=" << worker_count << " init_median_us=" << Median(init_us)
	          << " probe_median_us=" << Median(probe_us) << " clear_median_us=" << Median(clear_us)
	          << " reduce_median_us=" << Median(reduce_us)
	          << " probes_per_sec=" << (probe_count / (Median(probe_us) / 1e6)) << '\n';
}

//! Proxy 4: cache-capacity stress — sequential vs random probe throughput.
void RunCacheStress(int iterations, size_t slot_count, size_t probe_count) {
	std::cerr << "scale_proxy cache_stress slot_count=" << slot_count << " probe_count=" << probe_count
	          << " allocated_bytes=" << (slot_count * sizeof(uint64_t)) << '\n';

	std::vector<uint64_t> state(slot_count, 0);
	std::mt19937_64 rng(42);
	std::uniform_int_distribution<size_t> key_dist(0, slot_count > 0 ? slot_count - 1 : 0);

	std::vector<double> seq_us;
	std::vector<double> rand_us;

	for (int iter = 0; iter < iterations; iter++) {
		// Sequential probes
		{
			const auto t0 = Clock::now();
			uint64_t acc = 0;
			for (size_t i = 0; i < probe_count; i++) {
				const auto idx = i % slot_count;
				state[idx] += 1;
				acc += state[idx];
			}
			(void)acc;
			seq_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}

		// Random probes
		{
			const auto t0 = Clock::now();
			uint64_t acc = 0;
			for (size_t i = 0; i < probe_count; i++) {
				const auto idx = key_dist(rng);
				state[idx] += 1;
				acc += state[idx];
			}
			(void)acc;
			rand_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}
	}

	std::cerr << "scale_proxy_result mode=cache_stress slot_count=" << slot_count
	          << " seq_median_us=" << Median(seq_us) << " rand_median_us=" << Median(rand_us)
	          << " seq_probes_per_sec=" << (probe_count / (Median(seq_us) / 1e6))
	          << " rand_probes_per_sec=" << (probe_count / (Median(rand_us) / 1e6)) << '\n';
}

//! Proxy 5: merge-stress — sum int64 accumulators across workers at amplified sizes.
void RunMergeStress(int iterations, size_t entry_count, size_t worker_count) {
	std::cerr << "scale_proxy merge_stress entry_count=" << entry_count << " worker_count=" << worker_count << '\n';

	std::vector<std::vector<int64_t>> worker_data(worker_count);
	for (auto &wd : worker_data) {
		wd.resize(entry_count);
		for (size_t i = 0; i < entry_count; i++) {
			wd[i] = static_cast<int64_t>(i % 1000);
		}
	}

	std::vector<double> merge_us;
	for (int iter = 0; iter < iterations; iter++) {
		std::vector<int64_t> merged(entry_count, 0);
		const auto t0 = Clock::now();
		for (size_t w = 0; w < worker_count; w++) {
			for (size_t i = 0; i < entry_count; i++) {
				merged[i] += worker_data[w][i];
			}
		}
		merge_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
	}

	std::cerr << "scale_proxy_result mode=merge_stress entry_count=" << entry_count
	          << " worker_count=" << worker_count << " merge_median_us=" << Median(merge_us)
	          << " entries_per_sec=" << (entry_count / (Median(merge_us) / 1e6)) << '\n';
}

//! Proxy 3: intermediate-materialization amplification.
struct ProxyRecord {
	int64_t key = 0;
	int64_t value = 0;
	int64_t revenue = 0;
};

void RunMaterializationStress(int iterations, size_t record_count, size_t worker_count) {
	std::cerr << "scale_proxy materialization record_count=" << record_count << " worker_count=" << worker_count
	          << " bytes_per_record=" << sizeof(ProxyRecord) << '\n';

	std::vector<double> append_us;
	std::vector<double> concat_us;
	std::vector<double> sort_us;

	for (int iter = 0; iter < iterations; iter++) {
		std::vector<std::vector<ProxyRecord>> worker_bufs(worker_count);
		const auto per_worker = record_count / worker_count;

		// Append
		{
			const auto t0 = Clock::now();
			for (size_t w = 0; w < worker_count; w++) {
				auto &buf = worker_bufs[w];
				buf.reserve(per_worker);
				for (size_t i = 0; i < per_worker; i++) {
					buf.push_back({static_cast<int64_t>(w * per_worker + i), static_cast<int64_t>(i % 100),
					               static_cast<int64_t>(i * 17)});
				}
			}
			append_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}

		// Concatenate
		std::vector<ProxyRecord> merged;
		{
			const auto t0 = Clock::now();
			size_t total = 0;
			for (const auto &wb : worker_bufs) {
				total += wb.size();
			}
			merged.reserve(total);
			for (const auto &wb : worker_bufs) {
				merged.insert(merged.end(), wb.begin(), wb.end());
			}
			concat_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}

		// Sort
		{
			const auto t0 = Clock::now();
			std::sort(merged.begin(), merged.end(),
			          [](const ProxyRecord &a, const ProxyRecord &b) { return a.revenue > b.revenue; });
			sort_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
		}
	}

	std::cerr << "scale_proxy_result mode=materialization record_count=" << record_count
	          << " worker_count=" << worker_count << " append_median_us=" << Median(append_us)
	          << " concat_median_us=" << Median(concat_us) << " sort_median_us=" << Median(sort_us)
	          << " peak_bytes=" << (record_count * sizeof(ProxyRecord)) << '\n';
}

size_t ResolveSlotCount(const std::string &domain, int scale_multiplier) {
	if (domain == "order") {
		return scale_multiplier == 10 ? kSf10OrderSlots : kSf1OrderSlots;
	}
	if (domain == "customer") {
		return scale_multiplier == 10 ? kSf10CustomerSlots : kSf1CustomerSlots;
	}
	if (domain == "part") {
		return scale_multiplier == 10 ? kSf10PartSlots : kSf1PartSlots;
	}
	return kSf1OrderSlots * static_cast<size_t>(scale_multiplier);
}

} // namespace

int main() {
	const auto mode = GetEnvString("ACCEL_SCALE_PROXY", "all");
	const int iterations = GetEnvInt("ACCEL_SCALE_PROXY_ITERS", 5);
	const int worker_count = GetEnvInt("ACCEL_BENCH_THREADS", 8);
	const int scale_multiplier = GetEnvInt("ACCEL_SCALE_MULTIPLIER", 1);
	const std::string domain = GetEnvString("ACCEL_SCALE_DOMAIN", "order");

	const size_t slot_count = ResolveSlotCount(domain, scale_multiplier);
	const size_t probe_count = GetEnvInt("ACCEL_SCALE_PROBE_COUNT", 5'000'000);
	const size_t record_count = static_cast<size_t>(GetEnvInt("ACCEL_SCALE_RECORD_COUNT", 500'000)) *
	                            static_cast<size_t>(scale_multiplier);

	std::cerr << "scale_proxy_start mode=" << mode << " iterations=" << iterations << " worker_count=" << worker_count
	          << " scale_multiplier=" << scale_multiplier << " domain=" << domain << '\n';

	auto run_if = [&](const std::string &name, auto &&fn) {
		if (mode == "all" || mode == name) {
			fn();
		}
	};

	run_if("state_amplification", [&] { RunStateAmplification(iterations, slot_count, probe_count, worker_count); });
	run_if("cache_stress", [&] { RunCacheStress(iterations, slot_count, probe_count); });
	run_if("merge_stress", [&] { RunMergeStress(iterations, slot_count / 100, worker_count); });
	run_if("materialization", [&] { RunMaterializationStress(iterations, record_count, worker_count); });

	std::cerr << "scale_proxy_done\n";
	return 0;
}
