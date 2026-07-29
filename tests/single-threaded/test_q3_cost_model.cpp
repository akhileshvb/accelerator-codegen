#include "accelerator_codegen/tpch_q3_maintenance.hpp"

#include <iostream>

int main() {
	try {
		accelerator_codegen::LoadQ3CostModelParams();
		const auto &params = accelerator_codegen::GetQ3CostModelParams();
		if (params.min_samples == 0) throw std::runtime_error("invalid params");
		std::cout << "test_q3_cost_model passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
