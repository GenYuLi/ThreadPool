#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include <latch>
#include <stop_token>
#include <coroutine>

class ThreadPool {
public:
	ThreadPool(size_t);
	template <class F, class... Args>
	auto enqueue(F&& f, Args&&... args)
	  -> std::future<typename std::invoke_result_t<F, Args...>>;
	~ThreadPool();

private:
	// need to keep track of threads so we can join them
	std::vector<std::jthread> workers;
	// the task queue
	std::queue<std::function<void()>> tasks;

	// synchronization
	std::mutex queue_mutex;
	std::latch latch;
	bool stop;

	// coroutine task type
	struct CoroutineTask {
		struct promise_type {
			CoroutineTask get_return_object() {
				return CoroutineTask{
				  std::coroutine_handle<promise_type>::from_promise(*this)};
			}
			std::suspend_always initial_suspend() { return {}; }
			std::suspend_always final_suspend() noexcept { return {}; }
			void return_void() {}
			void unhandled_exception() { std::terminate(); }
		};

		std::coroutine_handle<promise_type> handle;

		CoroutineTask(std::coroutine_handle<promise_type> h) : handle(h) {}
		~CoroutineTask() {
			if (handle)
				handle.destroy();
		}
	};
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(size_t threads)
  : latch(threads), stop(false)
{
	for (size_t i = 0; i < threads; ++i)
		workers.emplace_back(
		  [this](std::stop_token stoken) {
			  for (;;) {
				  std::function<void()> task;

				  {
					  std::unique_lock<std::mutex> lock(this->queue_mutex);
					  this->latch.wait();
					  if (stoken.stop_requested() && this->tasks.empty())
						  return;
					  task = std::move(this->tasks.front());
					  this->tasks.pop();
				  }

				  task();
			  }
		  });
}

// add new work item to the pool
template <class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
  -> std::future<typename std::invoke_result_t<F, Args...>>
{
	using return_type = typename std::invoke_result_t<F, Args...>;

	auto task = std::make_shared<std::packaged_task<return_type()>>(
	  std::bind(std::forward<F>(f), std::forward<Args>(args)...));

	std::future<return_type> res = task->get_future();
	{
		std::unique_lock<std::mutex> lock(queue_mutex);

		// don't allow enqueueing after stopping the pool
		if (stop)
			throw std::runtime_error("enqueue on stopped ThreadPool");

		tasks.emplace([task]() { (*task)(); });
	}
	latch.count_down();
	return res;
}

// add new coroutine task to the pool
template <class F, class... Args>
auto ThreadPool::enqueue_coroutine(F&& f, Args&&... args)
  -> CoroutineTask
{
	auto task = [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() -> CoroutineTask {
		co_await std::suspend_always{};
		std::invoke(f, args...);
	};

	{
		std::unique_lock<std::mutex> lock(queue_mutex);

		// don't allow enqueueing after stopping the pool
		if (stop)
			throw std::runtime_error("enqueue on stopped ThreadPool");

		tasks.emplace([task]() { task(); });
	}
	latch.count_down();
	return task();
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool()
{
	{
		std::unique_lock<std::mutex> lock(queue_mutex);
		stop = true;
	}
	for (std::jthread& worker : workers)
		worker.request_stop();
}

#endif
