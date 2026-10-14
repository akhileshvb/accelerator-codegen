#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "tpch/q14/duckdb/base/query.hpp"

#include "prims/simd.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

#if defined(__SIZEOF_INT128__)
using AccT = __int128_t;
#else
using AccT = int64_t;
#endif

// TPC-H SF1 partkeys are 1..200000.
constexpr size_t kDefaultPartCapacity = 200001;

enum class PromoClass : uint8_t {
	Absent = 0,
	NonPromo = 1,
	Promo = 2,
};

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsPromoType(const duckdb::string_t &value) {
	static constexpr const char kPromo[] = "PROMO";
	constexpr auto kPromoSize = sizeof(kPromo) - 1;
	return value.GetSize() >= kPromoSize && std::memcmp(value.GetData(), kPromo, kPromoSize) == 0;
}

AccT DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return static_cast<AccT>(extendedprice_raw) * static_cast<AccT>(100 - discount_raw);
}

duckdb::hugeint_t AccToHugeint(AccT value) {
#if defined(__SIZEOF_INT128__)
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
#else
	return duckdb::hugeint_t(value);
#endif
}

double DecimalSumToDouble(AccT raw_scale4) {
	auto decimal_value = duckdb::Value::DECIMAL(AccToHugeint(raw_scale4), 38, 4);
	return decimal_value.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
}

class PartPromoStore {
public:
	PartPromoStore() {
		classes.assign(kDefaultPartCapacity, static_cast<uint8_t>(PromoClass::Absent));
		capacity = classes.size();
	}

	void Store(int64_t partkey, PromoClass promo) {
		if (partkey < 0) {
			overflow[partkey] = promo;
			return;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key >= capacity) {
			if (partkey > 1'000'000'000) {
				overflow[partkey] = promo;
				return;
			}
			EnsureCapacity(key + 1);
		}
		classes[key] = static_cast<uint8_t>(promo);
	}

	//! Ensure the dense array covers keys [0, size) (called before concurrent dense stores).
	void Reserve(size_t size) {
		if (size > classes.size() && size <= 1'000'000'000) {
			classes.resize(size, static_cast<uint8_t>(PromoClass::Absent));
			capacity = classes.size();
		}
	}

	//! Concurrent-safe for distinct keys: writes the dense slot if in range, otherwise returns false.
	bool StoreDenseIfInRange(int64_t partkey, PromoClass promo) {
		if (partkey < 0 || static_cast<size_t>(partkey) >= capacity) {
			return false;
		}
		classes[static_cast<size_t>(partkey)] = static_cast<uint8_t>(promo);
		return true;
	}

	PromoClass Probe(int64_t partkey) const {
		if (partkey < 0) {
			const auto it = overflow.find(partkey);
			return it == overflow.end() ? PromoClass::Absent : it->second;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key < capacity) {
			return static_cast<PromoClass>(classes[key]);
		}
		const auto it = overflow.find(partkey);
		return it == overflow.end() ? PromoClass::Absent : it->second;
	}

	bool CanProbeUnchecked(int64_t partkey) const {
		return partkey >= 0 && static_cast<size_t>(partkey) < capacity;
	}

	PromoClass ProbeNonNegUnchecked(uint64_t partkey) const {
		return static_cast<PromoClass>(classes[partkey]);
	}

private:
	void EnsureCapacity(size_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		size_t target = new_capacity;
		if (capacity > 0) {
			const size_t doubled = capacity > (std::numeric_limits<size_t>::max() / 2)
			                           ? std::numeric_limits<size_t>::max()
			                           : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		classes.resize(target, static_cast<uint8_t>(PromoClass::Absent));
		capacity = classes.size();
	}

	size_t capacity = 0;
	std::vector<uint8_t> classes;
	std::unordered_map<int64_t, PromoClass> overflow;
};

struct Q14WorkerAgg {
	AccT promo_revenue = 0;
	AccT total_revenue = 0;
	bool saw_contribution = false;
};

struct PartWorker {
	std::vector<std::pair<int64_t, PromoClass>> overflow;
};

//! Classify one part chunk and write the dense class array directly (p_partkey is the primary key, so
//! workers write disjoint slots); keys outside the dense range are collected per worker and stored serially.
void ConsumePartChunk(duckdb::DataChunk &chunk, PartPromoStore &store, PartWorker &worker) {
	auto &partkey_vector = chunk.data[Q14PartChunkLayout::PARTKEY_INDEX];
	auto &type_vector = chunk.data[Q14PartChunkLayout::TYPE_INDEX];

	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat type_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	type_vector.ToUnifiedFormat(chunk.size(), type_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto types = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format);

	// p_type is dictionary-compressed: classify each distinct dictionary entry once per chunk.
	constexpr duckdb::idx_t kCache = STANDARD_VECTOR_SIZE;
	uint8_t class_cache[kCache];
	std::memset(class_cache, 0, sizeof(class_cache));

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto type_index = type_format.sel->get_index(row);
		if (!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) {
			continue;
		}
		PromoClass promo = PromoClass::NonPromo;
		if (type_format.validity.AllValid() || type_format.validity.RowIsValidUnsafe(type_index)) {
			if (type_index < kCache && class_cache[type_index] != 0) {
				promo = static_cast<PromoClass>(class_cache[type_index]);
			} else {
				promo = IsPromoType(types[type_index]) ? PromoClass::Promo : PromoClass::NonPromo;
				if (type_index < kCache) {
					class_cache[type_index] = static_cast<uint8_t>(promo);
				}
			}
		}
		const auto partkey = partkeys[partkey_index];
		if (!store.StoreDenseIfInRange(partkey, promo)) {
			worker.overflow.emplace_back(partkey, promo);
		}
	}
}

void BuildPartPromoStore(duckdb::ClientContext &context, const duckdb::FunctionData &part_bind,
                         PartPromoStore &store, duckdb::idx_t worker_count) {
	store.Reserve(static_cast<size_t>(GetNativeScanTableRowCount(context, part_bind)) + 1);
	std::vector<CacheLinePadded<PartWorker>> workers(worker_count);
	RunParallelChunkScan(context, part_bind, worker_count, [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
		ConsumePartChunk(chunk, store, workers[worker_id].value);
	});
	for (auto &worker : workers) {
		for (const auto &entry : worker.value.overflow) {
			store.Store(entry.first, entry.second);
		}
	}
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const Q14Parameters &params, const PartPromoStore &store,
                             Q14WorkerAgg &agg) {
	auto &partkey_vector = chunk.data[Q14LineitemChunkLayout::PARTKEY_INDEX];
	auto &price_vector = chunk.data[Q14LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q14LineitemChunkLayout::DISCOUNT_INDEX];
	auto &shipdate_vector = chunk.data[Q14LineitemChunkLayout::SHIPDATE_INDEX];

	// Fast path: flat, all-valid chunk.  The shipdate window is evaluated with a SIMD selection vector (the
	// scan does not push it down); only the survivors probe the part table.
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(price_vector) && IsFlatAllValid(discount_vector) &&
	    IsFlatAllValid(shipdate_vector) && chunk.size() <= STANDARD_VECTOR_SIZE &&
	    params.end_date.days != std::numeric_limits<int32_t>::min()) {
		const auto *partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		const auto *prices = duckdb::FlatVector::GetData<int64_t>(price_vector);
		const auto *discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		const auto *shipdates = reinterpret_cast<const int32_t *>(duckdb::FlatVector::GetData(shipdate_vector));
		uint32_t sel[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const size_t m =
		    simd::SelectBetweenI32(shipdates, chunk.size(), params.start_date.days, params.end_date.days - 1, sel);
		for (size_t k = 0; k < m; k++) {
			const uint32_t row = sel[k];
			const auto partkey = partkeys[row];
			PromoClass promo;
			if (store.CanProbeUnchecked(partkey)) {
				promo = store.ProbeNonNegUnchecked(static_cast<uint64_t>(partkey));
			} else {
				promo = store.Probe(partkey);
			}
			if (promo == PromoClass::Absent) {
				continue;
			}
			const auto revenue = DiscountedRevenueRaw(prices[row], discounts[row]);
			agg.total_revenue += revenue;
			agg.saw_contribution = true;
			if (promo == PromoClass::Promo) {
				agg.promo_revenue += revenue;
			}
		}
		return;
	}

	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat price_format;
	duckdb::UnifiedVectorFormat discount_format;
	duckdb::UnifiedVectorFormat shipdate_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	price_vector.ToUnifiedFormat(chunk.size(), price_format);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	shipdate_vector.ToUnifiedFormat(chunk.size(), shipdate_format);

	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto prices = duckdb::UnifiedVectorFormat::GetData<int64_t>(price_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
	auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto price_index = price_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		const auto shipdate_index = shipdate_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!price_format.validity.AllValid() && !price_format.validity.RowIsValidUnsafe(price_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index)) ||
		    (!shipdate_format.validity.AllValid() && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index))) {
			continue;
		}
		const auto shipdate = shipdates[shipdate_index];
		if (!(shipdate >= params.start_date && shipdate < params.end_date)) {
			continue;
		}

		const auto partkey = partkeys[partkey_index];
		PromoClass promo;
		if (store.CanProbeUnchecked(partkey)) {
			promo = store.ProbeNonNegUnchecked(static_cast<uint64_t>(partkey));
		} else {
			promo = store.Probe(partkey);
		}
		if (promo == PromoClass::Absent) {
			continue;
		}

		const auto revenue = DiscountedRevenueRaw(prices[price_index], discounts[discount_index]);
		agg.total_revenue += revenue;
		agg.saw_contribution = true;
		if (promo == PromoClass::Promo) {
			agg.promo_revenue += revenue;
		}
	}
}

class Q14ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q14ParallelKernel(Q14Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q14 parallel expects bound part and lineitem scans");
		}

		const auto worker_count = ParallelWorkerCount(context);
		PartPromoStore part_promo;
		BuildPartPromoStore(context, *bound_inputs[0], part_promo, worker_count);
		// Freeze: part_promo is not mutated after this point; workers only probe.

		std::vector<CacheLinePadded<Q14WorkerAgg>> workers(worker_count);

		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     AccumulateLineitemChunk(chunk, params, part_promo, workers[worker_id].value);
		                     });

		promo_revenue_raw = 0;
		total_revenue_raw = 0;
		saw_contribution = false;
		for (const auto &worker : workers) {
			promo_revenue_raw += worker.value.promo_revenue;
			total_revenue_raw += worker.value.total_revenue;
			saw_contribution = saw_contribution || worker.value.saw_contribution;
		}
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		if (!saw_contribution) {
			result_is_null = true;
		} else if (total_revenue_raw == 0) {
			if (promo_revenue_raw == 0) {
				result_value = std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0);
			} else {
				result_value = std::numeric_limits<double>::infinity();
			}
		} else {
			const AccT numerator_raw = static_cast<AccT>(10000) * promo_revenue_raw;
			const auto numerator_d = duckdb::Value::DECIMAL(AccToHugeint(numerator_raw), 38, 6)
			                             .DefaultCastAs(duckdb::LogicalType::DOUBLE)
			                             .GetValue<double>();
			const auto total_d = DecimalSumToDouble(total_revenue_raw);
			result_value = numerator_d / total_d;
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		if (result_is_null) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DOUBLE));
		} else {
			output.SetValue(0, 0, duckdb::Value::DOUBLE(result_value));
		}
		output.SetCardinality(1);
		emitted = true;
		return true;
	}

private:
	Q14Parameters params;
	AccT promo_revenue_raw = 0;
	AccT total_revenue_raw = 0;
	bool saw_contribution = false;
	bool result_is_null = false;
	double result_value = 0.0;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ14ParallelKernel(const Q14Parameters &params) {
	return duckdb::make_uniq<Q14ParallelKernel>(params);
}

} // namespace accelerator_codegen
