#pragma once
#include <string>
#include <mysql/mysql.h>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

class Mysql_Connection // MySQL 连接类
{
public:
    MYSQL* conn_;
    std::chrono::steady_clock::time_point last_borrow_time_; // 最后一次借用时间
    std::chrono::steady_clock::time_point last_return_time_; // 最后一次归还时间

    Mysql_Connection(const std::string& host, int port, const std::string& user,
        const std::string& db_name, const std::string& password)
        : conn_(nullptr)
    {
        conn_ = mysql_init(nullptr); // 初始化
        if (!conn_)
        {
            throw std::runtime_error("mysql_init() failed");
        }

        // 设置连接选项
        mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4"); // 设置字符集
        int reconnect = 1;
        mysql_options(conn_, MYSQL_OPT_RECONNECT, &reconnect); // 启用自动重连

        if (!mysql_real_connect(conn_, host.c_str(), user.c_str(),
            password.c_str(), db_name.c_str(),
            port, nullptr, CLIENT_MULTI_STATEMENTS))
        {
            std::string err = mysql_error(conn_);
            mysql_close(conn_);
            throw std::runtime_error("mysql_real_connect() failed: " + err);
        }

        // 设置连接属性
        mysql_autocommit(conn_, 1); // 默认开启自动提交
        last_borrow_time_ = std::chrono::steady_clock::now();// 初始化借用时间
        last_return_time_ = std::chrono::steady_clock::now(); // 初始化归还时间
    }

    ~Mysql_Connection()
    {
        if (conn_)
        {
            mysql_close(conn_); // 关闭连接
        }
    }

    // 检查连接是否有效
    bool is_valid() const
    {
        if (!conn_) return false;
        return mysql_ping(conn_) == 0;
    }

    // 重置连接状态
    void reset()
    {
        if (conn_)
        {
            mysql_reset_connection(conn_);// 重置连接
        }
    }
};

class Mysql_Pool // MySQL 连接池类
{
public:
    struct PoolStats
    {
        size_t total_connections;   // 总连接数
        size_t idle_connections;    // 空闲连接数
        size_t active_connections;  // 活跃连接数
        size_t waiting_threads;     // 等待线程数
        size_t created_connections; // 已创建连接数
        size_t destroyed_connections; // 已销毁连接数
    };

private:
    // 配置参数
    std::string host_;           // 数据库主机地址
    std::string user_;           // 数据库用户名
    std::string db_name_;        // 数据库名称
    std::string password_;       // 数据库密码
    int port_;                   // 数据库端口号
    size_t min_pool_size_;       // 最小连接池大小
    size_t max_pool_size_;       // 最大连接池大小
    std::chrono::seconds connection_timeout_; // 连接获取超时时间
    std::chrono::seconds max_idle_time_;      // 最大空闲时间

    // 连接池状态
    std::queue<std::shared_ptr<Mysql_Connection>> pool_; // 空闲连接池
    std::condition_variable cv_;              // 条件变量
    mutable std::mutex mutex_;                // 互斥锁

    // 统计信息
    std::atomic<size_t> total_connections_{ 0 }; // 当前总连接数
    std::atomic<size_t> created_connections_{ 0 }; // 已创建连接数
    std::atomic<size_t> destroyed_connections_{ 0 }; // 已销毁连接数
    std::atomic<size_t> waiting_threads_{ 0 }; // 等待线程数

    // 清理线程控制
    std::atomic<bool> cleanup_thread_running_{ false }; // 清理线程运行标志
    std::thread cleanup_thread_; // 清理线程

    // 创建新连接
    std::shared_ptr<Mysql_Connection> create_connection()
    {
        try
        {
            auto conn = std::make_shared<Mysql_Connection>(
                host_, port_, user_, db_name_, password_);
            created_connections_++; // 统计创建数
            total_connections_++; // 增加总连接数
            return conn;
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("Failed to create connection: " + std::string(e.what()));
        }
    }

    // 清理无效连接
    void cleanup_idle_connections()
    {
        std::lock_guard<std::mutex> lock(mutex_); // 锁定连接池
        std::queue<std::shared_ptr<Mysql_Connection>> valid_connections; // 有效连接队列
        size_t removed_count = 0; // 移除计数
        while (!pool_.empty())
        {
            auto conn = pool_.front();
            pool_.pop();
            bool should_remove = false; // 是否应移除连接
            // 检查连接是否有效
            if (!conn->is_valid())
            {
                should_remove = true;
            }
            // 检查是否空闲超时
            else if (max_idle_time_.count() > 0)
            {
                auto idle_time = std::chrono::steady_clock::now() - conn->last_return_time_;
                if (idle_time > max_idle_time_)
                {
                    should_remove = true;
                }
            }

            if (should_remove)
            {
                destroyed_connections_++; // 统计销毁数
                total_connections_--; // 减少总连接数
                removed_count++; // 增加移除计数
            }
            else
            {
                valid_connections.push(conn); // 保留有效连接
            }
        }

        pool_.swap(valid_connections); // 交换队列

        // 如果连接数小于最小连接数，补充连接
        while (pool_.size() < min_pool_size_ && total_connections_ < max_pool_size_)
        {
            try
            {
                pool_.push(create_connection());
            }
            catch (...)
            {
                break;
            }
        }
    }

    // 清理线程函数
    void cleanup_thread_func()
    {
        while (cleanup_thread_running_)
        {
            std::this_thread::sleep_for(std::chrono::seconds(30)); // 每30秒清理一次
            if (cleanup_thread_running_)
            {
                cleanup_idle_connections();
            }
        }
    }

public:
    Mysql_Pool(const std::string& host, int port, const std::string& user,
        const std::string& db_name, const std::string& password,
        size_t min_pool_size = 5, size_t max_pool_size = 20,
        std::chrono::seconds connection_timeout = std::chrono::seconds(5),
        std::chrono::seconds max_idle_time = std::chrono::seconds(300))
        : host_(host), port_(port), user_(user), db_name_(db_name),
        password_(password), min_pool_size_(min_pool_size),
        max_pool_size_(max_pool_size), connection_timeout_(connection_timeout),
        max_idle_time_(max_idle_time)
    {
        if (min_pool_size_ > max_pool_size_)
        {
            throw std::invalid_argument("min_pool_size cannot be greater than max_pool_size");
        }
        if (min_pool_size_ == 0)
        {
            throw std::invalid_argument("min_pool_size must be greater than 0");
        }
        // 创建初始连接
        for (size_t i = 0; i < min_pool_size_; ++i)
        {
            try
            {
                pool_.push(create_connection());
            }
            catch (const std::exception& e)
            {
                // 清理已创建的连接
                while (!pool_.empty())
                {
                    auto conn = pool_.front();
                    pool_.pop();
                }
                throw std::runtime_error("Failed to initialize connection pool: " + std::string(e.what()));
            }
        }
        // 启动清理线程
        cleanup_thread_running_ = true;
        cleanup_thread_ = std::thread(&Mysql_Pool::cleanup_thread_func, this); // 启动清理线程
    }
    ~Mysql_Pool()
    {
        cleanup_thread_running_ = false; // 停止清理线程
        if (cleanup_thread_.joinable()) // 等待线程结束
        {
            cleanup_thread_.join(); // 等待清理线程结束
        }

        std::lock_guard<std::mutex> lock(mutex_); // 锁定连接池
        while (!pool_.empty())
        {
            pool_.pop();
        }
    }
    // 禁止拷贝
    Mysql_Pool(const Mysql_Pool&) = delete;
    Mysql_Pool& operator=(const Mysql_Pool&) = delete;
    // 允许移动
    Mysql_Pool(Mysql_Pool&&) = delete;
    Mysql_Pool& operator=(Mysql_Pool&&) = delete;
    // 获取连接
    std::shared_ptr<Mysql_Connection> get_connection() // 获取连接
    {
        waiting_threads_++; // 增加等待线程数
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待可用连接或超时
        bool success = cv_.wait_for(lock, connection_timeout_, [this]()
        {
            return !pool_.empty() || total_connections_ < max_pool_size_; // 有可用连接或可创建新连接
        });

        if (!success) // 超时
        {
            waiting_threads_--;
            throw std::runtime_error("Get connection timeout after " +
                std::to_string(connection_timeout_.count()) + " seconds");
        }
        std::shared_ptr<Mysql_Connection> conn; // 连接指针
        if (!pool_.empty()) // 使用池中连接
        {
            // 从池中获取连接
            conn = pool_.front();
            pool_.pop();
            // 检查连接是否有效
            if (!conn->is_valid())
            {
                destroyed_connections_++; // 统计销毁数
                total_connections_--; // 减少总连接数
                try
                {
                    conn = create_connection(); // 创建新连接
                }
                catch (const std::exception& e)
                {
                    waiting_threads_--;
                    throw std::runtime_error("Connection is invalid and failed to create new one: " +
                        std::string(e.what()));
                }
            }
        }
        else if (total_connections_ < max_pool_size_) // 创建新连接
        {
            // 创建新连接
            try
            {
                conn = create_connection();
            }
            catch (const std::exception& e)
            {
                waiting_threads_--;
                throw;
            }
        }
        if (conn)
        {
            conn->last_borrow_time_ = std::chrono::steady_clock::now(); // 更新借用时间
        }
        waiting_threads_--;
        return conn;
    }
    // 释放连接
    void release_connection(std::shared_ptr<Mysql_Connection> conn)
    {
        if (!conn)
        {
            return;
        }
        // 重置连接状态
        conn->reset();
        conn->last_return_time_ = std::chrono::steady_clock::now(); // 更新归还时间
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 如果连接池已满或连接无效，销毁连接
            if (pool_.size() >= max_pool_size_ || !conn->is_valid())
            {
                destroyed_connections_++; // 统计销毁数
                total_connections_--; // 减少总连接数
            }
            else
            {
                pool_.push(conn);
            }
        }
        cv_.notify_one();
    }

    // 强制清理所有连接
    void cleanup_all_connections()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty())
        {
            auto conn = pool_.front();
            pool_.pop();
            destroyed_connections_++;
            total_connections_--;
        }
        // 重新创建最小连接数
        for (size_t i = 0; i < min_pool_size_; ++i)
        {
            try
            {
                pool_.push(create_connection());
            }
            catch (...)
            {
                break;
            }
        }
    }

    // 获取统计信息
    PoolStats get_stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = total_connections_.load(); // 总连接数
        size_t idle = pool_.size(); // 空闲连接数
        size_t active = total > idle ? total - idle : 0; // 活跃连接数
        return PoolStats{
            total,
            idle,
            active,
            waiting_threads_.load(),
            created_connections_.load(),
            destroyed_connections_.load()
        };
    }

    // 手动清理空闲连接
    void manual_cleanup()
    {
        cleanup_idle_connections(); // 清理空闲连接
    }
};

// RAII 连接守卫
class MySQLConnGuard
{
public:
    explicit MySQLConnGuard(Mysql_Pool& pool)
        : pool_(pool), conn_(nullptr)
    {
        conn_ = pool_.get_connection();
    }

    ~MySQLConnGuard()
    {
        if (conn_)
        {
            pool_.release_connection(conn_);
        }
    }

    // 禁止拷贝
    MySQLConnGuard(const MySQLConnGuard&) = delete;
    MySQLConnGuard& operator=(const MySQLConnGuard&) = delete;

    // 允许移动
    MySQLConnGuard(MySQLConnGuard&& other) noexcept
        : pool_(other.pool_), conn_(std::move(other.conn_))
    {
        other.conn_.reset();
    }

    MySQLConnGuard& operator=(MySQLConnGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (conn_)
            {
                pool_.release_connection(conn_);
            }
            conn_ = std::move(other.conn_);
            other.conn_.reset();
        }
        return *this;
    }

    // 访问连接
    MYSQL* operator->() const // 访问 MYSQL 结构体
    {
        if (!conn_ || !conn_->conn_)
        {
            throw std::runtime_error("Invalid MySQL connection");
        }
        return conn_->conn_;
    }

    MYSQL* get() const // 获取 MYSQL 结构体 指针                                                                       
    {
        if (!conn_ || !conn_->conn_)
        {
            throw std::runtime_error("Invalid MySQL connection");
        }
        return conn_->conn_;
    }
    // 检查连接是否有效
    bool is_valid() const
    {
        return conn_ && conn_->is_valid();
    }
    // 显式归还连接
    void release()
    {
        if (conn_)
        {
            pool_.release_connection(conn_);
            conn_.reset();
        }
    }

private:
    Mysql_Pool& pool_;
    std::shared_ptr<Mysql_Connection> conn_;
};
