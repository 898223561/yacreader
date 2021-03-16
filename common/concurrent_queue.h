#ifndef CONCURRENT_QUEUE_H
#define CONCURRENT_QUEUE_H

#include <cassert>
#include <thread>
#include <limits>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <queue>
#include <vector>

namespace YACReader {
class ConcurrentQueue
{
public:
    explicit ConcurrentQueue(int threadCount)
        : jobsLeft(0),
          bailout(false)
    {
        threads = std::vector<std::thread>(threadCount);
        for (int index = 0; index < threadCount; ++index) {
            threads[index] = std::thread([this] {
                this->nextJob();
            });
        }
    }

    ~ConcurrentQueue()
    {
        joinAll();
    }

    void enqueue(std::function<void(void)> job)
    {
        {
            std::lock_guard<std::mutex> lock(jobsLeftMutex);
            ++jobsLeft;
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            _queue.emplace(job);
        }

        jobAvailableVar.notify_one();
    }

    //! @brief Cancels all jobs that have not been picked up by worker threads yet.
    //! @return The number of jobs that were canceled.
    std::size_t cancelPending()
    {
        decltype(_queue) oldQueue;
        {
            const std::lock_guard<std::mutex> lock(queueMutex);
            // The mutex locking time is lower with swap() compared to assigning a
            // temporary (which destroys _queue's elements and deallocates memory).
            _queue.swap(oldQueue);
        }

        const auto size = oldQueue.size();
        assert(size <= std::numeric_limits<int>::max());
        if (size != 0)
            finalizeJobs(static_cast<int>(size));
        return size;
    }

    void waitAll()
    {
        std::unique_lock<std::mutex> lock(jobsLeftMutex);
        if (jobsLeft > 0) {
            _waitVar.wait(lock, [this] {
                return jobsLeft == 0;
            });
        }
    }

private:
    std::vector<std::thread> threads;
    std::queue<std::function<void(void)>> _queue;
    int jobsLeft; //!< @invariant jobsLeft >= _queue.size()
    bool bailout;
    std::condition_variable jobAvailableVar;
    std::condition_variable _waitVar;
    std::mutex jobsLeftMutex;
    std::mutex queueMutex;

    void nextJob()
    {
        while (true) {
            std::function<void(void)> job;

            {
                std::unique_lock<std::mutex> lock(queueMutex);

                if (bailout) {
                    return;
                }

                jobAvailableVar.wait(lock, [this] {
                    return _queue.size() > 0 || bailout;
                });

                if (bailout) {
                    return;
                }

                job = _queue.front();
                _queue.pop();
            }

            job();
            finalizeJobs(1);
        }
    }

    void finalizeJobs(int count)
    {
        assert(count > 0);

        int remainingJobs;
        {
            std::lock_guard<std::mutex> lock(jobsLeftMutex);
            assert(jobsLeft >= count);
            jobsLeft -= count;
            remainingJobs = jobsLeft;
        }

        assert(remainingJobs >= 0);
        if (remainingJobs == 0)
            _waitVar.notify_all();
    }

    void joinAll()
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);

            if (bailout) {
                return;
            }

            bailout = true;
        }

        jobAvailableVar.notify_all();

        for (auto &x : threads) {
            if (x.joinable()) {
                x.join();
            }
        }
    }
};

}

#endif // CONCURRENT_QUEUE_H
