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

const int TASK_MAX_THRESHHOLD = INT32_MAX; // �����������
const int THREAD_MAX_THRESHHOLD = 1024; // ����߳�����
const int THREAD_MAX_IDLE_TIME = 10; // �̵߳�������ʱ�䣬��λ����

// �̳߳�֧�ֵ�ģʽ
enum class PoolMode
{
	MODE_FIXED, // �̶��������߳�
	MODE_CACHED, // �߳������ɶ�̬����
};

// �߳�����
class Thread
{
public:
	// �̺߳�����������
	using ThreadFunc = std::function<void(size_t)>;

	// �̹߳���
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{}
	// �߳�����
	~Thread() = default;
	// �����߳�
	void start()
	{
		// ����һ���߳���ִ��һ���̺߳���
		std::thread t(func_, threadId_);
		t.detach(); // ���÷����߳�
	}

	// ��ȡ�߳�id
	size_t getId() const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	size_t threadId_; // �����߳�id
};

// ��̬��Ա���������ʼ��
int Thread::generateId_ = 0;

// �̳߳�����
class ThreadPool
{
public:
	// �̳߳ع���
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

	// �̳߳�����
	~ThreadPool()
	{
		isPoolRunning_ = false;

		// �ȴ��̳߳��������е��̷߳��� ������״̬������ & ����ִ��������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// �����Ȼ�ȡ����֪ͨ�̣߳���Ȼ���ܻ�������
		// �����֪ͨ���ڻ�ȡ��ǰ�棬������Щ����ִ����Ϻ�Ҫ�ٴγ��Ի�ȡ�����߳�
		// �����Ƿ���Ի�ȡ��������������notEmpty����������
		// ��ʱ���޷�֪ͨ����ɾ���̶߳���
		notEmpty_.notify_all(); // ֪ͨ�������߳�ִ���̺߳�������ʱ��ִ��ɾ���̶߳�����߼�
		exitCond_.wait(lock, [&]()->bool {return curThreadSize_ == 0; });
	}

	// �����̳߳صĹ���ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	// ����task�������������ֵ
	void setTaskQueMaxThreshHold(size_t threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	// �����̳߳�cachedģʽ���߳�����������ֵ
	void setThreadSizeThreshHold(size_t threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}

	// ���̳߳��ύ����
	// ʹ�ÿɱ����ģ���̣���submitTask���Խ������������������������Ĳ���
	// ʹ��future<>��ȡ�ͽ��շ���ֵ
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// ������񣬷��������������
		using RType = decltype(func(args...));
		// ʹ������ת���õ�����ĺ����������Ȼ���Ϊһ���ɵ��ö��������첽����
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		// ��ȡ����ֵ
		std::future<RType> result = task->get_future();

		// ��ȡ���������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// �ȴ���������п���
		// �û��ύ�����������������1s�������ж��ύ����ʧ�ܣ�����
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			// ��ʾnotFull_�ȴ�1s��������Ȼû�����㣬��������񲢷���
			std::cerr << "task queue is full, submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType { return RType(); });
			(*task)();
			return task->get_future();
		}

		// ����п��࣬���������������������
		taskQue_.emplace([task]() {(*task)();});
		taskSize_++;

		// ��Ϊ�·�������������п϶������ˣ���notEmpty_�Ͻ���֪ͨ�������߳�ִ������
		notEmpty_.notify_all();

		// cachedģʽ ������ȽϽ��� ������С��������� 
		// ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << ">>> create new thread..." << std::endl;

			// �����µ��̶߳���
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			size_t threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// �����߳�
			threads_[threadId]->start();
			// �޸��̸߳�����صı���
			curThreadSize_++;
			idleThreadSize_++;
		}
		// ���������Result����
		return result;
	}

	// �����̳߳أ�Ĭ��ֵ��Ӳ�����
	void start(size_t initThreadSize = std::thread::hardware_concurrency())
	{
		// �����̳߳ص�����״̬
		isPoolRunning_ = true;

		// ��¼��ʼ�̸߳���
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// �����̶߳���
		for (size_t i = 0; i < initThreadSize_; i++)
		{
			// ����thread�̶߳����ʱ�򣬰��̺߳����󶨵�thread�̶߳���
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			size_t threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));// unique_ptrֻ������ֵ����
		}

		// ���������߳�
		for (size_t i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start(); // ��Ҫȥִ��һ���̺߳���
			idleThreadSize_++; // ��¼��ʼ�����̵߳�����
		}
	}

	// ��ֹ�û�ʹ�ÿ��������븳ֵ
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// �����̺߳���
	void threadFunc(size_t threadid)
	{
		// ��¼һ�ο�ʼ��ʱ��
		auto lastTime = std::chrono::high_resolution_clock().now();

		// �����������ִ����ɣ��̳߳زſ��Ի��������߳���Դ
		for (;;)
		{
			Task task;
			{
				// �Ȼ�ȡ���������
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id()
					<< "���Ի�ȡ����..." << std::endl;

				// cachedģʽ�£��п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s��Ӧ�ðѶ�����߳̽������յ�
				// ����initThreadSize_�������߳�Ҫ���л���
				// ��ǰʱ�� - ��һ���߳�ִ�е�ʱ�� > 60s
				while (taskQue_.size() == 0)
				{
					// �̳߳�Ҫ�����������߳���Դ
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						curThreadSize_--;
						std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
							<< std::endl;
						exitCond_.notify_all();
						return;// �̺߳����������߳̽���
					}

					// cachedģʽ��
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						// ������������ʱ������
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// ��ʼ���յ�ǰ�߳�
								// ��¼�߳���������ر�����ֵ�޸�
								// ���̶߳�����߳��б�������ɾ��
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
						// �ȴ�notEmpty����
						notEmpty_.wait(lock);
					}				
				}

				// ������в��գ���ʼ��������
				idleThreadSize_--;

				std::cout << "tid:" << std::this_thread::get_id()
					<< "��ȡ����ɹ�..." << std::endl;

				// �����������ȡһ���������
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// �����Ȼ��ʣ�����񣬼���֪ͨ�������߳�ִ������
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				// ȡ��һ�����񣬽���֪ͨ��֪ͨ���Լ����ύ��������
				notFull_.notify_all();
			} // ���������ͷ���

			// ��ǰ�̸߳���ִ���������
			if (task != nullptr)
			{
				task(); //ִ��function<void()>
			}
			// ִ�����
			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); //�����߳�ִ���������ʱ��
		}
	}

	// ���pool������״̬
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<size_t, std::unique_ptr<Thread>> threads_; // �߳��б�
	size_t initThreadSize_; // ��ʼ���߳�����
	std::atomic_uint curThreadSize_; // ��¼��ǰ�̳߳������̵߳�������
	size_t threadSizeThreshHold_; // �߳�����������ֵ
	std::atomic_uint idleThreadSize_; // ��¼�����̵߳�����

	// Task����	��������
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;	// �������
	std::atomic_uint taskSize_; // ���������
	size_t taskQueMaxThreshHold_; // �����������������ֵ

	std::mutex taskQueMtx_; // ��֤������е��̰߳�ȫ
	std::condition_variable notFull_; // ��ʾ������в���
	std::condition_variable notEmpty_; // ��ʾ������в���
	std::condition_variable exitCond_; // �ȴ��߳���Դȫ������

	PoolMode poolMode_; // ��ǰ�̳߳صĹ���ģʽ
	std::atomic_bool isPoolRunning_; // ��ʾ��ǰ�̳߳ص�����״̬
};

#endif


