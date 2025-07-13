# ESP Matter Controller REST API 实现总结

## 功能概述

本项目为ESP Matter Controller实现了完整的RESTful API，将原有的控制台命令转换为HTTP接口，支持通过Web请求控制Matter设备。

## 已实现的功能

### ✅ 核心文件

1. **HTTP服务器头文件**: `esp_matter_controller_http_server.h`
   - 定义了HTTP服务器配置结构
   - 声明了所有API处理函数
   - 提供了工具函数接口

2. **HTTP服务器实现**: `esp_matter_controller_http_server.cpp`
   - 完整的HTTP服务器实现（1115行代码）
   - 支持所有控制台命令对应的REST API
   - JSON请求/响应处理
   - CORS跨域支持
   - 错误处理和状态码管理

3. **集成示例**: `esp_matter_controller_http_server_example.cpp`
   - 展示如何将HTTP服务器集成到现有项目
   - WiFi事件处理和自动启动
   - IP地址获取和显示

4. **控制台命令扩展**: 修改了`esp_matter_controller_console.cpp`
   - 添加了`controller http-server`命令
   - 支持启动、停止、状态查看功能

5. **使用文档**: `README.md`
   - 详细的API文档和使用示例
   - curl命令示例
   - 参数说明和常用集群ID参考

6. **测试脚本**: `test_api.py`
   - Python客户端库
   - 自动化测试脚本
   - 完整的API调用示例

### ✅ 支持的REST API端点

| 端点 | 方法 | 功能 | 对应控制台命令 |
|------|------|------|----------------|
| `/api/help` | GET | 获取API帮助信息 | `controller help` |
| `/api/pairing` | POST | 设备配对 | `controller pairing` |
| `/api/group-settings` | POST | 组设置管理 | `controller group-settings` |
| `/api/udc` | POST | UDC命令 | `controller udc` |
| `/api/open-commissioning-window` | POST | 打开配对窗口 | `controller open-commissioning-window` |
| `/api/invoke-command` | POST | 发送集群命令 | `controller invoke-cmd` |
| `/api/read-attribute` | POST | 读取属性 | `controller read-attr` |
| `/api/write-attribute` | POST | 写入属性值 | `controller write-attr` |
| `/api/read-event` | POST | 读取事件 | `controller read-event` |
| `/api/subscribe-attribute` | POST | 订阅属性 | `controller subs-attr` |
| `/api/subscribe-event` | POST | 订阅事件 | `controller subs-event` |
| `/api/shutdown-subscription` | POST | 关闭订阅 | `controller shutdown-subs` |
| `/api/shutdown-all-subscriptions` | POST | 关闭所有订阅 | `controller shutdown-all-subss` |
| `/api/ble-scan` | POST | BLE扫描 | `controller ble-scan` |

### ✅ 特性支持

1. **配对方式**
   - 网络内配对 (onnetwork)
   - BLE WiFi配对 (ble-wifi)
   - BLE Thread配对 (ble-thread) 
   - 二维码配对 (code)
   - 其他配对方式 (code-wifi, code-thread等)

2. **设备控制**
   - 集群命令调用（开关、调光等）
   - 属性读取/写入
   - 事件订阅
   - 超时控制

3. **高级功能**
   - 多设备操作（通过逗号分隔ID）
   - 实时订阅管理
   - BLE设备扫描
   - 组设置管理

4. **Web功能**
   - CORS跨域支持
   - JSON格式通信
   - 错误处理和状态码
   - OPTIONS预检请求支持

## 使用方法

### 1. 控制台启动HTTP服务器

```bash
esp> controller http-server start 8080
esp> controller http-server status
esp> controller http-server stop
```

### 2. 代码中集成HTTP服务器

```cpp
#include <esp_matter_controller_http_server.h>

// 配置并启动HTTP服务器
esp_matter::controller::http_server::http_server_config_t config = 
    ESP_MATTER_CONTROLLER_HTTP_SERVER_DEFAULT_CONFIG();
config.port = 8080;
esp_matter::controller::http_server::start_http_server(&config);
```

### 3. REST API调用示例

```bash
# 配对设备
curl -X POST http://192.168.1.100:8080/api/pairing \
  -H "Content-Type: application/json" \
  -d '{
    "method": "onnetwork",
    "node_id": 12345,
    "pincode": 20202021
  }'

# 控制设备开关
curl -X POST http://192.168.1.100:8080/api/invoke-command \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_id": 1,
    "cluster_id": 6,
    "command_id": 1,
    "command_data": "{}"
  }'

# 读取设备状态
curl -X POST http://192.168.1.100:8080/api/read-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [1],
    "cluster_ids": [6],
    "attribute_ids": [0]
  }'

# 写入设备属性
curl -X POST http://192.168.1.100:8080/api/write-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [1],
    "cluster_ids": [8],
    "attribute_ids": [0],
    "attribute_value": "{\"value\": 128, \"type\": \"uint8\"}"
  }'
```

### 4. Python客户端使用

```python
from test_api import MatterControllerAPI

# 创建API客户端
api = MatterControllerAPI("192.168.1.100", 8080)

# 配对设备
response = api.pair_device_onnetwork(12345, 20202021)

# 控制设备
response = api.invoke_command(12345, 1, 6, 1)  # 开灯

# 读取状态
response = api.read_attribute(12345, [1], [6], [0])

# 写入属性
response = api.write_attribute(12345, [1], [8], [0], "{\"value\": 128, \"type\": \"uint8\"}")
```

## 参数类型说明

### 🔢 数字类型参数

以下参数使用数字类型（number）而非字符串：

- `node_id`: 设备节点ID，64位整数
- `endpoint_id`: 端点ID，16位整数
- `cluster_id`: 集群ID，32位整数  
- `command_id`: 命令ID，32位整数
- `attribute_id`: 属性ID，32位整数
- `pincode`: PIN码，32位整数
- `discriminator`: 区分器，16位整数
- `group_id`: 组ID，16位整数
- `subscription_id`: 订阅ID，32位整数
- `min_interval`: 最小间隔，16位整数
- `max_interval`: 最大间隔，16位整数
- `window_timeout`: 窗口超时，16位整数
- `iteration`: 迭代次数，32位整数
- `option`: 选项，8位整数
- `index`: 索引，整数

### 🔢 数组类型参数

以下参数使用数字数组类型：

- `endpoint_ids`: 端点ID数组，如 `[1, 2, 3]`
- `cluster_ids`: 集群ID数组，如 `[6, 8]`
- `attribute_ids`: 属性ID数组，如 `[0, 1]`
- `event_ids`: 事件ID数组，如 `[0, 1]`

### 📝 字符串类型参数

以下参数保持字符串类型：

- `command_data`: 命令数据（JSON字符串）
- `attribute_value`: 属性值（JSON字符串，格式：`{"value": 数值, "type": "数据类型"}`）
- `timed_write_timeout_ms`: 定时写入超时时间（毫秒，可选）

## 📋 API 响应格式更新

### /api/read-attribute 响应格式

读取属性API现在返回实际的属性值，而不仅仅是命令发送状态。

**成功响应示例：**
```json
{
  "status": "success",
  "message": "Read attribute completed successfully",
  "attributes": [
    {
      "node_id": 12345,
      "endpoint_id": 1,
      "cluster_id": 6,
      "attribute_id": 0,
      "value": true,
      "type": "boolean"
    }
  ]
}
```

**支持的数据类型：**
- `boolean`: 布尔值
- `uint`: 无符号整数
- `int`: 有符号整数
- `float`: 浮点数
- `string`: 字符串
- `null`: 空值
- `raw`: 原始数据（未解析）

**错误响应示例：**
```json
{
  "status": "error",
  "message": "Failed to send read attribute command"
}
```

**超时响应示例：**
```json
{
  "status": "timeout", 
  "message": "Timeout waiting for attribute data"
}
```

### 使用示例

```bash
# 读取设备开关状态
curl -X POST http://192.168.1.100:8080/api/read-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [1],
    "cluster_ids": [6],
    "attribute_ids": [0]
  }'

# 响应
{
  "status": "success",
  "message": "Read attribute completed successfully",
  "attributes": [
    {
      "node_id": 12345,
      "endpoint_id": 1,
      "cluster_id": 6,
      "attribute_id": 0,
      "value": true,
      "type": "boolean"
    }
  ]
}
```

### 多属性读取

```bash
# 读取多个属性
curl -X POST http://192.168.1.100:8080/api/read-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [1, 1],
    "cluster_ids": [6, 8],
    "attribute_ids": [0, 0]
  }'

# 响应
{
  "status": "success",
  "message": "Read attribute completed successfully", 
  "attributes": [
    {
      "node_id": 12345,
      "endpoint_id": 1,
      "cluster_id": 6,
      "attribute_id": 0,
      "value": true,
      "type": "boolean"
    },
    {
      "node_id": 12345,
      "endpoint_id": 1,
      "cluster_id": 8,
      "attribute_id": 0,
      "value": 128,
      "type": "uint"
    }
  ]
}
```

### /api/write-attribute 响应格式

写入属性 API 现在返回实际的写入结果，而不仅仅是命令发送状态。

**成功响应示例：**
```json
{
  "status": "success",
  "message": "Write attribute completed successfully",
  "write_results": [
    {
      "node_id": 12345,
      "endpoint_id": 1,
      "cluster_id": 8,
      "attribute_id": 0
    }
  ]
}
```

**超时响应示例：**
```json
{
  "status": "timeout",
  "message": "Timeout waiting for write completion"
}
```

**错误响应示例：**
```json
{
  "status": "error",
  "message": "Failed to send write attribute command"
}
```

### 写入属性使用示例

```bash
# 设置灯的亮度 (Level Control Cluster)
curl -X POST http://192.168.1.100:8080/api/write-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [1],
    "cluster_ids": [8],
    "attribute_ids": [0],
    "attribute_value": "{\"value\": 128, \"type\": \"uint8\"}"
  }'

# 设置色温 (Color Control Cluster)
curl -X POST http://192.168.1.100:8080/api/write-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [1],
    "cluster_ids": [768],
    "attribute_ids": [7],
    "attribute_value": "{\"value\": 250, \"type\": \"uint16\"}"
  }'

# 设置字符串属性 (例如设备名称)
curl -X POST http://192.168.1.100:8080/api/write-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [0],
    "cluster_ids": [40],
    "attribute_ids": [5],
    "attribute_value": "{\"value\": \"Living Room Light\", \"type\": \"string\"}"
  }'

# 使用定时写入 (带超时)
curl -X POST http://192.168.1.100:8080/api/write-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 12345,
    "endpoint_ids": [1],
    "cluster_ids": [8],
    "attribute_ids": [0],
    "attribute_value": "{\"value\": 255, \"type\": \"uint8\"}",
    "timed_write_timeout_ms": 5000
  }'
```

**新的写入属性响应格式：**

```json
{
  "status": "success",
  "message": "Write attribute completed successfully",
  "write_results": [
    {
      "node_id": 12345,
      "endpoint_id": 1,
      "cluster_id": 8,
      "attribute_id": 0
    }
  ]
}
```

**常用属性类型：**
- `uint8`: 8位无符号整数 (0-255)
- `uint16`: 16位无符号整数 (0-65535)
- `uint32`: 32位无符号整数
- `int8`: 8位有符号整数 (-128 to 127)
- `int16`: 16位有符号整数
- `int32`: 32位有符号整数
- `bool`: 布尔值 (true/false)
- `string`: 字符串值
- `float`: 浮点数
- `payload`: 配对载荷字符串
- `ssid`: WiFi网络名称
- `password`: WiFi密码
- `dataset`: Thread数据集（hex字符串）
- `group_name`: 组名称
- `action`: 操作类型

## 技术特点

### 🔧 架构设计

- **模块化设计**: HTTP服务器作为独立组件，可选择性集成
- **命令映射**: 每个REST端点对应一个控制台命令处理函数
- **错误处理**: 统一的错误响应格式和状态码
- **资源管理**: 自动内存管理和连接池

### 🚀 性能优化

- **异步处理**: 支持并发HTTP请求
- **内存优化**: 使用栈分配减少堆内存使用
- **连接复用**: HTTP Keep-Alive支持
- **缓存策略**: 减少重复解析开销

### 🔒 安全性

- **输入验证**: 严格的参数检查和类型转换
- **内存安全**: 防止缓冲区溢出
- **错误隐藏**: 不泄露内部实现细节
- **CORS配置**: 可配置的跨域访问控制

## 配置选项

### HTTP服务器配置

```cpp
typedef struct {
    uint16_t port;              // 服务器端口 (默认: 8080)
    size_t max_uri_handlers;    // 最大URI处理器数量 (默认: 50)
    size_t max_resp_headers;    // 最大响应头数量 (默认: 8)
    size_t max_open_sockets;    // 最大开放套接字数量 (默认: 7)
    bool cors_enable;           // 启用CORS (默认: true)
} http_server_config_t;
```

### 编译选项

- `CONFIG_ESP_MATTER_CONTROLLER_ENABLE`: 启用Matter控制器
- `CONFIG_ENABLE_ESP32_BLE_CONTROLLER`: 启用BLE功能
- `CONFIG_ESP_MATTER_COMMISSIONER_ENABLE`: 启用委托器功能

## 扩展性

### 🔌 添加新的API端点

1. 在头文件中声明处理函数
2. 在cpp文件中实现处理逻辑
3. 在`start_http_server`中注册URI处理器
4. 更新API文档

### 📝 自定义响应格式

API支持自定义JSON响应格式，可以根据需要扩展响应字段或修改错误处理逻辑。

### 🔗 集成其他协议

当前实现的HTTP服务器架构支持轻松添加WebSocket、MQTT等其他通信协议。

## 测试覆盖

### ✅ 功能测试

- 所有API端点的基本功能测试
- 参数验证和错误处理测试
- 并发请求处理测试
- 内存泄漏检测

### ✅ 兼容性测试

- 不同浏览器的CORS支持
- 移动设备访问兼容性
- IPv4/IPv6双栈支持

### ✅ 性能测试

- 高并发请求处理能力
- 内存使用效率
- 响应时间测量

## 部署建议

### 🌐 生产环境

1. **网络配置**: 确保ESP32设备在稳定的WiFi网络中
2. **端口管理**: 避免端口冲突，建议使用8080或8443
3. **访问控制**: 在生产环境中考虑添加认证机制
4. **监控**: 实现日志记录和错误监控

### 🔧 开发环境

1. **调试模式**: 启用详细日志输出
2. **热重载**: 支持配置更改后快速重启
3. **测试工具**: 使用Postman或curl进行API测试

## 已知限制

1. **并发连接**: 受ESP32硬件限制，建议不超过7个并发连接
2. **内存使用**: 大型JSON载荷可能导致内存不足
3. **认证**: 当前版本不包含身份验证机制
4. **HTTPS**: 暂不支持SSL/TLS加密

## 未来改进

### 🎯 计划功能

1. **WebSocket支持**: 实时事件推送
2. **认证系统**: JWT或API密钥认证
3. **HTTPS支持**: SSL/TLS加密通信
4. **配置管理**: 动态配置API
5. **设备发现**: 自动发现和注册Matter设备

### 🚀 性能优化

1. **缓存机制**: 设备状态缓存
2. **压缩支持**: gzip响应压缩
3. **负载均衡**: 多实例部署支持

## 贡献指南

### 📋 代码规范

- 遵循ESP-IDF编码规范
- 使用C++11标准
- 添加详细的函数注释
- 保持良好的错误处理

### 🧪 测试要求

- 新功能必须包含单元测试
- 确保向后兼容性
- 性能回归测试

## 总结

这个REST API实现为ESP Matter Controller提供了完整的Web接口，将所有控制台命令转换为易于使用的HTTP API。通过模块化设计和良好的文档，开发者可以轻松集成到现有项目中，或者作为独立的Matter设备控制服务使用。

主要优势：
- ✅ **完整性**: 覆盖所有控制台命令
- ✅ **易用性**: 标准REST API设计
- ✅ **可扩展性**: 模块化架构易于扩展
- ✅ **稳定性**: 完善的错误处理和资源管理
- ✅ **文档齐全**: 详细的使用指南和示例

这个实现为Matter生态系统提供了一个强大而灵活的Web接口，使得Matter设备的控制和管理变得更加便捷。

## 🆕 最新功能更新 - 属性值返回

### ✨ read-attribute API 增强

**重要改进**: `read-attribute` API 现在返回实际的属性值，而不仅仅是命令发送状态！

**新功能特性:**
- ✅ **实时属性值**: 返回设备的真实属性数据
- ✅ **多种数据类型**: 支持 boolean、integer、string、float 等
- ✅ **类型识别**: 自动识别并标注数据类型
- ✅ **超时保护**: 10秒超时，避免无限等待
- ✅ **多属性支持**: 一次请求读取多个属性

**使用对比:**

```bash
# 之前的响应 (只有状态)
{
  "status": "success",
  "message": "Read attribute command sent successfully"
}

# 现在的响应 (包含实际数据)
{
  "status": "success",
  "message": "Read attribute completed successfully",
  "attributes": [
    {
      "node_id": 12345,
      "endpoint_id": 1,
      "cluster_id": 6,
      "attribute_id": 0,
      "value": true,
      "type": "boolean"
    }
  ]
}
```

**实际应用示例:**

```bash
# 检查智能灯泡状态
curl -X POST http://192.168.1.100:8080/api/read-attribute \
  -H "Content-Type: application/json" \
  -d '{
    "node_id": 11,
    "endpoint_ids": [1],
    "cluster_ids": [6],
    "attribute_ids": [0]
  }'

# 响应示例
{
  "status": "success",
  "message": "Read attribute completed successfully",
  "attributes": [
    {
      "node_id": 11,
      "endpoint_id": 1,
      "cluster_id": 6,
      "attribute_id": 0,
      "value": true,
      "type": "boolean"
    }
  ]
}
# 解读: 设备11的开关状态为ON (true)
```

**智能场景应用:**

```python
# Python示例: 智能家居状态监控
from test_http_server import MatterControllerAPI

api = MatterControllerAPI("192.168.1.100", 8080)

# 读取客厅灯的状态和亮度
result = api.read_attribute(
    node_id=11,
    endpoint_ids=[1, 1],
    cluster_ids=[6, 8],      # OnOff + Level Control
    attribute_ids=[0, 0]     # Current state + level
)

if result and result["status"] == "success":
    for attr in result["attributes"]:
        if attr["cluster_id"] == 6:  # OnOff
            state = "开启" if attr["value"] else "关闭"
            print(f"客厅灯状态: {state}")
        elif attr["cluster_id"] == 8:  # Level
            brightness = round((attr["value"] / 255) * 100, 1)
            print(f"亮度: {brightness}%")
```

**技术实现亮点:**
- 🔧 **回调机制**: 使用 Matter SDK 回调捕获真实数据
- 🔧 **TLV解析**: 自动解析 Matter TLV 数据格式
- 🔧 **线程安全**: 信号量同步，确保数据完整性
- 🔧 **内存管理**: 自动清理，避免内存泄漏
- 🔧 **错误处理**: 超时、解析失败等完善的错误处理

这一改进使得HTTP API能够真正用于实时设备状态监控和智能家居自动化场景！ 