#include "otl_log.h"
#include <thread>
#include <vector>
#include <chrono>
#include <iostream>

// Test multiple threads writing logs
void thread_log_task(int thread_id) {
    for (int i = 0; i < 10; i++) {
        OTL_LOG(otl::log::LOG_INFO, "Thread") << "Thread " << thread_id 
            << " message " << i << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    // Initialize log with console and file output
    otl::log::LogConfig config;
    config.targets = otl::log::OutputTarget::Console | otl::log::OutputTarget::File;
    config.level = otl::log::LOG_DEBUG;
    config.enableConsole = true;
    config.abortOnFatal = false; // Don't abort in test
    config.fileConfig.path = "test_log.log";
    config.fileConfig.rollSizeMB = 10;
    config.fileConfig.maxFiles = 5;
    config.queueSize = 1000;
    
    std::cout << "Initializing log system..." << std::endl;
    otl::log::init(config);
    
    // Test different log levels
    std::cout << "\nTesting different log levels:" << std::endl;
    OTL_LOG(otl::log::LOG_TRACE, "Test") << "This is a TRACE message" << std::endl;
    OTL_LOG(otl::log::LOG_DEBUG, "Test") << "This is a DEBUG message" << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "Test") << "This is an INFO message" << std::endl;
    OTL_LOG(otl::log::LOG_WARNING, "Test") << "This is a WARNING message" << std::endl;
    OTL_LOG(otl::log::LOG_ERROR, "Test") << "This is an ERROR message" << std::endl;
    OTL_LOG(otl::log::LOG_FATAL, "Test") << "This is a FATAL message (with abortOnFatal=false)" << std::endl;
    
    // Test printf style
    std::cout << "\nTesting printf-style interface:" << std::endl;
    otl::log::LogPrintf("Test", otl::log::LOG_INFO, "Printf style message: %d", 123);
    otl::log::LogPrintf("Test", otl::log::LOG_WARNING, "Printf complex: %s %d %.2f", "test", 456, 3.14159);
    
    // Update log level and test filtering
    std::cout << "\nUpdating log level to WARNING:" << std::endl;
    config.level = otl::log::LOG_WARNING;
    otl::log::updateConfig(config);
    
    std::cout << "Testing log level filtering (only WARNING+ should appear):" << std::endl;
    OTL_LOG(otl::log::LOG_TRACE, "Test") << "TRACE should be filtered" << std::endl;
    OTL_LOG(otl::log::LOG_DEBUG, "Test") << "DEBUG should be filtered" << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "Test") << "INFO should be filtered" << std::endl;
    OTL_LOG(otl::log::LOG_WARNING, "Test") << "WARNING should appear" << std::endl;
    OTL_LOG(otl::log::LOG_ERROR, "Test") << "ERROR should appear" << std::endl;
    OTL_LOG(otl::log::LOG_FATAL, "Test") << "FATAL should always appear" << std::endl;
    
    // Update back to DEBUG
    config.level = otl::log::LOG_DEBUG;
    otl::log::updateConfig(config);
    
    // Test multi-threading
    std::cout << "\nTesting multi-threading with 5 threads:" << std::endl;
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back(thread_log_task, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Test disable console output
    std::cout << "\nTesting console output disable:" << std::endl;
    config.enableConsole = false;
    otl::log::updateConfig(config);
    OTL_LOG(otl::log::LOG_INFO, "Test") << "This should NOT appear in console but still in file" << std::endl;
    
    // Re-enable console
    config.enableConsole = true;
    otl::log::updateConfig(config);
    OTL_LOG(otl::log::LOG_INFO, "Test") << "Console output re-enabled" << std::endl;
    
    // Test module tag
    std::cout << "\nTesting different module tags:" << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "Network") << "Network module message" << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "Storage") << "Storage module message" << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "UI") << "UI module message" << std::endl;
    
    // Get and print current config
    otl::log::LogConfig currentConfig = otl::log::getConfig();
    std::cout << "\nCurrent log level: " << otl::log::LogLevelToString(currentConfig.level) << std::endl;
    std::cout << "Log file path: " << currentConfig.fileConfig.path << std::endl;
    
    // Clean up
    std::cout << "\nDeinitializing log system..." << std::endl;
    OTL_LOG(otl::log::LOG_INFO, "Test") << "Final log message before shutdown" << std::endl;
    otl::log::deinit();
    
    std::cout << "Test completed. Check test_log.log for file output." << std::endl;
    return 0;
}
