// distribution.h - support for generating random distributions

#pragma once

#include <random>

class Distribution {
 public:
  virtual ~Distribution() {}

  // Generate the next sample.
  virtual double operator()() = 0;
  virtual double Mean() const = 0;
};

class FixedDistribution : public Distribution {
 public:
  FixedDistribution(double val) : val_(val) {}
  ~FixedDistribution() {}

  double operator()() { return val_; }
  double Mean() const { return val_; }

 private:
  const double val_;
};

class BimodalDistribution : public Distribution {
 public:
  BimodalDistribution(int seed, double low, double high, double fraction)
      : frac_(fraction), low_(low), high_(high), rand_(seed), dist_(0.0, 1.0) {}

  double operator()() { return dist_(rand_) > frac_ ? high_ : low_; }
  double Mean() const { return high_ * (1 - frac_) + low_ * frac_; }

 private:
  const double frac_;
  const double low_;
  const double high_;
  std::mt19937 rand_;
  std::uniform_real_distribution<double> dist_;
};

class ExponentialDistribution : public Distribution {
 public:
  ExponentialDistribution(int seed, double mean)
      : mean_(mean), rand_(seed), dist_(1.0f / mean) {}
  ~ExponentialDistribution() {}

  double operator()() { return dist_(rand_); }
  double Mean() const { return mean_; }

 private:
  const double mean_;
  std::mt19937 rand_;
  std::exponential_distribution<double> dist_;
};

// Parses a string to generate one of the above distributions.
Distribution *DistributionFactory(std::string s);
