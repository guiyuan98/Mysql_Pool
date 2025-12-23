# MySQL 连接池

一个基于 C++17 的 MySQL 连接池实现，包含连接超时等待、空闲清理、RAII 连接守卫等能力。

## 特性
- 最小/最大连接数控制
- 连接获取超时等待（条件变量）
- 空闲连接清理（后台线程）
- 连接有效性检查与重置
- RAII 守卫自动归还连接
- 基础统计信息（总数/空闲/活跃/等待）

## 文件
```
B/mysql_pool.h
```

## 依赖
- MySQL C API（`mysqlclient`）
- C++17

## 使用示例
```cpp
#include "mysql_pool.h"

int main() {
  Mysql_Pool pool("127.0.0.1", 3306, "user", "db", "pwd", 5, 20);
  MySQLConnGuard conn(pool);
  if (conn.is_valid()) {
    // 使用 conn-> 执行 MySQL C API 调用
  }
  return 0;
}
```

## 说明
- 连接池大小与超时参数请根据业务负载调整。
- 需要先确保 MySQL 客户端库已安装并正确链接。

## License
MIT. See `LICENSE`.
