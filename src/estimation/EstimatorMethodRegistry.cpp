#include "FlightEnvPlatformRuntime/estimation/EstimatorMethodRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace FlightEnvPlatformRuntime::estimation {
namespace {

constexpr double kMinVariance = 1.0e-9;

std::vector<double> resizedObservation(const RuntimeObservationFrame& frame, std::size_t state_dim) {
  std::vector<double> out(state_dim, 0.0);
  for (std::size_t i = 0; i < state_dim && i < frame.values.size(); ++i) {
    out[i] = std::isfinite(frame.values[i]) ? frame.values[i] : 0.0;
  }
  return out;
}

std::vector<std::string> resizedLabels(const RuntimeObservationFrame& frame, std::size_t state_dim) {
  std::vector<std::string> labels(state_dim);
  for (std::size_t i = 0; i < state_dim; ++i) {
    labels[i] = i < frame.value_labels.size() && !frame.value_labels[i].empty()
                    ? frame.value_labels[i]
                    : "state_" + std::to_string(i);
  }
  return labels;
}

double squaredDistance(const std::vector<double>& a, const std::vector<double>& b) {
  const std::size_t n = std::min(a.size(), b.size());
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

double norm2(const std::vector<double>& values) {
  double sum = 0.0;
  for (double value : values) {
    sum += value * value;
  }
  return std::sqrt(sum);
}

nlohmann::json vectorJson(const std::vector<double>& values) {
  nlohmann::json out = nlohmann::json::array();
  for (double value : values) {
    out.push_back(value);
  }
  return out;
}

std::vector<double> resizedDiagConfig(
    const nlohmann::json& method_config,
    const std::string& key,
    std::size_t state_dim,
    double fallback,
    double min_value) {
  std::vector<double> out(state_dim, fallback);
  const nlohmann::json values = method_config.value(key, nlohmann::json::array());
  if (!values.is_array()) {
    return out;
  }
  for (std::size_t i = 0; i < state_dim && i < values.size(); ++i) {
    if (!values.at(i).is_number()) {
      continue;
    }
    const double value = values.at(i).get<double>();
    out[i] = std::isfinite(value) ? std::max(min_value, value) : fallback;
  }
  return out;
}

double traceOf(const std::vector<double>& values) {
  return std::accumulate(values.begin(), values.end(), 0.0);
}

class ParticleFilterMethod final : public IEstimatorMethod {
 public:
  std::string kind() const override {
    return "particle_filter";
  }

  void configure(const nlohmann::json& method_config, std::size_t state_dim) override {
    state_dim_ = std::max<std::size_t>(1, state_dim);
    particle_count_ = std::max(1, method_config.value("particle_count", 128));
    ess_threshold_ratio_ = method_config.value("ess_threshold_ratio", 0.5);
    resampling_ = method_config.value("resampling", std::string("systematic"));
    particles_.clear();
    weights_.clear();
    resampling_count_ = 0;
  }

  EstimatorStepResult step(const EstimatorStepRequest& request) override {
    const std::vector<double> observation = resizedObservation(request.observation, state_dim_);
    ensureParticles(observation);
    for (std::size_t p = 0; p < particles_.size(); ++p) {
      for (std::size_t d = 0; d < state_dim_; ++d) {
        const double drift = 0.001 * static_cast<double>((static_cast<int>(p + d) % 7) - 3);
        const double previous = request.previous && d < request.previous->state_mean.size()
                                    ? request.previous->state_mean[d]
                                    : particles_[p][d];
        particles_[p][d] = 0.92 * particles_[p][d] + 0.08 * previous + drift;
      }
    }
    double weight_sum = 0.0;
    const double variance = std::max(1.0, squaredDistance(observation, std::vector<double>(state_dim_, 0.0)) /
                                             static_cast<double>(std::max<std::size_t>(1, state_dim_)) * 0.01);
    for (std::size_t p = 0; p < particles_.size(); ++p) {
      const double distance = squaredDistance(particles_[p], observation);
      weights_[p] = std::exp(-0.5 * distance / variance);
      weight_sum += weights_[p];
    }
    if (weight_sum <= 0.0 || !std::isfinite(weight_sum)) {
      const double uniform = 1.0 / static_cast<double>(particle_count_);
      std::fill(weights_.begin(), weights_.end(), uniform);
    } else {
      for (double& weight : weights_) {
        weight /= weight_sum;
      }
    }
    const double ess = effectiveSampleSize();
    bool resampled = false;
    if (resampling_ != "none" && ess < ess_threshold_ratio_ * static_cast<double>(particle_count_)) {
      systematicResample();
      resampled = true;
      ++resampling_count_;
    }
    std::vector<double> mean(state_dim_, 0.0);
    for (std::size_t p = 0; p < particles_.size(); ++p) {
      for (std::size_t d = 0; d < state_dim_; ++d) {
        mean[d] += weights_[p] * particles_[p][d];
      }
    }
    std::vector<double> covariance_diag(state_dim_, kMinVariance);
    for (std::size_t p = 0; p < particles_.size(); ++p) {
      for (std::size_t d = 0; d < state_dim_; ++d) {
        const double delta = particles_[p][d] - mean[d];
        covariance_diag[d] += weights_[p] * delta * delta;
      }
    }
    PosteriorFrame posterior;
    posterior.frame_index = request.observation.frame_index;
    posterior.sample_time_s = request.observation.sample_time_s;
    posterior.value_labels = resizedLabels(request.observation, state_dim_);
    posterior.state_mean = std::move(mean);
    posterior.covariance_diag = std::move(covariance_diag);
    posterior.checkpoint_id = "posterior.pf.frame_" + std::to_string(request.observation.frame_index);
    posterior.diagnostics = {
        {"particle_count", particle_count_},
        {"ess", ess},
        {"resampling_count", resampling_count_},
        {"resampled_this_frame", resampled},
        {"posterior_checkpoint", posterior.checkpoint_id},
    };
    return {posterior, posterior.diagnostics};
  }

 private:
  void ensureParticles(const std::vector<double>& center) {
    if (!particles_.empty()) {
      return;
    }
    particles_.assign(static_cast<std::size_t>(particle_count_), std::vector<double>(state_dim_, 0.0));
    weights_.assign(static_cast<std::size_t>(particle_count_), 1.0 / static_cast<double>(particle_count_));
    const double half = 0.5 * static_cast<double>(particle_count_ - 1);
    for (int p = 0; p < particle_count_; ++p) {
      const double offset_scale = (static_cast<double>(p) - half) / static_cast<double>(std::max(1, particle_count_));
      for (std::size_t d = 0; d < state_dim_; ++d) {
        particles_[static_cast<std::size_t>(p)][d] =
            center[d] + offset_scale * (1.0 + 0.01 * static_cast<double>(d + 1));
      }
    }
  }

  double effectiveSampleSize() const {
    double sum_sq = 0.0;
    for (double weight : weights_) {
      sum_sq += weight * weight;
    }
    return sum_sq > 0.0 ? 1.0 / sum_sq : 0.0;
  }

  void systematicResample() {
    std::vector<std::vector<double>> resampled;
    resampled.reserve(particles_.size());
    const double step = 1.0 / static_cast<double>(particle_count_);
    double cursor = 0.5 * step;
    double cumulative = weights_.empty() ? 0.0 : weights_[0];
    std::size_t source = 0;
    for (int i = 0; i < particle_count_; ++i, cursor += step) {
      while (cursor > cumulative && source + 1 < weights_.size()) {
        ++source;
        cumulative += weights_[source];
      }
      resampled.push_back(particles_[source]);
    }
    particles_ = std::move(resampled);
    std::fill(weights_.begin(), weights_.end(), 1.0 / static_cast<double>(particle_count_));
  }

  std::size_t state_dim_ = 1;
  int particle_count_ = 128;
  double ess_threshold_ratio_ = 0.5;
  std::string resampling_ = "systematic";
  int resampling_count_ = 0;
  std::vector<std::vector<double>> particles_;
  std::vector<double> weights_;
};

class BlackboxUnscentedKalmanMethod final : public IEstimatorMethod {
 public:
  std::string kind() const override {
    return "blackbox_unscented_kalman";
  }

  void configure(const nlohmann::json& method_config, std::size_t state_dim) override {
    state_dim_ = std::max<std::size_t>(1, state_dim);
    alpha_ = method_config.value("alpha", 1.0e-3);
    beta_ = method_config.value("beta", 2.0);
    kappa_ = method_config.value("kappa", 0.0);
    covariance_jitter_ = method_config.value("covariance_jitter", 1.0e-9);
    mean_.assign(state_dim_, 0.0);
    covariance_diag_.assign(state_dim_, 1.0);
    initialized_ = false;
    jitter_count_ = 0;
  }

  EstimatorStepResult step(const EstimatorStepRequest& request) override {
    const std::vector<double> observation = resizedObservation(request.observation, state_dim_);
    if (!initialized_) {
      mean_ = observation;
      covariance_diag_.assign(state_dim_, 1.0);
      initialized_ = true;
    }
    const std::vector<std::vector<double>> sigma_points = buildSigmaPoints();
    std::vector<double> predicted_mean(state_dim_, 0.0);
    for (const auto& point : sigma_points) {
      for (std::size_t d = 0; d < state_dim_; ++d) {
        predicted_mean[d] += point[d] / static_cast<double>(sigma_points.size());
      }
    }
    std::vector<double> innovation(state_dim_, 0.0);
    for (std::size_t d = 0; d < state_dim_; ++d) {
      innovation[d] = observation[d] - predicted_mean[d];
      mean_[d] = predicted_mean[d] + 0.7 * innovation[d];
      covariance_diag_[d] = 0.5 * covariance_diag_[d] + 0.1 * std::abs(innovation[d]) + covariance_jitter_;
      if (covariance_diag_[d] < covariance_jitter_) {
        covariance_diag_[d] = covariance_jitter_;
        ++jitter_count_;
      }
    }
    PosteriorFrame posterior;
    posterior.frame_index = request.observation.frame_index;
    posterior.sample_time_s = request.observation.sample_time_s;
    posterior.value_labels = resizedLabels(request.observation, state_dim_);
    posterior.state_mean = mean_;
    posterior.covariance_diag = covariance_diag_;
    posterior.checkpoint_id = "posterior.ukf.frame_" + std::to_string(request.observation.frame_index);
    const double trace = std::accumulate(covariance_diag_.begin(), covariance_diag_.end(), 0.0);
    posterior.diagnostics = {
        {"sigma_point_count", static_cast<int>(sigma_points.size())},
        {"innovation_norm", norm2(innovation)},
        {"posterior_cov_trace", trace},
        {"jitter_count", jitter_count_},
        {"posterior_checkpoint", posterior.checkpoint_id},
        {"covariance_diag_nonnegative", true},
        {"covariance_symmetric", true},
    };
    return {posterior, posterior.diagnostics};
  }

 private:
  std::vector<std::vector<double>> buildSigmaPoints() const {
    std::vector<std::vector<double>> points;
    points.reserve(2 * state_dim_ + 1);
    points.push_back(mean_);
    const double lambda = alpha_ * alpha_ * (static_cast<double>(state_dim_) + kappa_) -
                          static_cast<double>(state_dim_);
    const double scale = std::sqrt(std::max(kMinVariance, static_cast<double>(state_dim_) + lambda));
    for (std::size_t d = 0; d < state_dim_; ++d) {
      const double spread = scale * std::sqrt(std::max(kMinVariance, covariance_diag_[d]));
      std::vector<double> plus = mean_;
      std::vector<double> minus = mean_;
      plus[d] += spread;
      minus[d] -= spread;
      points.push_back(std::move(plus));
      points.push_back(std::move(minus));
    }
    return points;
  }

  std::size_t state_dim_ = 1;
  double alpha_ = 1.0e-3;
  double beta_ = 2.0;
  double kappa_ = 0.0;
  double covariance_jitter_ = 1.0e-9;
  bool initialized_ = false;
  int jitter_count_ = 0;
  std::vector<double> mean_;
  std::vector<double> covariance_diag_;
};

class ExtendedKalmanMethod final : public IEstimatorMethod {
 public:
  std::string kind() const override {
    return "extended_kalman";
  }

  void configure(const nlohmann::json& method_config, std::size_t state_dim) override {
    state_dim_ = std::max<std::size_t>(1, state_dim);
    linearization_ = method_config.value("linearization", std::string("finite_difference"));
    jacobian_provider_ = method_config.value("jacobian_provider", std::string("finite_difference"));
    innovation_gate_sigma_ = method_config.value("innovation_gate_sigma", 6.0);
    process_noise_diag_ = resizedDiagConfig(method_config, "process_noise_diag", state_dim_, 1.0e-3, 0.0);
    measurement_noise_diag_ = resizedDiagConfig(method_config, "measurement_noise_diag", state_dim_, 1.0, kMinVariance);
    mean_.assign(state_dim_, 0.0);
    covariance_diag_.assign(state_dim_, 1.0);
    initialized_ = false;
  }

  EstimatorStepResult step(const EstimatorStepRequest& request) override {
    const std::vector<double> observation = resizedObservation(request.observation, state_dim_);
    if (!initialized_) {
      mean_ = request.previous && request.previous->state_mean.size() >= state_dim_
                  ? request.previous->state_mean
                  : observation;
      covariance_diag_.assign(state_dim_, 1.0);
      initialized_ = true;
    }

    std::vector<double> innovation(state_dim_, 0.0);
    double gated_count = 0.0;
    for (std::size_t d = 0; d < state_dim_; ++d) {
      const double predicted = mean_[d];
      const double measurement_var = std::max(kMinVariance, measurement_noise_diag_[d]);
      const double predicted_var = std::max(kMinVariance, covariance_diag_[d] + process_noise_diag_[d]);
      const double gate = innovation_gate_sigma_ * std::sqrt(predicted_var + measurement_var);
      innovation[d] = observation[d] - predicted;
      if (std::abs(innovation[d]) > gate) {
        innovation[d] = innovation[d] < 0.0 ? -gate : gate;
        gated_count += 1.0;
      }
      const double gain = predicted_var / (predicted_var + measurement_var);
      mean_[d] = predicted + gain * innovation[d];
      covariance_diag_[d] = std::max(kMinVariance, (1.0 - gain) * predicted_var);
    }

    PosteriorFrame posterior;
    posterior.frame_index = request.observation.frame_index;
    posterior.sample_time_s = request.observation.sample_time_s;
    posterior.value_labels = resizedLabels(request.observation, state_dim_);
    posterior.state_mean = mean_;
    posterior.covariance_diag = covariance_diag_;
    posterior.checkpoint_id = "posterior.ekf.frame_" + std::to_string(request.observation.frame_index);
    posterior.diagnostics = {
        {"linearization", linearization_},
        {"jacobian_provider", jacobian_provider_},
        {"requires_jacobian", true},
        {"supports_blackbox_transition", jacobian_provider_ == "finite_difference"},
        {"innovation_norm", norm2(innovation)},
        {"posterior_cov_trace", traceOf(covariance_diag_)},
        {"gated_component_count", gated_count},
        {"posterior_checkpoint", posterior.checkpoint_id},
        {"covariance_diag_nonnegative", true},
        {"method_skeleton", true},
    };
    return {posterior, posterior.diagnostics};
  }

 private:
  std::size_t state_dim_ = 1;
  std::string linearization_ = "finite_difference";
  std::string jacobian_provider_ = "finite_difference";
  double innovation_gate_sigma_ = 6.0;
  bool initialized_ = false;
  std::vector<double> mean_;
  std::vector<double> covariance_diag_;
  std::vector<double> process_noise_diag_;
  std::vector<double> measurement_noise_diag_;
};

class EnsembleKalmanMethod final : public IEstimatorMethod {
 public:
  std::string kind() const override {
    return "ensemble_kalman";
  }

  void configure(const nlohmann::json& method_config, std::size_t state_dim) override {
    state_dim_ = std::max<std::size_t>(1, state_dim);
    ensemble_size_ = std::max(1, method_config.value("ensemble_size", 32));
    inflation_factor_ = std::max(kMinVariance, method_config.value("inflation_factor", 1.0));
    localization_ = method_config.value("localization", std::string("none"));
    initialized_ = false;
    ensemble_.clear();
  }

  EstimatorStepResult step(const EstimatorStepRequest& request) override {
    const std::vector<double> observation = resizedObservation(request.observation, state_dim_);
    ensureEnsemble(request, observation);

    for (int member = 0; member < ensemble_size_; ++member) {
      for (std::size_t d = 0; d < state_dim_; ++d) {
        const double centered_member = static_cast<double>(member) - 0.5 * static_cast<double>(ensemble_size_ - 1);
        const double perturbation = 1.0e-3 * centered_member / static_cast<double>(std::max(1, ensemble_size_));
        const double innovation = observation[d] - ensemble_[static_cast<std::size_t>(member)][d];
        ensemble_[static_cast<std::size_t>(member)][d] += 0.55 * innovation + inflation_factor_ * perturbation;
      }
    }

    std::vector<double> mean(state_dim_, 0.0);
    for (const auto& member : ensemble_) {
      for (std::size_t d = 0; d < state_dim_; ++d) {
        mean[d] += member[d] / static_cast<double>(ensemble_size_);
      }
    }
    std::vector<double> covariance_diag(state_dim_, kMinVariance);
    for (const auto& member : ensemble_) {
      for (std::size_t d = 0; d < state_dim_; ++d) {
        const double delta = member[d] - mean[d];
        covariance_diag[d] += delta * delta / static_cast<double>(std::max(1, ensemble_size_ - 1));
      }
    }

    ensemble_mean_ = mean;
    PosteriorFrame posterior;
    posterior.frame_index = request.observation.frame_index;
    posterior.sample_time_s = request.observation.sample_time_s;
    posterior.value_labels = resizedLabels(request.observation, state_dim_);
    posterior.state_mean = std::move(mean);
    posterior.covariance_diag = std::move(covariance_diag);
    posterior.checkpoint_id = "posterior.enkf.frame_" + std::to_string(request.observation.frame_index);
    posterior.diagnostics = {
        {"ensemble_size", ensemble_size_},
        {"inflation_factor", inflation_factor_},
        {"localization", localization_},
        {"ensemble_spread_trace", traceOf(posterior.covariance_diag)},
        {"posterior_cov_trace", traceOf(posterior.covariance_diag)},
        {"posterior_checkpoint", posterior.checkpoint_id},
        {"covariance_diag_nonnegative", true},
        {"method_skeleton", true},
    };
    return {posterior, posterior.diagnostics};
  }

 private:
  void ensureEnsemble(const EstimatorStepRequest& request, const std::vector<double>& center) {
    if (initialized_ && !ensemble_.empty()) {
      return;
    }
    initialized_ = true;
    const std::vector<double> base =
        request.previous && request.previous->state_mean.size() >= state_dim_
            ? request.previous->state_mean
            : center;
    ensemble_.assign(static_cast<std::size_t>(ensemble_size_), std::vector<double>(state_dim_, 0.0));
    const double half = 0.5 * static_cast<double>(ensemble_size_ - 1);
    for (int member = 0; member < ensemble_size_; ++member) {
      const double offset = (static_cast<double>(member) - half) / static_cast<double>(std::max(1, ensemble_size_));
      for (std::size_t d = 0; d < state_dim_; ++d) {
        ensemble_[static_cast<std::size_t>(member)][d] = base[d] + offset * (0.1 + 0.001 * static_cast<double>(d));
      }
    }
    ensemble_mean_ = base;
  }

  std::size_t state_dim_ = 1;
  int ensemble_size_ = 32;
  double inflation_factor_ = 1.0;
  std::string localization_ = "none";
  bool initialized_ = false;
  std::vector<double> ensemble_mean_;
  std::vector<std::vector<double>> ensemble_;
};

}  // namespace

std::unique_ptr<IEstimatorMethod> EstimatorMethodRegistry::create(const std::string& kind) {
  if (kind == "particle_filter") {
    return std::make_unique<ParticleFilterMethod>();
  }
  if (kind == "blackbox_unscented_kalman") {
    return std::make_unique<BlackboxUnscentedKalmanMethod>();
  }
  if (kind == "extended_kalman") {
    return std::make_unique<ExtendedKalmanMethod>();
  }
  if (kind == "ensemble_kalman") {
    return std::make_unique<EnsembleKalmanMethod>();
  }
  throw std::runtime_error("Unsupported estimator method kind: " + kind);
}

}  // namespace FlightEnvPlatformRuntime::estimation
