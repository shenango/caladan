// distribution.cc - support for generating random distributions

#include "distribution.h"
#include "util.h"

#include <string>

Distribution *DistributionFactory(std::string s) {
  std::vector<std::string> tokens = split(s, ':');

  // the first token is the type of worker, must be specified
  auto cnt = tokens.size();
  if (cnt < 1) return nullptr;

  if (tokens[0] == "fixed" && cnt == 2) {
    double val = std::stod(tokens[1], nullptr);
    return new FixedDistribution(val);
  } else if (tokens[0] == "exponential" && cnt == 2) {
    double val = std::stod(tokens[1], nullptr);
    return new ExponentialDistribution(rand(), val);
  } else if (tokens[0] == "bimodal" && cnt == 4) {
    double low = std::stod(tokens[1], nullptr);
    double high = std::stod(tokens[2], nullptr);
    double frac = std::stod(tokens[3], nullptr);
    return new BimodalDistribution(rand(), low, high, frac);
  }

  // invalid type of worker
  return nullptr;
}
