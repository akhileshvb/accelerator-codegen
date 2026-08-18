#include "calibrate_aux_cost_model_common.hpp"

int main(int argc, char **argv) {
  return accelerator_codegen::calibration::CalibrateAndWriteCostModel(21, argc, argv);
}