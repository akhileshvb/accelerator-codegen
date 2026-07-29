#include "calibrate_aux_cost_model_common.hpp"

#include <iostream>

int main(int argc, char **argv) {
	try {
		return accelerator_codegen::calibration::CalibrateAndWriteCostModel(1, argc, argv);
	} catch (const std::exception &ex) {
		std::cerr << "calibrate_q1_aux_cost_model failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
