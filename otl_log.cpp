#include "otl_log.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <map>
#include <set>
#include <sstream>
#include <memory>
#include <cstdarg>
#include <algorithm>
#include <sys/syscall.h>
#include <cstring>

// Network headers for Telnet server
#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

#include <cassert>

namespace otl {
namespace log {

// Log message structure
struct LogMessage {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string moduleTag;
    std::string file;
    int line;
    std::string content;
    uint32_t pid;
    uint64_t tid;
};

// Thread-safe queue for log messages
class LogQueue {
public:
    explicit LogQueue(size_t maxSize) : m_maxSize(maxSize) {}
    bool push(const LogMessage& msg) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.size() >= m_maxSize) return false;
        m_queue.push(msg);
        m_cv.notify_one();
        return true;
    }
    bool pop(LogMessage& msg) {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_queue.empty() && !m_stopped) {
            m_cv.wait(lock);
        }
        if (m_queue.empty()) return false;
        msg = m_queue.front();
        m_queue.pop();
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopped = true;
        }
        m_cv.notify_all();
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }
    
    size_t capacity() const {
        return m_maxSize;
    }
private:
    std::queue<LogMessage> m_queue;
    size_t m_maxSize;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopped = false;
};

// Get process ID
inline uint32_t get_pid() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}
// Get thread ID
inline uint64_t get_tid() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
#else
    return static_cast<uint64_t>(::syscall(SYS_gettid)); // Linux: SYS_gettid
#endif
}

// Format time to string with millisecond precision
std::string format_time(const std::chrono::system_clock::time_point& tp) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    std::ostringstream oss;
    oss << buf << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// Log level to string conversion internal implementation
std::string level_to_string(LogLevel level) {
    switch (level) {
        case LOG_TRACE: return "T";
        case LOG_DEBUG: return "D";
        case LOG_INFO: return "I";
        case LOG_WARNING: return "W";
        case LOG_ERROR: return "E";
        case LOG_FATAL: return "F";
        default: return "?";
    }
}

LogLevel level_from_string(const std::string& str) {
    if (str == "TRACE") return LOG_TRACE;
    if (str == "DEBUG") return LOG_DEBUG;
    if (str == "INFO") return LOG_INFO;
    if (str == "WARNING") return LOG_WARNING;
    if (str == "ERROR") return LOG_ERROR;
    if (str == "FATAL") return LOG_FATAL;
    return LOG_INFO;
}

// Global state
struct LoggerState {
    LogConfig config;
    std::unique_ptr<LogQueue> queue;
    std::thread worker;
    std::atomic<bool> running{false};
    std::ofstream fileStream;
    std::mutex fileMutex;
    // Telnet server state
    std::atomic<bool> telnetRunning{false};
    std::thread telnetThread;
    std::mutex telnetMutex;
    std::vector<int> telnetClients;
    int telnetSocket = -1;
    std::map<std::string, TelnetCmdInfo> telnetCommands;
    // TODO: file rolling state
};
LoggerState g_logger;

// Forward declarations for internal functions
namespace {

// ANSI color codes for console output
namespace Colors {
    const char* Reset   = "\033[0m";
    const char* Red     = "\033[31m";
    const char* Green   = "\033[32m";
    const char* Yellow  = "\033[33m";
    const char* Blue    = "\033[34m";
    const char* Magenta = "\033[35m";
    const char* Cyan    = "\033[36m";
    const char* White   = "\033[37m";
}

// Get color for log level
const char* get_level_color(LogLevel level) {
    switch (level) {
        case LOG_TRACE: return Colors::White;
        case LOG_DEBUG: return Colors::Cyan;
        case LOG_INFO: return Colors::Green;
        case LOG_WARNING: return Colors::Yellow;
        case LOG_ERROR: return Colors::Red;
        case LOG_FATAL: return Colors::Magenta;
        default: return Colors::White;
    }
}

// Format log message to string
std::string format_log(const LogMessage& msg, bool useColor = true) {
    std::ostringstream oss;
    
    // Basic timestamp
    oss << format_time(msg.timestamp) << " ";
    
    // Colored level indicator
    if (useColor) {
        oss << get_level_color(msg.level) << level_to_string(msg.level) << Colors::Reset;
    } else {
        oss << level_to_string(msg.level);
    }
    
    // Short format: timestamp level/module file:line content
    oss << "/" << msg.moduleTag << " "
        << msg.file << ":" << msg.line << " "
        << msg.content;
        
    return oss.str();
}

// Output to file (without colors)
void write_to_file(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_logger.fileMutex);
    if (g_logger.fileStream.is_open()) {
        g_logger.fileStream << line << std::endl;
    }
}
// Output to console
void write_to_console(const std::string& line, LogLevel level) {
    if (level >= LOG_ERROR) {
        std::cerr << line << std::endl;
    } else {
        std::cout << line << std::endl;
    }
}

// Log worker thread
void log_worker() {
    while (g_logger.running) {
        LogMessage msg;
        if (!g_logger.queue->pop(msg)) break;
        // Format with colors for console, without colors for file
        if (g_logger.config.targets & OutputTarget::File) {
            std::string fileLine = format_log(msg, false); // no colors for file
            write_to_file(fileLine);
        }
        if (g_logger.config.targets & OutputTarget::Console) {
            if (g_logger.config.enableConsole) {
                std::string consoleLine = format_log(msg, true); // with colors for console
                write_to_console(consoleLine, msg.level);
            }
        }
        // Telnet output
        if (g_logger.config.targets & OutputTarget::Telnet && g_logger.telnetRunning) {
            std::string telnetLine = format_log(msg, false); // no colors for telnet
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            for (auto client : g_logger.telnetClients) {
                if (client > 0) {
                    telnetLine += "\r\n"; // CRLF for telnet
                    // Non-blocking write, ignore errors
                    send(client, telnetLine.c_str(), telnetLine.length(), 0);
                }
            }
        }
        if (msg.level == LOG_FATAL && g_logger.config.abortOnFatal) {
            std::cerr << "Process aborted due to FATAL log." << std::endl;
            std::abort();
        }
    }
    // Flush remaining messages
    LogMessage msg;
    while (g_logger.queue->pop(msg)) {
        // Format with colors for console, without colors for file
        if (g_logger.config.targets & OutputTarget::File) {
            std::string fileLine = format_log(msg, false); // no colors for file
            write_to_file(fileLine);
        }
        if (g_logger.config.targets & OutputTarget::Console) {
            if (g_logger.config.enableConsole) {
                std::string consoleLine = format_log(msg, true); // with colors for console
                write_to_console(consoleLine, msg.level);
            }
        }
        // Telnet output
        if (g_logger.config.targets & OutputTarget::Telnet && g_logger.telnetRunning) {
            std::string telnetLine = format_log(msg, false); // no colors for telnet
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            for (auto client : g_logger.telnetClients) {
                if (client > 0) {
                    telnetLine += "\r\n"; // CRLF for telnet
                    // Non-blocking write, ignore errors
                    send(client, telnetLine.c_str(), telnetLine.length(), 0);
                }
            }
        }
    }
}

} // anonymous namespace

// Logger initialization
void init(const LogConfig& config) {
    deinit();
    g_logger.config = config;
    g_logger.queue.reset(new LogQueue(config.queueSize));
    g_logger.running = true;
    if (config.targets & OutputTarget::File) {
        g_logger.fileStream.open(config.fileConfig.path, std::ios::app);
    }
    // Register built-in telnet commands if not already registered
    {
        static bool registered = false;
        if (!registered) {
            // System commands
            // Help command - handled specially in processTelnetCommand
            registerTelnetCommand("help", "help [module]", 
                                 "List available commands, optionally filtered by module", 
                                 "System", 
                                 nullptr);
            
            // Cmdshow command - handled specially in processTelnetCommand
            registerTelnetCommand("cmdshow", "cmdshow [module]", 
                                 "Show detailed information about available commands, optionally filtered by module", 
                                 "System", 
                                 nullptr);
            
            // Status command
            registerTelnetCommand("status", "status", 
                                "Show logger status", 
                                "System", 
                                [](const std::vector<std::string>& args) -> std::string {
                                    std::ostringstream oss;
                                    oss << "=== Logger Status ===\r\n";
                                    oss << "Log level: " << ::otl::log::LogLevelToString(g_logger.config.level) << "\r\n";
                                    oss << "Output targets: ";
                                    if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::Console)) != 0) oss << "console ";
                                    if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::File)) != 0) oss << "file ";
                                    if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::Telnet)) != 0) oss << "telnet ";
                                    oss << "\r\n";
                                    
                                    if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::File)) != 0) {
                                        oss << "Log file: " << g_logger.config.fileConfig.path << "\r\n";
                                    }
                                    
                                    oss << "Queue capacity: " << g_logger.queue->capacity() << "\r\n";
                                    oss << "Queue size: " << g_logger.queue->size() << "\r\n";
                                    
                                    if (g_logger.telnetSocket != -1) {
                                        oss << "Telnet server: running on port " << g_logger.config.telnetConfig.port << "\r\n";
                                        oss << "Telnet connections: " << g_logger.telnetClients.size() << "/" 
                                            << g_logger.config.telnetConfig.maxConnections << "\r\n";
                                    } else {
                                        oss << "Telnet server: not running\r\n";
                                    }
                                    
                                    return oss.str();
                                });
            
            // Level command
            registerTelnetCommand("level", 
                                "level [trace|debug|info|warning|error|fatal]",
                                "Get or set log level",
                                "System",
                                [](const std::vector<std::string>& args) -> std::string {
                                    if (args.size() < 2) {
                                        // No level provided, return current level
                                        std::string currentLevel = LogLevelToString(g_logger.config.level);
                                        return std::string("Current log level: ") + currentLevel + "\r\n";
                                    }
                                    
                                    LogLevel level = LogLevelFromString(args[1]);
                                    if (level == LOG_FATAL && args[1] != "fatal") {
                                        return std::string("Invalid log level: ") + args[1] + "\r\n";
                                    }
                                    
                                    LogConfig config = getConfig();
                                    config.level = level;
                                    ::otl::log::updateConfig(config);
                                    
                                    std::string newLevel = LogLevelToString(level);
                                    return std::string("Log level set to: ") + newLevel + "\r\n";
                                });
            
            // Enable command
            registerTelnetCommand("enable", 
                                "enable [console|file|telnet]",
                                "Enable output to specified target",
                                "System",
                                [](const std::vector<std::string>& args) -> std::string {
                                    if (args.size() < 2) {
                                        return std::string("Usage: enable [console|file|telnet]\r\n");
                                    }
                                    
                                    std::string target = args[1];
                                    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                                    
                                    LogConfig config = getConfig();
                                    
                                    if (target == "console") {
                                        config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::Console));
                                    } else if (target == "file") {
                                        config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::File));
                                    } else if (target == "telnet") {
                                        config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::Telnet));
                                    } else {
                                        return std::string("Unknown target: ") + target + "\r\n";
                                    }
                                    
                                    ::otl::log::updateConfig(config);
                                    
                                    return target + std::string(" output enabled\r\n");
                                });
            
            // Disable command
            registerTelnetCommand("disable", 
                                "disable [console|file|telnet]",
                                "Disable output to specified target",
                                "System",
                                [](const std::vector<std::string>& args) -> std::string {
                                    if (args.size() < 2) {
                                        return std::string("Usage: disable [console|file|telnet]\r\n");
                                    }
                                    
                                    std::string target = args[1];
                                    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                                    
                                    LogConfig config = getConfig();
                                    
                                    if (target == "console") {
                                        config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::Console));
                                    } else if (target == "file") {
                                        config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::File));
                                    } else if (target == "telnet") {
                                        config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::Telnet));
                                    } else {
                                        return std::string("Unknown target: ") + target + "\r\n";
                                    }
                                    
                                    ::otl::log::updateConfig(config);
                                    
                                    return target + std::string(" output disabled\r\n");
                                });
            
            // Log command
            registerTelnetCommand("log", 
                                "log <message> [level]",
                                "Generate a log message at specified level (default: info)",
                                "System",
                                [](const std::vector<std::string>& args) -> std::string {
                                    if (args.size() < 2) {
                                        return std::string("Usage: log <message> [level]\r\n");
                                    }
                                    
                                    std::string message = args[1];
                                    LogLevel level = LogLevel::LOG_INFO;
                                    
                                    if (args.size() > 2) {
                                        level = LogLevelFromString(args[2]);
                                    }
                                    
                                    // 使用LogPrintf函数
                                    LogPrintf("TelnetCmd", level, "%s", message.c_str());
                                    
                                    std::string levelStr = ::otl::log::LogLevelToString(level);
                                    return std::string("Log message sent at level: ") + levelStr + "\r\n";
                                });
            
            // Quit command - handled specially in processTelnetCommand
            registerTelnetCommand("quit", 
                                "quit/exit/bye", 
                                "Close telnet connection", 
                                "System", 
                                nullptr);
            
            registered = true;
        }
    }
    
    // Initialize telnet server if enabled
    if (config.telnetConfig.enable) {
        startTelnetServer(config.telnetConfig.port, config.telnetConfig.maxConnections);
    }
    g_logger.worker = std::thread(log_worker);
}

void deinit() {
    if (g_logger.running) {
        g_logger.running = false;
        if (g_logger.queue) g_logger.queue->stop();
        if (g_logger.worker.joinable()) g_logger.worker.join();
        if (g_logger.fileStream.is_open()) g_logger.fileStream.close();
        // Telnet server stop
        stopTelnetServer();
    }
}

void updateConfig(const LogConfig& config) {
    std::lock_guard<std::mutex> lock(g_logger.fileMutex);
    g_logger.config = config;
    // File output update
    if (config.targets & OutputTarget::File) {
        if (!g_logger.fileStream.is_open()) {
            g_logger.fileStream.open(config.fileConfig.path, std::ios::app);
        }
    } else {
        if (g_logger.fileStream.is_open()) {
            g_logger.fileStream.close();
        }
    }
    // Telnet update
    if (config.targets & OutputTarget::Telnet) {
        if (config.telnetConfig.enable && !g_logger.telnetRunning) {
            startTelnetServer(config.telnetConfig.port, config.telnetConfig.maxConnections);
        } else if (!config.telnetConfig.enable && g_logger.telnetRunning) {
            stopTelnetServer();
        }
    } else if (g_logger.telnetRunning) {
        stopTelnetServer();
    }
}

LogConfig getConfig() {
    return g_logger.config;
}

LogStream::LogStream(const char* file, int line, LogLevel level, const std::string& moduleTag)
    : std::ostringstream(), m_file(file), m_line(line), m_level(level), m_moduleTag(moduleTag) {}

LogStream::~LogStream() {
    if (m_level < g_logger.config.level && m_level != LOG_FATAL) return;
    LogMessage msg;
    msg.timestamp = std::chrono::system_clock::now();
    msg.level = m_level;
    msg.moduleTag = m_moduleTag;
    msg.file = m_file;
    msg.line = m_line;
    msg.content = this->str();
    msg.pid = get_pid();
    msg.tid = get_tid();
    if (!g_logger.queue || !g_logger.queue->push(msg)) {
        // fallback to direct output
        // Format with colors for console, without colors for file
        if (g_logger.config.targets & OutputTarget::File) {
            std::string fileLine = format_log(msg, false); // no colors for file
            write_to_file(fileLine);
        }
        if (g_logger.config.targets & OutputTarget::Console) {
            if (g_logger.config.enableConsole) {
                std::string consoleLine = format_log(msg, true); // with colors for console
                write_to_console(consoleLine, msg.level);
            }
        }
        if (msg.level == LOG_FATAL && g_logger.config.abortOnFatal) {
            std::cerr << "Process aborted due to FATAL log." << std::endl;
            std::abort();
        }
    }
}

// printf-style log interface with variadic arguments
void LogPrintf(const std::string& moduleTag, LogLevel level, const char* fmt, ...) {
    if (level < g_logger.config.level && level != LOG_FATAL) return;
    
    constexpr size_t BUF_SIZE = 4096;
    char buf[BUF_SIZE];
    
    va_list args;
    va_start(args, fmt);
    
#if defined(_WIN32)
    int n = _vsnprintf_s(buf, BUF_SIZE, _TRUNCATE, fmt, args);
#else
    int n = vsnprintf(buf, BUF_SIZE, fmt, args);
#endif
    
    va_end(args);
    
    LogMessage msg;
    msg.timestamp = std::chrono::system_clock::now();
    msg.level = level;
    msg.moduleTag = moduleTag;
    msg.file = "";
    msg.line = 0;
    msg.content = std::string(buf, n > 0 ? n : 0);
    msg.pid = get_pid();
    msg.tid = get_tid();
    if (!g_logger.queue || !g_logger.queue->push(msg)) {
        // Format with colors for console, without colors for file
        if (g_logger.config.targets & OutputTarget::File) {
            std::string fileLine = format_log(msg, false); // no colors for file
            write_to_file(fileLine);
        }
        if (g_logger.config.targets & OutputTarget::Console) {
            if (g_logger.config.enableConsole) {
                std::string consoleLine = format_log(msg, true); // with colors for console
                write_to_console(consoleLine, msg.level);
            }
        }
        if (msg.level == LOG_FATAL && g_logger.config.abortOnFatal) {
            std::cerr << "Process aborted due to FATAL log." << std::endl;
            std::abort();
        }
    }
}

// Telnet命令测试函数实现
std::string processTelnetCommandForTest(const std::vector<std::string>& args) {
    if (args.empty()) {
        return "Error: No command specified";
    }

    std::string cmd = args[0];
    std::string response;
    bool handled = false;

    // 检查自定义注册的命令
    {
        std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
        auto it = g_logger.telnetCommands.find(cmd);
        if (it != g_logger.telnetCommands.end()) {
            assert(it->second.handler != nullptr);
            response = it->second.handler(args);
            handled = true;
        }
    }

    // 内置命令
    if (!handled) {
        if (cmd == "help") {
            response = "=== OTL Logger Telnet Console Help ===\r\n\r\n";
            response += "Built-in commands:\r\n";
            response += "  help               - Show this help menu\r\n";
            response += "  cmdshow [module]   - Show detailed command information, optionally filtered by module\r\n";
            response += "  quit/exit/bye      - Disconnect from server\r\n";
            response += "  status             - Show logger status (level, targets, file path, clients)\r\n";
            response += "  level [lvl]        - Get/Set log level (TRACE,DEBUG,INFO,WARNING,ERROR,FATAL)\r\n";
            response += "  enable <target>    - Enable output target (console,file,telnet)\r\n";
            response += "  disable <target>   - Disable output target (console,file,telnet)\r\n";
            response += "  log <message> [lvl]- Log a message with optional level (default: INFO)\r\n";
            
            // 列出按模块分组的自定义命令
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            if (!g_logger.telnetCommands.empty()) {
                // 按模块对命令进行分组
                std::map<std::string, std::vector<std::string>> moduleCommands;
                
                for (const auto& pair : g_logger.telnetCommands) {
                    moduleCommands[pair.second.module].push_back(pair.first);
                }
                
                response += "\r\nAvailable custom commands by module:\r\n";
                for (const auto& modulePair : moduleCommands) {
                    response += "  [" + (modulePair.first.empty() ? "General" : modulePair.first) + "]\r\n";
                    for (const auto& cmdName : modulePair.second) {
                        response += "    " + cmdName + "\r\n";
                    }
                }
                
                response += "\r\nUse 'cmdshow' for detailed command information\r\n";
            }
            handled = true;
        }
        else if (cmd == "cmdshow") {
            response = "=== OTL Logger Telnet Command Details ===\r\n\r\n";
            
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            if (g_logger.telnetCommands.empty()) {
                response += "No custom commands registered.\r\n";
            } else {
                std::string moduleFilter;
                if (args.size() > 1) {
                    moduleFilter = args[1];
                }
                
                // 按模块对命令分组
                std::map<std::string, std::vector<const TelnetCmdInfo*>> moduleCommands;
                
                for (const auto& pair : g_logger.telnetCommands) {
                    if (moduleFilter.empty() || pair.second.module == moduleFilter) {
                        moduleCommands[pair.second.module].push_back(&pair.second);
                    }
                }
                
                if (moduleCommands.empty() && !moduleFilter.empty()) {
                    response += "No commands found for module '" + moduleFilter + "'\r\n";
                } else {
                    for (const auto& modulePair : moduleCommands) {
                        response += "[Module: " + (modulePair.first.empty() ? "General" : modulePair.first) + "]\r\n";
                        for (const auto* cmdInfo : modulePair.second) {
                            response += "  Command: " + cmdInfo->name + "\r\n";
                            response += "    Format: " + cmdInfo->format + "\r\n";
                            response += "    Description: " + cmdInfo->description + "\r\n\r\n";
                        }
                    }
                }
            }
            handled = true;
        }
        else if (cmd == "status") {
            LogConfig config = getConfig();
            response = "Logger Status:\r\n";
            response += "  Current level: " + std::string(::otl::log::LogLevelToString(config.level)) + "\r\n";
            response += "  Enabled targets: ";
            if (config.targets & OutputTarget::Console) response += "console ";
            if (config.targets & OutputTarget::File) response += "file ";
            if (config.targets & OutputTarget::Telnet) response += "telnet ";
            if ((static_cast<uint32_t>(config.targets) & 0x7) == 0) response += "none";
            response += "\r\n";
            response += "  File path: " + config.fileConfig.path + "\r\n";
            response += "  Console enabled: " + std::string(config.enableConsole ? "yes" : "no") + "\r\n";
            response += "  Abort on fatal: " + std::string(config.abortOnFatal ? "yes" : "no") + "\r\n";
            response += "  Queue size: " + std::to_string(config.queueSize) + "\r\n";
            response += "  Telnet clients: " + std::to_string(g_logger.telnetClients.size()) + "\r\n";
            handled = true;
        }
        else if (cmd == "level") {
            LogConfig config = getConfig();
            if (args.size() < 2) {
                response = "Current log level: " + std::string(::otl::log::LogLevelToString(config.level)) + "\r\n";
            } else {
                LogLevel level = LogLevelFromString(args[1]);
                config.level = level;
                ::otl::log::updateConfig(config);
                response = "Log level set to: " + std::string(::otl::log::LogLevelToString(level)) + "\r\n";
            }
            handled = true;
        }
        else if (cmd == "enable") {
            if (args.size() < 2) {
                response = "Error: Missing target parameter. Usage: enable <console|file|telnet>\r\n";
            } else {
                LogConfig config = getConfig();
                std::string target = args[1];
                std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                
                if (target == "console") {
                    config.enableConsole = true;
                    response = "Console output enabled\r\n";
                }
                else if (target == "file") {
                    config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::File));
                    response = "File output enabled\r\n";
                }
                else if (target == "telnet") {
                    config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::Telnet));
                    response = "Telnet output enabled\r\n";
                }
                else {
                    response = "Unknown target: " + target + ". Valid targets: console, file, telnet\r\n";
                }
                
                if (response.find("Unknown") == std::string::npos) {
                    ::otl::log::updateConfig(config);
                }
            }
            handled = true;
        }
        else if (cmd == "disable") {
            if (args.size() < 2) {
                response = "Error: Missing target parameter. Usage: disable <console|file|telnet>\r\n";
            } else {
                LogConfig config = getConfig();
                std::string target = args[1];
                std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                
                if (target == "console") {
                    config.enableConsole = false;
                    response = "Console output disabled\r\n";
                }
                else if (target == "file") {
                    config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::File));
                    response = "File output disabled\r\n";
                }
                else if (target == "telnet") {
                    config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::Telnet));
                    response = "Telnet output disabled\r\n";
                }
                else {
                    response = "Unknown target: " + target + ". Valid targets: console, file, telnet\r\n";
                }
                
                if (response.find("Unknown") == std::string::npos) {
                    ::otl::log::updateConfig(config);
                }
            }
            handled = true;
        }
        else if (cmd == "log") {
            if (args.size() < 2) {
                response = "Error: Missing message. Usage: log <message> [level]\r\n";
            } else {
                std::string message = args[1];
                LogLevel level = LOG_INFO;  // 默认级别
                
                if (args.size() > 2) {
                    level = LogLevelFromString(args[2]);
                }
                
                // 使用LogPrintf函数
                otl::log::LogPrintf("TelnetCmd", level, "%s", message.c_str());
                
                std::string levelStr = ::otl::log::LogLevelToString(level);
                response = "Log message sent at level: " + levelStr + "\r\n";
            }
            handled = true;
        }
        else if (cmd == "quit" || cmd == "exit" || cmd == "bye") {
            // 在测试模式下的退出命令处理
            response = "Goodbye!\r\n";
            handled = true;
        }
        else {
            response = "Unknown command: " + cmd + ". Type 'help' for available commands.\r\n";
            handled = true;
        }
    }

    return response;
}

// LogLevelToString and LogLevelFromString implementations
const char* LogLevelToString(LogLevel level) {
    // Return full names for external API
    switch (level) {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO: return "INFO";
        case LOG_WARNING: return "WARNING";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

LogLevel LogLevelFromString(const std::string& str) {
    std::string upper = str;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "TRACE" || upper == "T") return LOG_TRACE;
    if (upper == "DEBUG" || upper == "D") return LOG_DEBUG;
    if (upper == "INFO" || upper == "I") return LOG_INFO;
    if (upper == "WARNING" || upper == "W" || upper == "WARN") return LOG_WARNING;
    if (upper == "ERROR" || upper == "E" || upper == "ERR") return LOG_ERROR;
    if (upper == "FATAL" || upper == "F") return LOG_FATAL;
    
    // 默认返回INFO级别
    return LOG_INFO;
}

typedef std::function<std::string(const std::vector<std::string>&)> TelnetCmdHandler;

// Telnet server implementation
namespace {

// Send welcome message to a telnet client
void sendTelnetWelcome(int clientSocket) {
    std::string welcome = "\r\n=== OTL Log Telnet Console ===\r\n";
    welcome += "Type 'help' for available commands\r\n";
    welcome += "Type 'quit' to disconnect\r\n\r\n";
    send(clientSocket, welcome.c_str(), welcome.length(), 0);
}

// Process telnet client input
void processTelnetCommand(int clientSocket, const std::string& cmdLine) {
    std::istringstream iss(cmdLine);
    std::string cmd;
    std::vector<std::string> args;
    
    iss >> cmd;
    std::string arg;
    while (iss >> arg) {
        args.push_back(arg);
    }
    
    // Convert command to lowercase for case-insensitive comparison
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    
    std::string response;
    bool handled = false;
    
    // Check custom registered commands
    {
        std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
        auto it = g_logger.telnetCommands.find(cmd);
        if (it != g_logger.telnetCommands.end()) {
            assert(it->second.handler != nullptr);
            response = it->second.handler(args);
            handled = true;
        }
    }
    
    // Built-in commands
    if (!handled) {
        if (cmd == "help") {
            response = "=== OTL Logger Telnet Console Help ===\r\n\r\n";
            response += "Built-in commands:\r\n";
            response += "  help               - Show this help menu\r\n";
            response += "  cmdshow [module]   - Show detailed command information, optionally filtered by module\r\n";
            response += "  quit/exit/bye      - Disconnect from server\r\n";
            response += "  status             - Show logger status (level, targets, file path, clients)\r\n";
            response += "  level [lvl]        - Get/Set log level (TRACE,DEBUG,INFO,WARNING,ERROR,FATAL)\r\n";
            response += "  enable <target>    - Enable output target (console,file,telnet)\r\n";
            response += "  disable <target>    - Disable output target (console,file,telnet)\r\n";
            response += "  log <message> [lvl]- Log a message with optional level (default: INFO)\r\n";
            
            // List custom commands grouped by module
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            if (!g_logger.telnetCommands.empty()) {
                // Group commands by module
                std::map<std::string, std::vector<std::string>> moduleCommands;
                
                for (const auto& pair : g_logger.telnetCommands) {
                    moduleCommands[pair.second.module].push_back(pair.first);
                }
                
                response += "\r\nAvailable custom commands by module:\r\n";
                for (const auto& modulePair : moduleCommands) {
                    response += "  [" + modulePair.first + "]\r\n";
                    for (const auto& cmdName : modulePair.second) {
                        response += "    " + cmdName + "\r\n";
                    }
                }
                
                response += "\r\nUse 'cmdshow' for detailed command information\r\n";
            }
            handled = true;
        }
        else if (cmd == "cmdshow") {
            response = "=== OTL Logger Telnet Command Details ===\r\n\r\n";
            
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            if (g_logger.telnetCommands.empty()) {
                response += "No custom commands registered.\r\n";
            } else {
                std::string moduleFilter;
                if (!args.empty()) {
                    moduleFilter = args[0];
                }
                
                // Group commands by module
                std::map<std::string, std::vector<const TelnetCmdInfo*>> moduleCommands;
                
                for (const auto& pair : g_logger.telnetCommands) {
                    if (moduleFilter.empty() || pair.second.module == moduleFilter) {
                        moduleCommands[pair.second.module].push_back(&pair.second);
                    }
                }
                
                if (moduleCommands.empty() && !moduleFilter.empty()) {
                    response += "No commands found for module '" + moduleFilter + "'\r\n";
                } else {
                    for (const auto& modulePair : moduleCommands) {
                        response += "[Module: " + modulePair.first + "]\r\n";
                        for (const auto* cmdInfo : modulePair.second) {
                            response += "  Command: " + cmdInfo->name + "\r\n";
                            response += "    Format: " + cmdInfo->format + "\r\n";
                            response += "    Description: " + cmdInfo->description + "\r\n\r\n";
                        }
                    }
                }
            }
            handled = true;
        }
        else if (cmd == "quit" || cmd == "exit" || cmd == "bye") {
            response = "Goodbye!\r\n";
            send(clientSocket, response.c_str(), response.length(), 0);
            
            // Remove client from the list and close socket
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            auto it = std::find(g_logger.telnetClients.begin(), g_logger.telnetClients.end(), clientSocket);
            if (it != g_logger.telnetClients.end()) {
                g_logger.telnetClients.erase(it);
            }
#ifdef _WIN32
            closesocket(clientSocket);
#else
            close(clientSocket);
#endif
            return;
        }
        else if (cmd == "status") {
            LogConfig config = getConfig();
            response = "Logger Status:\r\n";
            response += "  Current level: " + std::string(::otl::log::LogLevelToString(config.level)) + "\r\n";
            response += "  Enabled targets: ";
            if (config.targets & OutputTarget::Console) response += "console ";
            if (config.targets & OutputTarget::File) response += "file ";
            if (config.targets & OutputTarget::Telnet) response += "telnet ";
            if ((static_cast<uint32_t>(config.targets) & 0x7) == 0) response += "none";
            response += "\r\n";
            response += "  File path: " + config.fileConfig.path + "\r\n";
            response += "  Console enabled: " + std::string(config.enableConsole ? "yes" : "no") + "\r\n";
            response += "  Abort on fatal: " + std::string(config.abortOnFatal ? "yes" : "no") + "\r\n";
            response += "  Queue size: " + std::to_string(config.queueSize) + "\r\n";
            response += "  Telnet clients: " + std::to_string(g_logger.telnetClients.size()) + "\r\n";
            handled = true;
        }
        else if (cmd == "level") {
            LogConfig config = getConfig();
            if (args.empty()) {
                response = "Current log level: " + std::string(::otl::log::LogLevelToString(config.level)) + "\r\n";
            } else {
                LogLevel level = LogLevelFromString(args[0]);
                config.level = level;
                ::otl::log::updateConfig(config);
                response = "Log level set to: " + std::string(::otl::log::LogLevelToString(level)) + "\r\n";
            }
            handled = true;
        }
        else if (cmd == "enable") {
            LogConfig config = getConfig();
            if (args.empty()) {
                response = "Specify target to enable: console, file, or telnet\r\n";
            } else {
                std::string target = args[0];
                std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                
                if (target == "console") {
                    config.enableConsole = true;
                    response = "Console output enabled\r\n";
                }
                else if (target == "file") {
                    config.targets = config.targets | OutputTarget::File;
                    response = "File output enabled\r\n";
                }
                else if (target == "telnet") {
                    config.targets = config.targets | OutputTarget::Telnet;
                    response = "Telnet output enabled\r\n";
                }
                else {
                    response = "Unknown target: " + target + "\r\n";
                }
                ::otl::log::updateConfig(config);
            }
            handled = true;
        }
        else if (cmd == "disable") {
            LogConfig config = getConfig();
            if (args.empty()) {
                response = "Specify target to disable: console, file, or telnet\r\n";
            } else {
                std::string target = args[0];
                std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                
                if (target == "console") {
                    config.enableConsole = false;
                    response = "Console output disabled\r\n";
                }
                else if (target == "file") {
                    config.targets = static_cast<OutputTarget>(
                        static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(OutputTarget::File));
                    response = "File output disabled\r\n";
                }
                else if (target == "telnet") {
                    config.targets = static_cast<OutputTarget>(
                        static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(OutputTarget::Telnet));
                    response = "Telnet output disabled\r\n";
                }
                else {
                    response = "Unknown target: " + target + "\r\n";
                }
                ::otl::log::updateConfig(config);
            }
            handled = true;
        }
    }
    
    if (!handled) {
        response = "Unknown command: '" + cmd + "'. Type 'help' for available commands.\r\n";
    }
    
    // Add CRLF if not present
    if (!response.empty() && response.substr(response.length() - 2) != "\r\n") {
        response += "\r\n";
    }
    
    // Send response
    send(clientSocket, response.c_str(), response.length(), 0);
}

// Telnet client handler thread
void telnetClientHandler(int clientSocket) {
    // Send welcome message
    sendTelnetWelcome(clientSocket);
    
    // Process client commands
    char buffer[1024];
    std::string cmdBuffer;
    
    while (true) {
        // Prompt
        const char* prompt = "log> ";
        send(clientSocket, prompt, strlen(prompt), 0);
        
        // Read client input
        memset(buffer, 0, sizeof(buffer));
        int n = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        
        if (n <= 0) {
            // Client disconnected or error
            break;
        }
        
        // Process buffer
        for (int i = 0; i < n; i++) {
            char c = buffer[i];
            if (c == '\r') {
                // Ignore CR
            } else if (c == '\n') {
                // Process command on LF
                if (!cmdBuffer.empty()) {
                    processTelnetCommand(clientSocket, cmdBuffer);
                    cmdBuffer.clear();
                }
            } else {
                // Add to command buffer
                cmdBuffer += c;
            }
        }
    }
    
    // Client disconnected, remove from list
    std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
    auto it = std::find(g_logger.telnetClients.begin(), g_logger.telnetClients.end(), clientSocket);
    if (it != g_logger.telnetClients.end()) {
        g_logger.telnetClients.erase(it);
    }
    
#ifdef _WIN32
    closesocket(clientSocket);
#else
    close(clientSocket);
#endif
}

// Telnet server main thread
void telnetServerThread(uint16_t port, size_t maxConnections) {
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Failed to initialize Winsock" << std::endl;
        return;
    }
#endif
    
    // Create socket
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "Failed to create telnet server socket" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }
    
    // Set socket options for reuse
    int opt = 1;
#ifdef _WIN32
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
#else
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
        std::cerr << "Failed to set socket options" << std::endl;
#ifdef _WIN32
        closesocket(serverSocket);
        WSACleanup();
#else
        close(serverSocket);
#endif
        return;
    }
    
    // Bind
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(serverSocket, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Failed to bind telnet server to port " << port << std::endl;
#ifdef _WIN32
        closesocket(serverSocket);
        WSACleanup();
#else
        close(serverSocket);
#endif
        return;
    }
    
    // Listen
    if (listen(serverSocket, 5) < 0) {
        std::cerr << "Failed to listen on telnet server socket" << std::endl;
#ifdef _WIN32
        closesocket(serverSocket);
        WSACleanup();
#else
        close(serverSocket);
#endif
        return;
    }
    
    // Store server socket
    g_logger.telnetSocket = serverSocket;
    
    // Accept connections
    std::cout << "Telnet server listening on port " << port << std::endl;
    
    // Add built-in telnet commands
    registerTelnetCommand("log", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Usage: log <message> [level]\r\nLevels: TRACE, DEBUG, INFO, WARNING, ERROR, FATAL\r\n";
        }
        
        // Join args to form message, except possibly the last one for level
        std::string message;
        LogLevel level = LOG_INFO;
        
        if (args.size() > 1) {
            // Check if last arg is a valid log level
            std::string lastArg = args.back();
            std::transform(lastArg.begin(), lastArg.end(), lastArg.begin(), ::toupper);
            
            if (lastArg == "TRACE" || lastArg == "DEBUG" || lastArg == "INFO" ||
                lastArg == "WARNING" || lastArg == "ERROR" || lastArg == "FATAL") {
                level = LogLevelFromString(lastArg);
                
                // Join all args except the last
                for (size_t i = 0; i < args.size() - 1; ++i) {
                    if (i > 0) message += " ";
                    message += args[i];
                }
            } else {
                // Join all args
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i > 0) message += " ";
                    message += args[i];
                }
            }
        } else {
            message = args[0];
        }
        
        // Log the message
        ::otl::log::LogPrintf("Telnet", level, "%s", message.c_str());
        return "Message logged at " + std::string(::otl::log::LogLevelToString(level)) + " level\r\n";
    });
    
    while (g_logger.telnetRunning) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSocket, &readfds);
        
        // Use select with timeout to make the thread responsive to shutdown
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(serverSocket + 1, &readfds, NULL, NULL, &timeout);
        
        if (activity < 0) {
            // Error or interrupted
            if (g_logger.telnetRunning) {
                std::cerr << "Telnet server select error" << std::endl;
            }
            continue;
        }
        
        if (!g_logger.telnetRunning) {
            break;
        }
        
        if (activity == 0) {
            // Timeout, continue
            continue;
        }
        
        if (FD_ISSET(serverSocket, &readfds)) {
            // New connection
            struct sockaddr_in clientAddr;
#ifdef _WIN32
            int addrLen = sizeof(clientAddr);
#else
            socklen_t addrLen = sizeof(clientAddr);
#endif
            int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addrLen);
            
            if (clientSocket < 0) {
                std::cerr << "Failed to accept telnet connection" << std::endl;
                continue;
            }
            
            // Check max connections
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            if (g_logger.telnetClients.size() >= maxConnections) {
                const char* msg = "Too many connections. Try again later.\r\n";
                send(clientSocket, msg, strlen(msg), 0);
#ifdef _WIN32
                closesocket(clientSocket);
#else
                close(clientSocket);
#endif
                continue;
            }
            
            // Add to client list
            g_logger.telnetClients.push_back(clientSocket);
            
            // Start client handler thread
            std::thread(telnetClientHandler, clientSocket).detach();
        }
    }
    
    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
        for (auto client : g_logger.telnetClients) {
            if (client > 0) {
                const char* msg = "Server shutting down\r\n";
                send(client, msg, strlen(msg), 0);
#ifdef _WIN32
                closesocket(client);
#else
                close(client);
#endif
            }
        }
        g_logger.telnetClients.clear();
    }
    
    // Close server socket
    if (g_logger.telnetSocket >= 0) {
#ifdef _WIN32
        closesocket(g_logger.telnetSocket);
        WSACleanup();
#else
        close(g_logger.telnetSocket);
#endif
        g_logger.telnetSocket = -1;
    }
}

} // anonymous namespace

// Start telnet server
void startTelnetServer(uint16_t port, size_t maxConnections) {
    if (g_logger.telnetRunning) {
        return; // Already running
    }
    
    g_logger.telnetRunning = true;
    g_logger.telnetThread = std::thread(telnetServerThread, port, maxConnections);
}

// Stop telnet server
void stopTelnetServer() {
    if (!g_logger.telnetRunning) {
        return; // Not running
    }
    
    g_logger.telnetRunning = false;
    
    if (g_logger.telnetThread.joinable()) {
        g_logger.telnetThread.join();
    }
}

// 简化版本单独实现，避免递归调用
void registerTelnetCommand(const std::string& cmd, TelnetCmdHandler handler) {
    std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
    TelnetCmdInfo cmdInfo;
    cmdInfo.name = cmd;
    cmdInfo.format = cmd;
    cmdInfo.description = "No description available";
    cmdInfo.module = "Default";
    cmdInfo.handler = handler;
    g_logger.telnetCommands[cmd] = cmdInfo;
}

void registerTelnetCommand(const std::string& cmd, const std::string& format, 
                         const std::string& description, const std::string& module,
                         TelnetCmdHandler handler) {
    std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
    TelnetCmdInfo cmdInfo;
    cmdInfo.name = cmd;
    cmdInfo.format = format;
    cmdInfo.description = description;
    cmdInfo.module = module;
    cmdInfo.handler = handler;
    g_logger.telnetCommands[cmd] = cmdInfo;
}

// Register built-in telnet commands
namespace {
void registerBuiltinTelnetCommands() {
    static bool registered = false;
    if (registered) {
        return;
    }
    
    // Register status command
    registerTelnetCommand("status", "status",
        "Shows current logger status including log level, output targets, file path and connected clients",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            std::ostringstream oss;
            oss << "=== Logger Status ===\r\n";
            oss << "Log level: " << ::otl::log::LogLevelToString(g_logger.config.level) << "\r\n";
            oss << "Output targets: ";
            if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::Console)) != 0) oss << "console ";
            if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::File)) != 0) oss << "file ";
            if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::Telnet)) != 0) oss << "telnet ";
            oss << "\r\n";
            
            if ((static_cast<uint32_t>(g_logger.config.targets) & static_cast<uint32_t>(otl::log::OutputTarget::File)) != 0) {
                oss << "Log file: " << g_logger.config.fileConfig.path << "\r\n";
            }
            
            // Telnet clients count
            std::lock_guard<std::mutex> lock(g_logger.telnetMutex);
            oss << "Connected telnet clients: " << g_logger.telnetClients.size() << "\r\n";
            
            return oss.str();
        }
    );
    
    // Register level command
    registerTelnetCommand("level", "level [level]",
        "Get current log level or set new level (TRACE, DEBUG, INFO, WARNING, ERROR, FATAL)",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.empty()) {
                return "Current log level: " + std::string(LogLevelToString(g_logger.config.level)) + "\r\n";
            }
            
            LogLevel level = LogLevelFromString(args[0]);
            
            LogConfig config = getConfig();
            config.level = level;
            updateConfig(config);
            
            return "Log level set to " + std::string(LogLevelToString(level)) + "\r\n";
        }
    );
    
    // Register enable command
    registerTelnetCommand("enable", "enable <target>",
        "Enable output target (console, file, telnet)",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.empty()) {
                return "Usage: enable <target>\r\nWhere <target> is: console, file, or telnet\r\n";
            }
            
            std::string target = args[0];
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);
            
            LogConfig config = getConfig();
            
            if (target == "console") {
                config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::Console));
            } else if (target == "file") {
                config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::File));
            } else if (target == "telnet") {
                config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) | static_cast<uint32_t>(otl::log::OutputTarget::Telnet));
            } else {
                return "Unknown target: " + target + "\r\n";
            }
            
            updateConfig(config);
            return "Enabled output target: " + target + "\r\n";
        }
    );
    
    // Register disable command
    registerTelnetCommand("disable", "disable <target>",
        "Disable output target (console, file, telnet)",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.empty()) {
                return "Usage: disable <target>\r\nWhere <target> is: console, file, or telnet\r\n";
            }
            
            std::string target = args[0];
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);
            
            LogConfig config = getConfig();
            
            if (target == "console") {
                config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::Console));
            } else if (target == "file") {
                config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::File));
            } else if (target == "telnet") {
                config.targets = static_cast<otl::log::OutputTarget>(static_cast<uint32_t>(config.targets) & ~static_cast<uint32_t>(otl::log::OutputTarget::Telnet));
            } else {
                return "Unknown target: " + target + "\r\n";
            }
            
            updateConfig(config);
            return "Disabled output target: " + target + "\r\n";
        }
    );
    
    // Register log command
    registerTelnetCommand("log", "log <message> [level]",
        "Log a message with optional level (default: INFO)",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.empty()) {
                return "Usage: log <message> [level]\r\n";
            }
            
            LogLevel level = LOG_INFO;
            std::string message;
            
            if (args.size() > 1) {
                level = LogLevelFromString(args[args.size() - 1]);
                // Combine all but last arg as message
                for (size_t i = 0; i < args.size() - 1; ++i) {
                    if (i > 0) message += " ";
                    message += args[i];
                }
            } else {
                message = args[0];
            }
            
            // Log the message
            LogPrintf("Telnet", level, "%s", message.c_str());
            return "Message logged at " + std::string(LogLevelToString(level)) + " level\r\n";
        }
    );
    
    // Help and cmdshow are handled directly in the command processing code
    // But we still register them here so they show up in cmdshow
    registerTelnetCommand("help", "help",
        "Shows this help menu with all available commands",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            // This is a stub - actual implementation is in command processing code
            return "";
        }
    );
    
    registerTelnetCommand("cmdshow", "cmdshow [module]",
        "Shows detailed information about commands, optionally filtered by module",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            // This is a stub - actual implementation is in command processing code
            return "";
        }
    );
    
    registerTelnetCommand("quit", "quit/exit/bye",
        "Disconnect from the Telnet server",
        "System",
        [](const std::vector<std::string>& args) -> std::string {
            // This is a stub - actual implementation is in command processing code
            return "Goodbye!\r\n";
        }
    );
    
    registered = true;
}
}

} // namespace log
} // namespace otl
