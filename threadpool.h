#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THREASHHOLD = 50000;
const int THREAD_MAX_THRESHHOLD = 16;
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒

// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,  // 固定数量线程
    MODE_CACHED, // 动态增长线程数量
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func)
        :func_(func)
        , threadId_(generateId_++)
    {
    }
    // 线程析构
    ~Thread() {
        if (thread_.joinable()) {
            thread_.join(); // 自动等待线程退出，不分离
        }
    }
    // 启动线程
    void start()
    {
        // 直接绑定线程对象
        thread_ = std::thread(func_, threadId_);
    }

    // 获取线程id
    int getId()const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    std::thread thread_;
    static int generateId_;
    int threadId_; // 保存线程id
};

int Thread::generateId_ = 0;

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool()
        :initThreadSize_(0)
        , taskSize_(0)
        , idleThreadSize_(0)
        , curThreadSize_(0)
        , taskQueMaxThreshHold_(TASK_MAX_THREASHHOLD) // 项目中除0和1尽量不出现其他数字，用有意义的变量代替
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
    {
    }
    // 线程池析构
    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(taskQueMtx_);
            isPoolRunning_ = false;
        }

        notEmpty_.notify_all();

        threads_.clear();
    }

    // 设置线程池工作模式
    void setMode(PoolMode mode)
    {
        // 记录初始线程个数
        if (checkRunningState())
            return;
        poolMode_ = mode;
    }

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshhold)
    {
        if (checkRunningState())
            return;
        taskQueMaxThreshHold_ = threshhold;
    }

    // 设置线程池cached模式下线程阈值
    void setThreadSizeThreashHold(int threshhold)
    {
        if (checkRunningState())
            return;
        if (poolMode_ == PoolMode::MODE_CACHED)
        {
            threadSizeThreshHold_ = threshhold;
        }
    }

    // 给线程池提交任务
    // 使用可变参模板编程，让submitTask()函数可以接收不同类型的任务
    // pool.submitTask(sum1,10,20)
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future < decltype(func(args...)) >
    {
        // 打包任务，放入任务队列
        using RType = decltype(func(args...));
        auto task = std::make_shared <std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
        {
            std::cerr << "task queue is full,submit task failed!" << std::endl;
            auto task = std::make_shared <std::packaged_task<RType()>>(
                []()->RType {return RType(); });
            (*task)();
            return task->get_future();
        }

        // 如果有空余，把任务放入任务队列中
        //taskQue_.emplace(sp);
        taskQue_.emplace([task]() {(*task)(); });
        taskSize_++;

        // 因为放了新任务，任务队列不空，在notEmpty_上进行通知
        notEmpty_.notify_all();

        // cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程
        if (poolMode_ == PoolMode::MODE_CACHED
            && taskSize_ > idleThreadSize_
            && curThreadSize_ < threadSizeThreshHold_)
        {
            //std::cout << ">>> create new thread" << std::endl;
            // 创建新的线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
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

    // 开启线程池
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // 设置线程池的运行状态
        isPoolRunning_ = true;

        // 记录初始线程个数
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // 创建线程对象
        for (int i = 0; i < initThreadSize_; i++)
        {
            // 创建thread线程对象的时候，把线程函数给到thread线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            //threads_.emplace_back(std::move(ptr)); //emplace_back() 向容器中添加元素，unique_ptr
        }

        // 启动所有线程 std::vector<Thread*> threads_;
        for (auto& kv : threads_) {
            kv.second->start(); // 需要执行一个线程函数
            idleThreadSize_++;
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 定义线程函数
    void threadFunc(int threadid)
    {
        auto lastTime = std::chrono::steady_clock().now();

        for (;;)
        {
            Task task;
            {
                // 先获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                //std::cout << "tid:" << std::this_thread::get_id()
                //    << "尝试获取任务..." << std::endl;

                // 每秒返回一次 怎么区分：超时返回？还是有任务待执行返回
                // 锁+双重判断
                while (isPoolRunning_ && taskQue_.size() == 0)
                {
                    if (poolMode_ == PoolMode::MODE_CACHED)
                    {
                        // 条件变量，超时返回
                        if (std::cv_status::timeout ==
                            notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::steady_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME
                                && curThreadSize_ > initThreadSize_)
                            {
                                // 开始回收当前线程
                                // 记录线程数量的相关变量的值修改
                                // 把线程对象从线程列表容器中删除

                                curThreadSize_--;
                                idleThreadSize_--;

                                //std::cout << "threadid:" << std::this_thread::get_id() << "exit!"
                                //   << std::endl;
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
                if (!isPoolRunning_)
                {
                    break;
                }

                idleThreadSize_--;

                //std::cout << "tid:" << std::this_thread::get_id()
                //    << "获取任务成功..." << std::endl;

                // 从任务队列中取一个任务
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;

                // 如果依然有剩余任务，继续通知其他线程执行任务
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

                // 取出任务，进行通知，通知可以继续提交生产任务
                notFull_.notify_all();
            }
            // 释放锁

            // 当前线程负责执行这个任务
            if (task != nullptr)
            {
                task();//执行function<void()>
            }

            idleThreadSize_++;
            lastTime = std::chrono::steady_clock().now(); // 更新线程执行完任务的时间
        }

        //std::cout << "threadid:" << std::this_thread::get_id() << "exit!"
        //    << std::endl;
        exitCond_.notify_all();
    }

    // 检查pool的运行状态
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表

    int initThreadSize_;    // 初始线程数量
    int threadSizeThreshHold_;// 线程数量上限阈值
    std::atomic_int curThreadSize_; // 记录当前线程池中线程的总数量
    std::atomic_int idleThreadSize_;// 记录空闲线程的数量

    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // 任务队列
    std::atomic_int  taskSize_; // 任务的数量
    int taskQueMaxThreshHold_; // 任务队列数量上限阈值

    std::mutex taskQueMtx_; // 保证任务队列的线程安全
    std::condition_variable notFull_; // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空
    std::condition_variable exitCond_;// 等待线程资源全部回收

    PoolMode poolMode_; // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_;// 表示当前线程池的启动状态
};

#endif
