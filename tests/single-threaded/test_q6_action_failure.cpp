#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q6_maintenance.hpp"

#include <iostream>

int main() {
	try {
		accelerator_codegen::LoadQ6CostModelParams();
		auto candidates = accelerator_codegen::EnumerateQ6ActionCandidatesDetailed({}, {}, accelerator_codegen::GetQ6CostModelParams(), true);
		if (candidates.empty()) throw std::runtime_error("expected candidates");
		std::cout << "test_q6_action_failure passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
