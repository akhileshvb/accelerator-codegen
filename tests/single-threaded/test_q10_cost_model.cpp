#include "accelerator_codegen/tpch_q10_maintenance.hpp"

#include <iostream>

int main() {
	try {
		accelerator_codegen::LoadQ10CostModelParams();
		const auto &params = accelerator_codegen::GetQ10CostModelParams();
		if (params.min_samples == 0) throw std::runtime_error("invalid params");
		std::cout << "test_q10_cost_model passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
