/*
  * throttled-file.cpp
  * 
  * Copyright (C) 2021 Alibaba Group.
  * 
  * This program is free software; you can redistribute it and/or
  * modify it under the terms of the GNU General Public License
  * as published by the Free Software Foundation; either version 2
  * of the License, or (at your option) any later version.
  * 
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  * 
  * See the file COPYING included with this distribution for more details.
*/

#include "throttled-file.h"
#include <inttypes.h>
#include "../ring.h"
#include "filesystem.h"
#include "forwardfs.h"
#include "../iovector.h"
#include "../utility.h"
#include "../alog.h"
using namespace std;

namespace FileSystem
{
    class StatisticsQueue
    {
    public:
        struct Sample
        {
            uint32_t time_stamp;
            uint32_t amount;
        };
        StatisticsQueue(uint32_t rate, uint32_t capacity) : m_events(capacity), m_rate(rate)
        {
            m_limit = (uint64_t)m_rate * m_time_window;
            m_timestamp_base = (photon::now / 1024UL) & ~((1UL<<29)-1);
        }
        const Sample& back() const
        {
            return m_events.back();
        }

        uint64_t try_pop()
        {
            auto now = photon::now / 1024UL;
            _update_timestamp_base(now);
            if (rate()) {
                auto w0 = now - m_time_window * 1024UL;
                uint64_t head_working_time = m_events.front().amount / rate() * 1024UL;
                while (!m_events.empty() && _get_time(m_events.front().time_stamp) < w0 && 
                    _get_time(m_events.front().time_stamp) + head_working_time <= now)
                {
                    m_sum -= m_events.front().amount;
                    m_events.pop_front();
                }
            }
            return now;
        }
        void push_back(uint32_t amount)
        {
            auto now = photon::now / 1024UL;
            if (rate()) {
                while (sum() >= limit()) {
                    uint64_t next_check = _get_time(m_events.front().time_stamp) + m_time_window * 1024UL;
                    if (next_check > now)
                        wait_for_pop((next_check - now) * 1024UL);
                    photon::thread_yield();
                    now = try_pop();
                }
                now = try_pop();
                if (m_events.empty() || _get_time(m_events.front().time_stamp) != now) {
                    while(m_events.full()) {
                        photon::thread_yield();
                        try_pop();
                    }
                    m_events.push_back(Sample{_get_stamp(now), amount});
                } else {
                    m_events.front().amount += amount;
                }
                m_sum += amount;
            }
        }
        uint64_t sum()
        {
            return m_sum;
        }
        uint64_t limit()
        {
            return m_limit;
        }
        uint32_t rate()
        {
            return m_rate;
        }
        uint64_t min_duration() // in us
        {
            if (rate())
                return sum() <= limit() ? 0 :   // use 1024 for 1000, for optimization
                    (sum() - limit()) * 1024 * 1024 / rate();
            else
                return 0;
        }
        void wait_for_push(uint64_t timeout = -1) {
            m_events.wait_for_push(timeout);
        }
        void wait_for_pop(uint64_t timeout = -1) {
            m_events.wait_for_pop(timeout);
        }

        RingQueue<Sample> m_events;
        uint32_t m_time_window = 1, m_rate;
        uint64_t m_sum = 0, m_limit;
        uint64_t m_timestamp_base = 0;

    protected:
        inline __attribute__((always_inline))
        void _update_timestamp_base(uint64_t now) {
            if (now > m_timestamp_base + ((1UL<<30) - 1)) {
                uint64_t new_base = now & ~((1UL<<29) - 1);
                for (size_t i = 0; i< m_events.size(); i++) {
                    m_events[i].time_stamp = m_events[i].time_stamp + m_timestamp_base - new_base;
                }
                m_timestamp_base = new_base;
            }
        }

        inline __attribute__((always_inline))
        uint64_t _get_time(uint32_t timestamp) {
            return m_timestamp_base + timestamp;
        }

        inline __attribute__((always_inline))
        uint32_t _get_stamp(uint64_t timems) {
            return timems - m_timestamp_base;
        }
    };
    
    struct scoped_queue
    {
        StatisticsQueue& _q;
        uint32_t _amount;
        uint64_t _ts_end;
        scoped_queue(StatisticsQueue& q, uint64_t count) :
            _q(q), _amount((uint32_t)(count))
        {
            q.push_back(_amount);
            _ts_end = photon::now + q.min_duration();
        }
        ~scoped_queue()
        {
            if (photon::now < _ts_end)
                photon::thread_usleep(_ts_end - photon::now);
            _q.try_pop();
        }
    };
    
    struct scoped_semaphore
    {
        uint64_t m_count;
        photon::semaphore& m_sem;
        scoped_semaphore(photon::semaphore& sem, uint64_t count) :
            m_count(count), m_sem(sem)
        {
            m_sem.wait(m_count);
        }
        ~scoped_semaphore()
        {
            m_sem.signal(m_count);
        }
    };

    struct split_iovector_view : public iovector_view
    {
        iovec* end;
        iovec f0, b0;
        uint64_t remaining, count, block_size;
        split_iovector_view(const iovec* iov, int iovcnt, uint64_t block_size_) :
            iovector_view((iovec*)iov, iovcnt)
        {
            end = (iovec*)iov + iovcnt;
            block_size = block_size_;
            count = sum();
            init(block_size);
        }
        void init(uint64_t block_size)
        {
            f0 = front();
            do_shrink(block_size);
        }
        void do_shrink(uint64_t block_size)
        {
            remaining = shrink_less_than(block_size);
            b0 = back();
            back().iov_len -= remaining;
        }
        void next()
        {
            if (iovcnt > 1)
            {
                front() = f0;
            }
            if (remaining == 0)
            {
                back() = b0;
                iov += iovcnt;
                iovcnt = (int)(end - iov);
                init(block_size);
                return;
            }

            iov = &back();
            (char*&)iov->iov_base += iov->iov_len;
            if (remaining < block_size)
            {
                iov->iov_len = remaining;
                iovcnt = (int)(end - iov);
                f0 = b0;
                do_shrink(block_size);
            }
            else // if (remaining >= block_size)
            {
                iov->iov_len = block_size;
                iovcnt = 1;
                remaining -= block_size;
            }
        }
    };
    
    template<typename Func, typename Adv>
    static inline __attribute__((always_inline))
    ssize_t split_io(ALogStringL name, size_t count, size_t block_size,
                     const Func& func, const Adv& adv)
    {
        if (block_size == 0 || count <= block_size)
            return func(count);
        
        ssize_t cnt = 0;
        while(count > 0)
        {
            auto len = count;
            if (len > block_size)
                len = block_size;
            ssize_t ret = func(len);
            assert(ret <= (ssize_t)block_size);
            if (ret < (ssize_t)len)
            {
                if (ret >= 0) {
                    ret += cnt;
                    LOG_ERRNO_RETURN(0, ret, "failed to m_file->`(), EoF?", name);
                } else {
                    LOG_ERRNO_RETURN(0, -1, "failed to m_file->`()", name);
                }
            }
            adv((size_t)ret);
            count -= ret;
            cnt += ret;
        }
        return cnt;
    }

    class ThrottledFile : public ForwardFile
    {
    public:
        struct Throttle
        {
            photon::semaphore num_io;
            StatisticsQueue iops;
            StatisticsQueue throughput;
            Throttle(const ThrottleLimits::UpperLimits& limits, uint32_t window) :
                num_io(limits.concurent_ops ? limits.concurent_ops : UINT32_MAX), // 0 for no limit, uint32_max to avoid overflow
                iops(limits.IOPS, window * 1024U),
                throughput(limits.throughput, window * 1024U)
            {
            }
        };
        
        struct scoped_throttle
        {
            uint64_t count;
            scoped_semaphore sem1, sem2;
            scoped_queue q11, q12, q21, q22;
            scoped_throttle(Throttle& t1, Throttle& t2, uint64_t cnt) :
                count(cnt),
                sem1(t1.num_io, 1),
                sem2(t2.num_io, 1),
                q11(t1.iops, 1),
                q12(t2.iops, 1),
                q21(t1.throughput, count),
                q22(t2.throughput, count)
            {
            }
            scoped_throttle(Throttle& t1, Throttle& t2, const iovec* iov, int iovcnt) :
                scoped_throttle(t1, t2, iovector_view((iovec*)iov, iovcnt).sum())
            {
            }
        };

        ThrottleLimits m_limits;
        Throttle t_all, t_read, t_write;
        ThrottledFile(IFile* file, const ThrottleLimits& limits) :
            ForwardFile(file), m_limits(limits),
            t_all(limits.RW, limits.time_window),
            t_read(limits.R, limits.time_window),
            t_write(limits.W, limits.time_window)
        {
        }

        virtual ssize_t pread(void *buf, size_t count, off_t offset) override
        {
            scoped_throttle t(t_all, t_read, count);
            return split_io(__func__, count, m_limits.R.block_size,
                [&](size_t len) { return m_file->pread(buf, len, offset); },
                [&](size_t len) { offset += len; (char*&)buf += len; });
        }
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, m_limits.R.block_size);
            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.R.block_size,
                [&](size_t len) { return m_file->preadv(v.iov, v.iovcnt, offset);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t preadv_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, m_limits.R.block_size);
            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.R.block_size,
                [&](size_t len) { return m_file->preadv(v.iov, v.iovcnt, offset);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t read(void *buf, size_t count) override
        {
            scoped_throttle t(t_all, t_read, count);
            return split_io(__func__, count, m_limits.R.block_size,
                [&](size_t len) { return m_file->read(buf, len);},
                [&](size_t len) { (char*&)buf += len; });
        }
        virtual ssize_t readv(const struct iovec *iov, int iovcnt) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, m_limits.R.block_size);
            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.R.block_size,
                [&](size_t len) { return m_file->readv(v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
        virtual ssize_t readv_mutable(struct iovec *iov, int iovcnt) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, m_limits.R.block_size);
            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.R.block_size,
                [&](size_t len) { return m_file->readv((iovec*)v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override
        {
            scoped_throttle t(t_all, t_write, count);
            return split_io(__func__, count, m_limits.W.block_size,
                [&](size_t len) { return m_file->pwrite(buf, len, offset); },
                [&](size_t len) { offset += len; (char*&)buf += len; });
        }
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, m_limits.W.block_size);
            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.W.block_size,
                [&](size_t len) { return m_file->pwritev(v.iov, v.iovcnt, offset);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, m_limits.W.block_size);
            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.W.block_size,
                [&](size_t len) { return m_file->pwritev(v.iov, v.iovcnt, offset);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t write(const void *buf, size_t count) override
        {
            scoped_throttle t(t_all, t_write, count);
            return split_io(__func__, count, m_limits.W.block_size,
                [&](size_t len) { return m_file->write(buf, len); },
                [&](size_t len) { (char*&)buf += len; });
        }
        virtual ssize_t writev(const struct iovec *iov, int iovcnt) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, m_limits.W.block_size);

            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.W.block_size,
                [&](size_t len) { return m_file->writev(v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
        virtual ssize_t writev_mutable(struct iovec *iov, int iovcnt) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, m_limits.W.block_size);
            scoped_throttle t(t_all, t_read, v.count);
            return split_io(__func__, v.count, m_limits.W.block_size,
                [&](size_t len) { return m_file->writev(v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
    };
    
    IFile* new_throttled_file(IFile* file, const ThrottleLimits& limits)
    {
        return new ThrottledFile(file, limits);
    }
}