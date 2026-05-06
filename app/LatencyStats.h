// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>

/**
 * @brief Thread-safe sliding window latency statistics
 * 
 * Maintains a rolling window of latency samples and computes
 * statistical metrics: average, standard deviation, percentiles,
 * min/max values.
 * 
 * Thread safety: All methods are protected by mutex.
 */
class LatencyStats
{
public:
    explicit LatencyStats(size_t window_size = 1000)
        : max_samples_(window_size), sum_(0.0), sum_sq_(0.0)
    {
    }

    /**
     * @brief Add a new latency sample
     * @param latency_ms Latency in milliseconds
     */
    void add_sample(double latency_ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (samples_.size() >= max_samples_)
        {
            double old = samples_.front();
            samples_.pop_front();
            sum_ -= old;
            sum_sq_ -= old * old;
        }

        samples_.push_back(latency_ms);
        sum_ += latency_ms;
        sum_sq_ += latency_ms * latency_ms;
    }

    /**
     * @brief Get average latency in milliseconds
     */
    double get_average() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_.empty() ? 0.0 : sum_ / samples_.size();
    }

    /**
     * @brief Get standard deviation in milliseconds
     */
    double get_std_dev() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty())
            return 0.0;

        double avg = sum_ / samples_.size();
        double variance = (sum_sq_ / samples_.size()) - (avg * avg);
        return variance > 0 ? std::sqrt(variance) : 0.0;
    }

    /**
     * @brief Get percentile value
     * @param percentile Value between 0.0 and 1.0 (e.g., 0.99 for P99)
     * @return Latency value at the given percentile
     */
    double get_percentile(double percentile) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty())
            return 0.0;

        std::deque<double> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        size_t index = static_cast<size_t>(sorted.size() * percentile);
        if (index >= sorted.size())
            index = sorted.size() - 1;

        return sorted[index];
    }

    /**
     * @brief Get P99 latency (99th percentile)
     */
    double get_p99() const
    {
        return get_percentile(0.99);
    }

    /**
     * @brief Get P95 latency (95th percentile)
     */
    double get_p95() const
    {
        return get_percentile(0.95);
    }

    /**
     * @brief Get maximum latency
     */
    double get_max() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty())
            return 0.0;

        return *std::max_element(samples_.begin(), samples_.end());
    }

    /**
     * @brief Get minimum latency
     */
    double get_min() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty())
            return 0.0;

        return *std::min_element(samples_.begin(), samples_.end());
    }

    /**
     * @brief Get number of samples in the window
     */
    size_t get_sample_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_.size();
    }

    /**
     * @brief Clear all samples
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        sum_ = 0.0;
        sum_sq_ = 0.0;
    }

private:
    mutable std::mutex mutex_;
    std::deque<double> samples_;
    size_t max_samples_;
    double sum_;
    double sum_sq_;
};
