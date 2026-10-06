// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

#include <atomic>

namespace load_progress
{
    // Shared vocabulary for atomic access. Reads acquire, writes release, and
    // read-modify-write operations acquire/release unless explicitly overridden.
    // Storage stays std::atomic; these helpers do not synchronize multiple values.
    class Atomic final
    {
    public:
        /**
         * @brief Reads an atomic value with acquire ordering by default.
         */
        template <class T>
        [[nodiscard]] static T get(const std::atomic<T>& a_target,
            std::memory_order a_order = std::memory_order_acquire) noexcept
        {
            return a_target.load(a_order);
        }

        /**
         * @brief Writes an atomic value with release ordering by default.
         */
        template <class T>
        static void set(std::atomic<T>& a_target, typename std::atomic<T>::value_type a_value,
            std::memory_order a_order = std::memory_order_release) noexcept
        {
            a_target.store(a_value, a_order);
        }

        /**
         * @brief Writes a value atomically and returns the previous value.
         */
        template <class T>
        static T get_and_set(std::atomic<T>& a_target, typename std::atomic<T>::value_type a_value,
            std::memory_order a_order = std::memory_order_acq_rel) noexcept
        {
            return a_target.exchange(a_value, a_order);
        }

        /**
         * @brief Clears a boolean flag atomically and returns whether it was set.
         */
        static bool get_and_clear(std::atomic_bool& a_target,
            std::memory_order a_order = std::memory_order_acq_rel) noexcept
        {
            return a_target.exchange(false, a_order);
        }

        /**
         * @brief Adds an amount atomically and returns the value before addition.
         */
        template <class T>
        static T get_and_add(std::atomic<T>& a_target, typename std::atomic<T>::difference_type a_amount,
            std::memory_order a_order = std::memory_order_acq_rel) noexcept
        {
            return a_target.fetch_add(a_amount, a_order);
        }

        /**
         * @brief Subtracts an amount atomically and returns the value before subtraction.
         */
        template <class T>
        static T get_and_subtract(std::atomic<T>& a_target, typename std::atomic<T>::difference_type a_amount,
            std::memory_order a_order = std::memory_order_acq_rel) noexcept
        {
            return a_target.fetch_sub(a_amount, a_order);
        }

        /**
         * @brief Conditionally writes a value, allowing spurious failure and updating expected on failure.
         * @note A release order uses relaxed failure ordering; acq_rel uses acquire failure ordering.
         */
        template <class T>
        static bool compare_and_set_weak(std::atomic<T>& a_target, T& a_expected,
            typename std::atomic<T>::value_type a_value,
            std::memory_order a_order = std::memory_order_acq_rel) noexcept
        {
            return a_target.compare_exchange_weak(a_expected, a_value, a_order);
        }

        /**
         * @brief Conditionally writes a value with separate success and failure orders, allowing spurious failure.
         * @note On failure, a_expected receives the observed value.
         */
        template <class T>
        static bool compare_and_set_weak(std::atomic<T>& a_target, T& a_expected,
            typename std::atomic<T>::value_type a_value,
            std::memory_order a_success, std::memory_order a_failure) noexcept
        {
            return a_target.compare_exchange_weak(a_expected, a_value, a_success, a_failure);
        }

        /**
         * @brief Conditionally writes a value without spurious failure, updating expected on failure.
         * @note A release order uses relaxed failure ordering; acq_rel uses acquire failure ordering.
         */
        template <class T>
        static bool compare_and_set(std::atomic<T>& a_target, T& a_expected,
            typename std::atomic<T>::value_type a_value,
            std::memory_order a_order = std::memory_order_acq_rel) noexcept
        {
            return a_target.compare_exchange_strong(a_expected, a_value, a_order);
        }

        /**
         * @brief Conditionally writes a value with separate success and failure orders, without spurious failure.
         * @note On failure, a_expected receives the observed value.
         */
        template <class T>
        static bool compare_and_set(std::atomic<T>& a_target, T& a_expected,
            typename std::atomic<T>::value_type a_value,
            std::memory_order a_success, std::memory_order a_failure) noexcept
        {
            return a_target.compare_exchange_strong(a_expected, a_value, a_success, a_failure);
        }

    private:
        Atomic() = delete;
    };
}
