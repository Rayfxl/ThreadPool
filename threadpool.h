#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = INT32_MAX; // 最大任务数量
const int THREAD_MAX_THRESHHOLD = 1024; // 最大线程数量
const int THREAD_MAX_IDLE_TIME = 10; // 线程的最大空闲时间，单位：秒

// 线程池支持的模式
enum class PoolMode
{
	MODE_FIXED, // 固定数量的线程
	MODE_CACHED, // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
	// 线程函数对象类型
	using ThreadFunc = std::function<void(size_t)>;

	// 线程构造
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{}
	// 线程析构
	~Thread() = default;
	// 启动线程
	void start()
	{
		// 创建一个线程来执行一个线程函数
		std::thread t(func_, threadId_);
		t.detach(); // 设置分离线程
	}

	// 获取线程id
	size_t getId() const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	size_t threadId_; // 保存线程id
};

// 静态成员变量类外初始化
int Thread::generateId_ = 0;

// 线程池类型
class ThreadPool
{
public:
	// 线程池构造
	ThreadPool()
		: initThreadSize_(0)
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}

	// 线程池析构
	~ThreadPool()
	{
		isPoolRunning_ = false;

		// 等待线程池里面所有的线程返回 有两种状态：阻塞 & 正在执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// 必须先获取锁再通知线程，不然可能会死锁。
		// 如果将通知放在获取锁前面，对于那些任务执行完毕后要再次尝试获取锁的线程
		// 无论是否可以获取锁，都将阻塞在notEmpty条件变量上
		// 此时将无法通知它们删除线程对象
		notEmpty_.notify_all(); // 通知阻塞的线程执行线程函数，此时会执行删除线程对象的逻辑
		exitCond_.wait(lock, [&]()->bool {return curThreadSize_ == 0; });
	}

	// 设置线程池的工作模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	// 设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(size_t threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	// 设置线程池cached模式下线程数量上限阈值
	void setThreadSizeThreshHold(size_t threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}

	// 给线程池提交任务
	// 使用可变参数模板编程，让submitTask可以接收任意任务函数和任意数量的参数
	// 使用future<>获取和接收返回值
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// 打包任务，放入任务队列里面
		using RType = decltype(func(args...));
		// 使用完美转发得到传入的函数与参数，然后绑定为一个可调用对象，允许异步调用
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		// 获取返回值
		std::future<RType> result = task->get_future();

		// 获取任务队列锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// 等待任务队列有空余
		// 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			// 表示notFull_等待1s，条件依然没有满足，构造空任务并返回
			std::cerr << "task queue is full, submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType { return RType(); });
			(*task)();
			return task->get_future();
		}

		// 如果有空余，把任务对象放入任务队列中
		taskQue_.emplace([task]() {(*task)();});
		taskSize_++;

		// 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知，分配线程执行任务
		notEmpty_.notify_all();

		// cached模式 任务处理比较紧急 场景：小而快的任务 
		// 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << ">>> create new thread..." << std::endl;

			// 创建新的线程对象
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			size_t threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// 启动线程
			threads_[threadId]->start();
			// 修改线程个数相关的变量
			curThreadSize_++;
			idleThreadSize_++;
		}
		// 返回任务的Result对象
		return result;
	}

	// 开启线程池，默认值与硬件相关
	void start(size_t initThreadSize = std::thread::hardware_concurrency())
	{
		// 设置线程池的运行状态
		isPoolRunning_ = true;

		// 记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// 创建线程对象
		for (size_t i = 0; i < initThreadSize_; i++)
		{
			// 创建thread线程对象的时候，把线程函数绑定到thread线程对象
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			size_t threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));// unique_ptr只允许右值引用
		}

		// 启动所有线程
		for (size_t i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start(); // 需要去执行一个线程函数
			idleThreadSize_++; // 记录初始空闲线程的数量
		}
	}

	// 禁止用户使用拷贝构造与赋值
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// 定义线程函数
	void threadFunc(size_t threadid)
	{
		// 记录一次开始的时间
		auto lastTime = std::chrono::high_resolution_clock().now();

		// 所有任务必须执行完成，线程池才可以回收所有线程资源
		for (;;)
		{
			Task task;
			{
				// 先获取任务队列锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id()
					<< "尝试获取任务..." << std::endl;

				// cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程结束回收掉
				// 超过initThreadSize_数量的线程要进行回收
				// 当前时间 - 上一次线程执行的时间 > 60s
				while (taskQue_.size() == 0)
				{
					// 线程池要结束，回收线程资源
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						curThreadSize_--;
						std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
							<< std::endl;
						exitCond_.notify_all();
						return;// 线程函数结束，线程结束
					}

					// cached模式下
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						// 条件变量，超时返回了
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// 开始回收当前线程
								// 记录线程数量的相关变量的值修改
								// 把线程对象从线程列表容器中删除
								threads_.erase(threadid);
								curThreadSize_--;
								idleThreadSize_--;
								std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
									<< std::endl;
								return;
							}
						}
					}
					else
					{
						// 等待notEmpty条件
						notEmpty_.wait(lock);
					}				
				}

				// 任务队列不空，开始分配任务
				idleThreadSize_--;

				std::cout << "tid:" << std::this_thread::get_id()
					<< "获取任务成功..." << std::endl;

				// 从任务队列中取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// 如果依然有剩余任务，继续通知其它的线程执行任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				// 取出一个任务，进行通知，通知可以继续提交生产任务
				notFull_.notify_all();
			} // 出作用域释放锁

			// 当前线程负责执行这个任务
			if (task != nullptr)
			{
				task(); //执行function<void()>
			}
			// 执行完毕
			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); //更新线程执行完任务的时间
		}
	}

	// 检查pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<size_t, std::unique_ptr<Thread>> threads_; // 线程列表
	size_t initThreadSize_; // 初始的线程数量
	std::atomic_uint curThreadSize_; // 记录当前线程池里面线程的总数量
	size_t threadSizeThreshHold_; // 线程数量上限阈值
	std::atomic_uint idleThreadSize_; // 记录空闲线程的数量

	// Task任务	函数对象
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;	// 任务队列
	std::atomic_uint taskSize_; // 任务的数量
	size_t taskQueMaxThreshHold_; // 任务队列数量上限阈值

	std::mutex taskQueMtx_; // 保证任务队列的线程安全
	std::condition_variable notFull_; // 表示任务队列不满
	std::condition_variable notEmpty_; // 表示任务队列不空
	std::condition_variable exitCond_; // 等待线程资源全部回收

	PoolMode poolMode_; // 当前线程池的工作模式
	std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};

#endif


