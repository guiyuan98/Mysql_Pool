#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <mysql/mysql.h>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
class Mysql_Connection // MySQL 连接封装类
{
  private:
    MYSQL *conn;                                            // mysql 连接句柄
    std::chrono::steady_clock::time_point last_used_time;   // 记录上次使用时间
    std::chrono::steady_clock::time_point last_return_time; // 记录上次归还时间
  public:
    Mysql_Connection(const std::string &host, int port, const std::string &user,
                     const std::string &password, const std::string &db_name) {
        conn = mysql_init(nullptr); // 初始化 MySQL 连接
        if (!conn) {
            throw std::runtime_error("mysql_init failed");
        }
        mysql_options(conn, MYSQL_OPT_RECONNECT, "1"); // 启用自动重连选项
        if (!mysql_real_connect(conn, host.c_str(), user.c_str(), password.c_str(),
                                db_name.c_str(), port, nullptr,
                                CLIENT_MULTI_STATEMENTS)) {
            std::string err = mysql_error(conn);
            mysql_close(conn);
            throw std::runtime_error("mysql_real_connect failed: " + err);
        }

        last_used_time = std::chrono::steady_clock::now();
        last_return_time = std::chrono::steady_clock::now();
    }
    ~Mysql_Connection() {
        if (conn) {
            mysql_close(conn);
            conn = nullptr;
        }
    }
    MYSQL *get_conn() // 获取 MySQL 连接句柄
    {
        return conn;
    }
    bool is_valid() // 检查连接是否有效
    {
        if (!conn) {
            return false;
        }
        return mysql_ping(conn) == 0;
    }
    void reset() // 重置连接状态
    {
        if (conn) {
            mysql_reset_connection(conn);
        }
    }
    std::chrono::steady_clock::time_point
    get_last_used_time() const // 获取上次使用时间
    {
        return last_used_time;
    }
    std::chrono::steady_clock::time_point
    get_last_return_time() const // 获取上次归还时间
    {
        return last_return_time;
    }
    void update_last_used_time() // 更新上次使用时间
    {
        last_used_time = std::chrono::steady_clock::now();
    }
    void update_last_return_time() // 更新上次归还时间
    {
        last_return_time = std::chrono::steady_clock::now();
    }
};

class Mysql_Pool // MySQL 连接池类
{
  private:
    struct PoolConfig // 连接池配置结构体
    {
        std::string host;                        // 数据库主机地址
        int port;                                // 数据库端口号
        std::string user;                        // 数据库用户名
        std::string password;                    // 数据库密码
        std::string db_name;                     // 数据库名称
        size_t min_connections;                  // 最小连接数
        size_t max_connections;                  // 最大连接数
        std::chrono::seconds connection_timeout; // 连接获取超时时间
        std::chrono::seconds idle_timeout;       // 连接空闲超时时间
    } config;
    struct PoolStats {
        std::atomic<int> total_connections; // 当前连接总数
        std::atomic<int> free_connections;  // 当前空闲连接数
        std::atomic<int> used_connections;  // 当前使用中的连接数
        std::atomic<int> waiting_threads;   // 当前等待获取连接的线程数
        std::atomic<int> task_nums;         // 当前任务数
    } stats;
    std::queue<std::shared_ptr<Mysql_Connection>>
        free_connections;                                                         // 空闲连接队列
    std::mutex mutex_;                                                            // 互斥锁
    std::condition_variable cv_;                                                  // 条件变量，用于通知等待的线程
    std::atomic<bool> shutdown_;                                                  // 连接池是否关闭
    std::thread cleaner_thread;                                                   // 清理线程
    std::shared_ptr<Mysql_Connection> create_connection(bool add_to_free = false) // 创建新的 MySQL 连接
    {
        auto conn = std::make_shared<Mysql_Connection>(
            config.host, config.port, config.user, config.password, config.db_name);
        stats.total_connections++;
        if (add_to_free) {
            stats.free_connections++;
        }
        return conn;
    }
    void cleanup_idle_connections() // 清理无用的空闲连接
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<std::shared_ptr<Mysql_Connection>>
            valid_connections; // 临时队列存放有效连接
        while (!free_connections.empty()) {
            auto conn = free_connections.front();
            free_connections.pop();
            if (!conn->is_valid() ||
                std::chrono::steady_clock::now() - conn->get_last_return_time() >
                    config.idle_timeout) {
                stats.total_connections--;
                stats.free_connections--;
            } else {
                valid_connections.push(conn);
            }
        }
        free_connections = std::move(valid_connections);
        while (stats.total_connections < config.min_connections) {
            auto conn = create_connection(true); // 创建空闲连接
            free_connections.push(conn);
        }
    }
    void cleanup_thread_func() // 清理线程函数
    {
        while (!shutdown_) {
            std::this_thread::sleep_for(std::chrono::seconds(30)); // 每30秒清理一次
            cleanup_idle_connections();
        }
    }

  public:
    Mysql_Pool(const std::string &host, int port, const std::string &user,
               const std::string &password, const std::string &db_name,
               size_t min_connections = 5, size_t max_connections = 20,
               std::chrono::seconds connection_timeout = std::chrono::seconds(5),
               std::chrono::seconds idle_timeout = std::chrono::seconds(300))
        : shutdown_(false) {
        config.host = host;
        config.port = port;
        config.user = user;
        config.password = password;
        config.db_name = db_name;
        config.min_connections = min_connections;
        config.max_connections = max_connections;
        config.connection_timeout = connection_timeout;
        config.idle_timeout = idle_timeout;
        stats.total_connections = 0;
        stats.free_connections = 0;
        stats.used_connections = 0;
        stats.waiting_threads = 0;
        stats.task_nums = 0;
        // 初始化连接池
        for (int i = 0; i < config.min_connections; ++i) {
            auto conn = create_connection(true); // 创建空闲连接
            free_connections.push(conn);
        }
        cleaner_thread = std::thread(&Mysql_Pool::cleanup_thread_func, this);
    }
    ~Mysql_Pool() {
        shutdown_ = true;
        cv_.notify_all(); // 通知所有等待的线程，以便它们能正常退出
        if (cleaner_thread.joinable())
            cleaner_thread.join();
        std::lock_guard<std::mutex> lock(mutex_);
        while (!free_connections.empty())
            free_connections.pop();
    }
    Mysql_Pool(const Mysql_Pool &) = delete; // 禁止拷贝
    Mysql_Pool &operator=(const Mysql_Pool &) = delete;
    std::shared_ptr<Mysql_Connection> get_connection() // 获取连接
    {
        if (shutdown_) {
            throw std::runtime_error("Connection pool is shut down");
        }

        std::unique_lock<std::mutex> lock(mutex_);

        // 尝试直接获取空闲连接（快速路径）
        if (!free_connections.empty()) {
            auto conn = free_connections.front();
            free_connections.pop();
            stats.free_connections--;
            stats.used_connections++;
            conn->update_last_used_time();
            return conn;
        }

        // 如果没有空闲连接，尝试创建新连接
        if (stats.total_connections < config.max_connections) {
            // 先在锁保护下预留位置，防止并发创建超过上限
            stats.total_connections++;
            stats.used_connections++;             // 预留使用位置
            lock.unlock();                        // 释放锁，因为创建连接可能需要较长时间
            auto conn = create_connection(false); // 创建后直接使用，不加入空闲队列
            lock.lock();
            stats.used_connections++; // 新创建的连接直接使用
            conn->update_last_used_time();
            return conn;
        }

        // 连接池已满，需要等待
        stats.waiting_threads++;
        auto expire_time = std::chrono::steady_clock::now() + config.connection_timeout;

        // 使用条件变量等待，直到有空闲连接或超时
        bool got_connection = cv_.wait_until(lock, expire_time, [this] {
            return !free_connections.empty() || shutdown_ ||
                   stats.total_connections < config.max_connections;
        });

        std::shared_ptr<Mysql_Connection> conn = nullptr;

        if (shutdown_) {
            stats.waiting_threads--;
            throw std::runtime_error("Connection pool is shut down");
        }

        // 再次尝试获取连接
        if (!free_connections.empty()) {
            conn = free_connections.front();
            free_connections.pop();
            stats.free_connections--;
            stats.used_connections++;
            conn->update_last_used_time();
            stats.waiting_threads--;
            return conn;
        }

        // 尝试创建新连接（可能因为连接数限制解除）
        if (stats.total_connections < config.max_connections) {
            // 先在锁保护下预留位置，防止并发创建超过上限
            stats.total_connections++;
            stats.used_connections++; // 预留使用位置
            lock.unlock();
            conn = create_connection(false); // 创建后直接使用，不加入空闲队列
            lock.lock();
            stats.used_connections++; // 新创建的连接直接使用
            conn->update_last_used_time();
            stats.waiting_threads--;
            return conn;
        }

        // 超时且无法获取连接
        stats.waiting_threads--;
        throw std::runtime_error("Get connection timed out");
    }
    void return_connection(std::shared_ptr<Mysql_Connection> conn) // 归还连接
    {
        if (!conn) {
            return; // 避免空指针
        }
        std::lock_guard<std::mutex> lock(mutex_);
        conn->update_last_return_time();
        conn->reset();
        free_connections.push(conn);
        stats.free_connections++;
        stats.used_connections--;
        // 通知等待的线程有空闲连接可用（关键优化：使用条件变量立即唤醒）
        cv_.notify_one();
    }
    void cleanup_all_connections() // 清理所有连接
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!free_connections.empty()) {
            free_connections.pop();
            stats.total_connections--;
            stats.free_connections--;
        }
        while (stats.total_connections < config.min_connections) {
            auto conn = create_connection(true); // 创建空闲连接
            free_connections.push(conn);
        }
    }
    std::string get_stats() const // 获取连接池状态
    {
        return "Total Connections: " +
               std::to_string(stats.total_connections.load()) +
               ", Free Connections: " +
               std::to_string(stats.free_connections.load()) +
               ", Used Connections: " +
               std::to_string(stats.used_connections.load()) +
               ", Waiting Threads: " +
               std::to_string(stats.waiting_threads.load()) +
               ", Task Nums: " + std::to_string(stats.task_nums.load());
    }
};

class Mysql_Connection_RAII // MySQL 连接 RAII 封装类
{
  private:
    Mysql_Pool &pool_;
    std::shared_ptr<Mysql_Connection> connection_;

  public:
    Mysql_Connection_RAII(Mysql_Pool &pool) : pool_(pool) {
        connection_ = pool_.get_connection();
    }
    ~Mysql_Connection_RAII() {
        if (connection_) {
            pool_.return_connection(connection_);
        }
    }
    Mysql_Connection_RAII(const Mysql_Connection_RAII &) = delete; // 禁止拷贝
    Mysql_Connection_RAII &operator=(const Mysql_Connection_RAII &) = delete;
    Mysql_Connection_RAII(Mysql_Connection_RAII &&) = delete; // 禁止移动
    Mysql_Connection_RAII &operator=(Mysql_Connection_RAII &&) = delete;
    MYSQL *get_conn() // 获取 MySQL 连接句柄
    {
        return connection_->get_conn();
    }
    bool is_valid() // 检查连接是否有效
    {
        return connection_->is_valid();
    }
};
