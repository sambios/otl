#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <vector>
#include <map>

namespace otl {
namespace log {

// 日志级别定义
enum LogLevel {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL
};

// 输出目标类型
enum class OutputTarget : uint32_t {
    None        = 0,
    File        = 1 << 0,
    Console     = 1 << 1,
    Telnet      = 1 << 2
};
inline OutputTarget operator|(OutputTarget a, OutputTarget b) {
    return static_cast<OutputTarget>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(OutputTarget a, OutputTarget b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// 文件输出配置
struct FileConfig {
    std::string path;                 // 日志文件路径
    size_t rollSizeMB = 100;          // 滚动分割大小（MB）
    bool rollByTime = false;          // 是否按天分割（每日0点新建）
    size_t maxFiles = 10;             // 最大保留文件数
};

// Telnet输出配置
struct TelnetConfig {
    uint16_t port = 2323;             // 监听端口
    size_t maxConnections = 5;        // 最大并发连接数
    bool enable = false;              // 是否启用
};

// 日志模块总配置
struct LogConfig {
    OutputTarget targets = OutputTarget::Console; // 输出目标
    LogLevel level = LOG_INFO;                   // 初始日志级别
    FileConfig fileConfig;                       // 文件配置
    TelnetConfig telnetConfig;                   // Telnet配置
    bool enableConsole = true;                   // 是否启用系统控制台
    bool abortOnFatal = false;                   // LOG_FATAL是否终止进程
    size_t queueSize = 4096;                     // 缓冲队列大小
};

// 初始化/销毁/动态配置接口
void init(int argc, char* argv[]);
void init(const LogConfig& config);

void deinit();
void updateConfig(const LogConfig& config);
LogConfig getConfig();
// 简化接口：单独设置/获取日志级别
void setLevel(LogLevel level);
LogLevel getLevel();

// C++流式日志接口宏
#define OTL_LOG(level, moduleTag) \
    otl::log::LogStream(__FILE__, __LINE__, level, moduleTag)

// printf style log interface
void LogPrintf(const std::string& moduleTag, LogLevel level, const char* fmt, ...);

// Convenience printf-style macros (compatible with usage in stream_encoder.cpp)
#ifndef OTL_LOGE
#define OTL_LOGE(tag, fmt, ...) ::otl::log::LogPrintf((tag), ::otl::log::LOG_ERROR,   (fmt), ##__VA_ARGS__)
#endif
#ifndef OTL_LOGW
#define OTL_LOGW(tag, fmt, ...) ::otl::log::LogPrintf((tag), ::otl::log::LOG_WARNING, (fmt), ##__VA_ARGS__)
#endif
#ifndef OTL_LOGI
#define OTL_LOGI(tag, fmt, ...) ::otl::log::LogPrintf((tag), ::otl::log::LOG_INFO,    (fmt), ##__VA_ARGS__)
#endif
#ifndef OTL_LOGD
#define OTL_LOGD(tag, fmt, ...) ::otl::log::LogPrintf((tag), ::otl::log::LOG_DEBUG,   (fmt), ##__VA_ARGS__)
#endif
#ifndef OTL_LOGT
#define OTL_LOGT(tag, fmt, ...) ::otl::log::LogPrintf((tag), ::otl::log::LOG_TRACE,   (fmt), ##__VA_ARGS__)
#endif
#ifndef OTL_LOGF
#define OTL_LOGF(tag, fmt, ...) ::otl::log::LogPrintf((tag), ::otl::log::LOG_FATAL,   (fmt), ##__VA_ARGS__)
#endif

// 日志流对象声明
class LogStream : public std::ostringstream {
public:
    LogStream(const char* file, int line, LogLevel level, const std::string& moduleTag);
    ~LogStream();
private:
    const char* m_file;
    int m_line;
    LogLevel m_level;
    std::string m_moduleTag;
};

// Telnet指令相关结构体和接口
using TelnetCmdHandler = std::function<std::string(const std::vector<std::string>& args)>;

// 命令元数据结构体
struct TelnetCmdInfo {
    std::string name;              // 命令名称
    std::string format;            // 命令格式 (如 "cmd [arg1] <arg2>")
    std::string description;       // 命令描述
    std::string module;            // 命令所属模块
    TelnetCmdHandler handler;      // 命令处理函数
};

// 注册Telnet命令
void registerTelnetCommand(const std::string& cmd, TelnetCmdHandler handler);

// 注册带完整信息的Telnet命令
void registerTelnetCommand(const std::string& cmd, const std::string& format, 
                         const std::string& description, const std::string& module,
                         TelnetCmdHandler handler);

// Telnet服务器控制函数（内部使用）
void startTelnetServer(uint16_t port, size_t maxConnections);
void stopTelnetServer();

// Telnet命令测试函数（用于自动化测试）
std::string processTelnetCommandForTest(const std::vector<std::string>& args);

// 日志级别字符串转换
const char* LogLevelToString(LogLevel level);
LogLevel LogLevelFromString(const std::string& str);

} // namespace log
} // namespace otl
