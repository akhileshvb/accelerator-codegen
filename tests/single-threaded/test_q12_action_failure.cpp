#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q12_maintenance.hpp"

#include <iostream>

int main() {
	try {
		accelerator_codegen::LoadQ12CostModelParams();
		auto candidates = accelerator_codegen::EnumerateQ12ActionCandidatesDetailed({}, {}, accelerator_codegen::GetQ12CostModelParams(), true);
		if (candidates.empty()) throw std::runtime_error("expected candidates");
		std::cout << "test_q12_action_failure passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
