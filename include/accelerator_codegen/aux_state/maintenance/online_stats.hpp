#pragma once

#include <cstdint>
#include <limits>

namespace accelerator_codegen::aux_state::maintenance {

struct EwmaStat {
	double value = 0.0;
	uint64_t sample_count = 0;
	double alpha = 0.2;

	void Observe(double sample);
	bool HasConfidence(uint64_t min_samples) const;
	double BoundedValue(double min_value, double max_value) const;
};

struct RateEwma {
	double events_per_second = 0.0;
	uint64_t sample_count = 0;
	double alpha = 0.2;

	void ObserveIntervalUs(uint64_t interval_us);
};

} // namespace accelerator_codegen::aux_state::maintenance
