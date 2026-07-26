#include "accelerator_codegen/aux_state/maintenance/online_stats.hpp"

#include <algorithm>

namespace accelerator_codegen::aux_state::maintenance {

void EwmaStat::Observe(double sample) {
	if (sample_count == 0) {
		value = sample;
	} else {
		value = alpha * sample + (1.0 - alpha) * value;
	}
	sample_count++;
}

bool EwmaStat::HasConfidence(uint64_t min_samples) const {
	return sample_count >= min_samples;
}

double EwmaStat::BoundedValue(double min_value, double max_value) const {
	return std::max(min_value, std::min(max_value, value));
}

void RateEwma::ObserveIntervalUs(uint64_t interval_us) {
	if (interval_us == 0) {
		return;
	}
	const double rate = 1000000.0 / static_cast<double>(interval_us);
	if (sample_count == 0) {
		events_per_second = rate;
	} else {
		events_per_second = alpha * rate + (1.0 - alpha) * events_per_second;
	}
	sample_count++;
}

} // namespace accelerator_codegen::aux_state::maintenance
