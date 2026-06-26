// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "lib/common/container/pool_object.hpp"
#include "lib/common/container/stable_vector.hpp"
#include "lib/common/defines.hpp"
#include "lib/common/demangle.hpp"
#include "lib/common/logging.hpp"
#include "lib/common/mpl.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace rocprofiler
{
namespace common
{
namespace container
{
template <typename Tp>
struct pool
{
    using size_type       = size_t;
    using pool_array_type = stable_vector<pool_object<Tp>, 32>;

    template <typename FuncT, typename... Args>
    explicit pool(std::piecewise_construct_t, size_type count, FuncT&& ctor, Args&&... args);

    ~pool()               = default;
    pool(const pool&)     = delete;
    pool(pool&&) noexcept = delete;
    pool& operator=(const pool&) = delete;
    pool& operator=(pool&&) noexcept = delete;

    // get an object from the pool. if all objects are in use, a new one will be created and added
    // to the pool
    pool_object<Tp>& acquire();
    void             release(size_type idx);

    template <typename FuncT, typename... Args>
    pool_object<Tp>& acquire(FuncT&& ctor, Args&&... args);

    template <typename FuncT = void (*)(pool_object<Tp>&)>
    void clear(FuncT&& func = [](pool_object<Tp>&) {});

    std::string get_usage_report() const;

private:
    size_type              m_count         = 256;
    std::function<void()>  m_function      = nullptr;
    mutable std::mutex     m_pool_mtx      = {};
    pool_array_type        m_pool          = {};
    mutable std::mutex     m_available_mtx = {};
    std::queue<size_type>  m_available     = {};
    std::atomic<size_type> m_released      = 0;
    std::atomic<size_type> m_reused        = 0;
    std::atomic<size_type> m_new_batch     = 0;
};

template <typename Tp>
template <typename FuncT, typename... Args>
pool<Tp>::pool(std::piecewise_construct_t, size_type count, FuncT&& ctor, Args&&... args)
: m_count{count}
, m_function{[this,
              _ctor       = std::forward<FuncT>(ctor),
              _args_tuple = std::make_tuple(std::forward<Args>(args)...)]() {
    for(size_type i = 0; i < m_count; ++i)
    {
        auto idx = m_pool.size();
        m_pool.emplace_back(idx, false, this);
        std::apply(
            [&](auto&&... unpacked_args) {
                _ctor(m_pool[idx].get(), std::forward<decltype(unpacked_args)>(unpacked_args)...);
            },
            _args_tuple);
        m_available.push(idx);
    }
}}
{
    m_function();
}

template <typename Tp>
pool_object<Tp>&
pool<Tp>::acquire()
{
    // [POOL-INSTR] threshold for logging slow operations (microseconds)
    constexpr int64_t _stall_us = 5000;
    using _clk                  = std::chrono::steady_clock;

    auto _idx = std::optional<size_type>{};
    {
        const auto _w0       = _clk::now();
        auto       _avail_lk = std::unique_lock<std::mutex>{m_available_mtx};
        const auto _wait =
            std::chrono::duration_cast<std::chrono::microseconds>(_clk::now() - _w0).count();
        ROCP_WARNING_IF(_wait > _stall_us)
            << "[POOL-INSTR] acquire waited " << _wait
            << "us for m_available_mtx (avail=" << m_available.size() << ")";
        if(!m_available.empty())
        {
            _idx = m_available.front();
            m_available.pop();
            if(m_released > 0)
            {
                m_reused++;
            }
        }
    }

    if(_idx.has_value())
    {
        const auto _w0      = _clk::now();
        auto       _read_lk = std::unique_lock<std::mutex>{m_pool_mtx};
        const auto _wait =
            std::chrono::duration_cast<std::chrono::microseconds>(_clk::now() - _w0).count();
        ROCP_WARNING_IF(_wait > _stall_us)
            << "[POOL-INSTR] acquire waited " << _wait << "us for m_pool_mtx (read path)";
        auto& _obj = m_pool.at(_idx.value());
        ROCP_FATAL_IF(!_obj.acquire()) << fmt::format(
            "Pool object at index {} was expected to be available but was not", _idx.value());
        return _obj;
    }

    // add a new batch
    {
        const auto _w0             = _clk::now();
        auto       _write_pool_lk  = std::unique_lock<std::mutex>{m_pool_mtx};
        auto       _write_avail_lk = std::unique_lock<std::mutex>{m_available_mtx};
        const auto _wait =
            std::chrono::duration_cast<std::chrono::microseconds>(_clk::now() - _w0).count();
        if(m_available.empty())
        {
            const auto _g0 = _clk::now();
            m_new_batch++;
            m_function();
            const auto _grow =
                std::chrono::duration_cast<std::chrono::microseconds>(_clk::now() - _g0).count();
            // [POOL-INSTR] growth is the prime suspect: log every grow with timing + counters
            ROCP_WARNING << "[POOL-INSTR] GROW #" << m_new_batch.load() << " of type "
                         << cxx_demangle(typeid(Tp).name()) << ": lock_wait=" << _wait
                         << "us create_batch(" << m_count << ")=" << _grow
                         << "us new_pool_size=" << m_pool.size()
                         << " released=" << m_released.load() << " reused=" << m_reused.load();
        }
        else
        {
            ROCP_WARNING_IF(_wait > _stall_us)
                << "[POOL-INSTR] acquire-grow path waited " << _wait << "us for pool mutexes";
        }
    }

    return acquire();
}

template <typename Tp>
void
pool<Tp>::release(size_type idx)
{
    if(idx < m_pool.size())
    {
        const auto _w0       = std::chrono::steady_clock::now();
        auto       _write_lk = std::unique_lock<std::mutex>{m_available_mtx};
        const auto _wait     = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - _w0)
                               .count();
        ROCP_WARNING_IF(_wait > 5000)
            << "[POOL-INSTR] release waited " << _wait << "us for m_available_mtx";
        ROCP_FATAL_IF(m_pool.at(idx).in_use())
            << fmt::format("Pool object at index {} was expected to be not in use", idx);
        m_available.push(idx);
        m_released++;
    }
}

// get an object from the pool. if all objects are in use, a new one will be created and added to
// the pool
template <typename Tp>
template <typename FuncT, typename... Args>
pool_object<Tp>&
pool<Tp>::acquire(FuncT&& ctor, Args&&... args)
{
    auto& _ref = acquire();
    ctor(_ref.get(), std::forward<Args>(args)...);
    return _ref;
}

template <typename Tp>
template <typename FuncT>
void
pool<Tp>::clear(FuncT&& func)
{
    auto _write_pool_lk = std::unique_lock<std::mutex>{m_pool_mtx};

    for(auto& itr : m_pool)
    {
        ROCP_WARNING_IF(itr.in_use()) << fmt::format(
            "Pool object at index {} is still in use during pool clear", itr.index());
        itr.release();
        // run cleanup lambda
        if constexpr(std::is_invocable_v<FuncT, pool_object<Tp>&>)
        {
            func(itr);
        }
        else if constexpr(std::is_invocable_v<FuncT, Tp&>)
        {
            func(itr.get());
        }
        else
        {
            static_assert(mpl::assert_false<FuncT>::value,
                          "Invalid function type for pool<Tp>::clear");
        }
    }

    auto _write_avail_lk = std::unique_lock<std::mutex>{m_available_mtx};

    while(!m_available.empty())
        m_available.pop();
    m_pool = pool_array_type{};
    m_released.store(0);
    m_reused.store(0);
    m_new_batch.store(0);
}

template <typename Tp>
std::string
pool<Tp>::get_usage_report() const
{
    auto _pool_lk  = std::unique_lock<std::mutex>{m_pool_mtx};
    auto _avail_lk = std::unique_lock<std::mutex>{m_available_mtx};
    return fmt::format("Usage report for pool (type='{}') :: size={}, available={}, reused={}, "
                       "released={}, batches={}",
                       cxx_demangle(typeid(Tp).name()),
                       m_pool.size(),
                       m_available.size(),
                       m_reused.load(),
                       m_released.load(),
                       m_new_batch.load());
}
}  // namespace container
}  // namespace common
}  // namespace rocprofiler
