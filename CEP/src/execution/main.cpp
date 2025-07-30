#include <cstdint>
#include <iostream>
#include "execution/experiment.h"
#include "execution/getopt.h"

auto main(int argc, char *argv[]) -> int {
  // plan_choice: 0 for static shared; 1 for static non-shared; 2 for dynamic shared
  int plan_choice = std::stoi(argv[1]);
  uint64_t q0_bound = std::stoull(argv[2]);
  uint64_t q1_bound = std::stoull(argv[3]);
  int shed_method = std::stoi(argv[4]);
  uint64_t q0_window = std::stoull(argv[5]);
  uint64_t q1_window = std::stoull(argv[6]);
  int shedding_ratio = std::stoi(argv[7]);
  // int pattern_length = std::stoi(argv[8]);
  // shed_method, the load shedding strategy we choose:
  // 0 -> Cost-Model Shedding
  // 1 -> Random State Shedding
  // 2 -> Fractional Load Shedding
  // 3 -> DARLING
  // 4 -> ICDE'20
  // int c;
  // std::string opt_str = "p: "
  // while ((c = _free_getopt(argc, argv, "f:c:q:p:m:T:w:x:y:z:n:F:Z:L:C:tsIABb")) != -1) {
  // }
  // query choice: 0 -> SEQ(ABCDEFG); SEQ(ABCDXYZ)
  // 1 -> SEQ(AB~CD); SEQ(AB~CE)
  // 2 ->SEQ(AB+CD); SEQ(AB+EF)
  auto experiment =
      cep::Experiment(plan_choice, 0, q0_bound, q1_bound, shed_method, q0_window, q1_window, shedding_ratio);
  experiment.PrintResult();
}
