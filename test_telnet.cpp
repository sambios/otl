#include "otl_log.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <string>

// 模拟模块日志函数
void moduleLogging(const std::string& moduleTag, int count) {
    for (int i = 0; i < count && i < 3; ++i) { // 限制最多执行3次循环
        OTL_LOG(otl::log::LOG_TRACE, moduleTag) << "TRACE message #" << i << " from " << moduleTag;
        OTL_LOG(otl::log::LOG_DEBUG, moduleTag) << "DEBUG message #" << i << " from " << moduleTag;
        OTL_LOG(otl::log::LOG_INFO, moduleTag) << "INFO message #" << i << " from " << moduleTag;
        OTL_LOG(otl::log::LOG_WARNING, moduleTag) << "WARNING message #" << i << " from " << moduleTag;
        OTL_LOG(otl::log::LOG_ERROR, moduleTag) << "ERROR message #" << i << " from " << moduleTag;
        
        // 缩短等待时间
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// 自定义Telnet命令处理程序
std::string handleEchoCommand(const std::vector<std::string>& args) {
    std::string result = "Echo: ";
    for (const auto& arg : args) {
        result += arg + " ";
    }
    return result;
}

// 自定义命令：打印系统时间
std::string handleTimeCommand(const std::vector<std::string>& args) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::string timeStr = std::ctime(&t);
    return "Current system time: " + timeStr;
}

// 模拟Telnet客户端，测试各种命令
void testTelnetCommands() {
    std::cout << "\n=== 开始Telnet命令自动化测试 ===\n" << std::endl;
    
    // 测试help命令
    std::cout << "测试 'help' 命令:" << std::endl;
    std::vector<std::string> helpArgs{"help"};
    std::string helpResult = otl::log::processTelnetCommandForTest(helpArgs);
    std::cout << helpResult << std::endl;
    
    // 测试cmdshow命令
    std::cout << "\n测试 'cmdshow' 命令:" << std::endl;
    std::vector<std::string> cmdshowArgs{"cmdshow"};
    std::string cmdshowResult = otl::log::processTelnetCommandForTest(cmdshowArgs);
    std::cout << cmdshowResult << std::endl;
    
    // 测试status命令
    std::cout << "\n测试 'status' 命令:" << std::endl;
    std::vector<std::string> statusArgs{"status"};
    std::string statusResult = otl::log::processTelnetCommandForTest(statusArgs);
    std::cout << statusResult << std::endl;
    
    // 测试level命令
    std::cout << "\n测试 'level' 命令 (设置为DEBUG):" << std::endl;
    std::vector<std::string> levelArgs{"level", "debug"};
    std::string levelResult = otl::log::processTelnetCommandForTest(levelArgs);
    std::cout << levelResult << std::endl;
    
    // 测试enable命令
    std::cout << "\n测试 'enable' 命令 (启用console):" << std::endl;
    std::vector<std::string> enableArgs{"enable", "console"};
    std::string enableResult = otl::log::processTelnetCommandForTest(enableArgs);
    std::cout << enableResult << std::endl;
    
    // 测试disable命令
    std::cout << "\n测试 'disable' 命令 (禁用telnet):" << std::endl;
    std::vector<std::string> disableArgs{"disable", "telnet"};
    std::string disableResult = otl::log::processTelnetCommandForTest(disableArgs);
    std::cout << disableResult << std::endl;
    
    // 测试log命令
    std::cout << "\n测试 'log' 命令:" << std::endl;
    std::vector<std::string> logArgs{"log", "这是一条通过Telnet命令发送的测试日志"};
    std::string logResult = otl::log::processTelnetCommandForTest(logArgs);
    std::cout << logResult << std::endl;
    
    // 测试自定义echo命令
    std::cout << "\n测试 'echo' 命令:" << std::endl;
    std::vector<std::string> echoArgs{"echo", "Hello", "World"};
    std::string echoResult = otl::log::processTelnetCommandForTest(echoArgs);
    std::cout << echoResult << std::endl;
    
    // 测试自定义time命令
    std::cout << "\n测试 'time' 命令:" << std::endl;
    std::vector<std::string> timeArgs{"time"};
    std::string timeResult = otl::log::processTelnetCommandForTest(timeArgs);
    std::cout << timeResult << std::endl;
    
    std::cout << "\n=== Telnet命令自动化测试完成 ===\n" << std::endl;
}

int main() {
    std::cout << "=== Telnet日志控制台测试 ===" << std::endl;
    
    // 配置日志系统
    otl::log::LogConfig config;
    config.level = otl::log::LOG_TRACE; // 允许所有级别的日志
    config.targets = otl::log::OutputTarget::Console | otl::log::OutputTarget::File | otl::log::OutputTarget::Telnet;
    config.fileConfig.path = "telnet_test.log";
    
    // 配置Telnet
    config.telnetConfig.enable = true;
    config.telnetConfig.port = 2323; // 标准的telnet测试端口
    config.telnetConfig.maxConnections = 5;
    
    // 初始化日志系统
    std::cout << "初始化日志系统..." << std::endl;
    otl::log::init(config);
    
    // 注册自定义Telnet命令
    otl::log::registerTelnetCommand("echo", handleEchoCommand);
    otl::log::registerTelnetCommand("time", handleTimeCommand);
    
    // 为内置命令注册处理函数，以支持自动化测试
    otl::log::TelnetCmdHandler helpHandler = [](const std::vector<std::string>& args) {
        std::string response = "=== OTL Logger Telnet Console Help ===\r\n\r\n";
        response += "Built-in commands:\r\n";
        response += "  help               - Show this help menu\r\n";
        response += "  cmdshow [module]   - Show detailed command information, optionally filtered by module\r\n";
        response += "  quit/exit/bye      - Disconnect from server\r\n";
        response += "  status             - Show logger status (level, targets, file path, clients)\r\n";
        response += "  level [lvl]        - Get/Set log level (TRACE,DEBUG,INFO,WARNING,ERROR,FATAL)\r\n";
        response += "  enable <target>    - Enable output target (console,file,telnet)\r\n";
        response += "  disable <target>   - Disable output target (console,file,telnet)\r\n";
        response += "  log <message> [lvl]- Log a message with optional level (default: INFO)\r\n";
        response += "  echo <message>     - Echo back the message\r\n";
        response += "  time               - Show current time\r\n";
        
        return response;
    };
    
    otl::log::TelnetCmdHandler cmdshowHandler = [](const std::vector<std::string>& args) {
        std::string response = "=== OTL Logger Telnet Command Details ===\r\n\r\n";
        if (args.size() > 1) {
            response += "Showing details for module: " + args[1] + "\r\n";
        }
        
        response += "Built-in commands:\r\n";
        response += "  help - Format: help - Show available commands\r\n";
        response += "  cmdshow - Format: cmdshow [module] - Show detailed command information\r\n";
        response += "  status - Format: status - Show logger status\r\n";
        response += "  level - Format: level [lvl] - Get/Set log level\r\n";
        response += "  enable - Format: enable <target> - Enable output target\r\n";
        response += "  disable - Format: disable <target> - Disable output target\r\n";
        response += "  log - Format: log <message> [lvl] - Log a message\r\n";
        response += "\r\nCustom commands:\r\n";
        response += "  echo - Format: echo <message> - Echo back the message\r\n";
        response += "  time - Format: time - Show current time\r\n";
        
        return response;
    };
    
    otl::log::TelnetCmdHandler statusHandler = [](const std::vector<std::string>& args) {
        otl::log::LogConfig config = otl::log::getConfig();
        std::string response = "Logger Status:\r\n";
        response += "  Current level: " + std::string(otl::log::LogLevelToString(config.level)) + "\r\n";
        response += "  Enabled targets: ";
        if (config.targets & otl::log::OutputTarget::Console) response += "console ";
        if (config.targets & otl::log::OutputTarget::File) response += "file ";
        if (config.targets & otl::log::OutputTarget::Telnet) response += "telnet ";
        if ((static_cast<uint32_t>(config.targets) & 0x7) == 0) response += "none";
        response += "\r\n";
        response += "  File path: " + config.fileConfig.path + "\r\n";
        response += "  Console enabled: " + std::string(config.enableConsole ? "yes" : "no") + "\r\n";
        response += "  Abort on fatal: " + std::string(config.abortOnFatal ? "yes" : "no") + "\r\n";
        response += "  Queue size: " + std::to_string(config.queueSize) + "\r\n";
        return response;
    };
    
    // 注册内置命令的处理函数
    otl::log::registerTelnetCommand("help", helpHandler);
    otl::log::registerTelnetCommand("cmdshow", cmdshowHandler);
    otl::log::registerTelnetCommand("status", statusHandler);
    
    std::cout << "Telnet服务器已在端口 " << config.telnetConfig.port << " 启动" << std::endl;
    std::cout << "请使用telnet客户端连接到 localhost:" << config.telnetConfig.port << std::endl;
    std::cout << "可用命令: help, status, level, enable, disable, echo, time, quit" << std::endl;
    
    // 开始生成日志
    std::cout << "开始生成测试日志..." << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "Main") << "Telnet测试程序已启动";

    // 执行Telnet命令自动化测试
    std::cout << "正在执行Telnet命令自动化测试..." << std::endl;
    testTelnetCommands();

    std::cout << "继续生成日志流..." << std::endl;

    // 启动多个模块的日志线程
    std::thread netThread(moduleLogging, "Network", 10);
    std::thread dbThread(moduleLogging, "Database", 10);
    std::thread uiThread(moduleLogging, "UI", 10);
    
    // 主线程也生成一些日志
    for (int i = 0; i < 3; ++i) { // 减少到3次
        OTL_LOG(otl::log::LOG_INFO, "Main") << "主线程日志消息 #" << i;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // 等待所有线程完成
    if (netThread.joinable()) netThread.join();
    if (dbThread.joinable()) dbThread.join();
    if (uiThread.joinable()) uiThread.join();
    
    // 测试日志级别的动态调整
    std::cout << "\n测试动态调整日志级别..." << std::endl;
    
    config = otl::log::getConfig();
    config.level = otl::log::LOG_WARNING;
    otl::log::updateConfig(config);
    
    OTL_LOG(otl::log::LOG_INFO, "Main") << "这条INFO消息不应该出现在控制台";
    OTL_LOG(otl::log::LOG_WARNING, "Main") << "但这条WARNING消息应该出现";
    OTL_LOG(otl::log::LOG_ERROR, "Main") << "ERROR消息也应该出现";
    
    // 最后的清理
    std::cout << "\n测试完成，停止Telnet服务器..." << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "Main") << "Telnet测试程序结束";
    
    // 短暂睡眠，让日志系统完成处理
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 关闭日志系统
    otl::log::deinit();
    
    std::cout << "测试完成，请检查telnet_test.log文件" << std::endl;
    return 0;
}
