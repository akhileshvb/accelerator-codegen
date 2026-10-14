#include "eval/bench/tpch/single-threaded/calibrate_aux_cost_model_common.hpp"

int main(int argc, char **argv) {
  return accelerator_codegen::calibration::CalibrateAndWriteCostModel(4, argc, argv);
}
