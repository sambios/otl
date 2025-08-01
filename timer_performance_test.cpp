#include "otl_timer.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <atomic>
#include <thread>
#include <iomanip>
#include <mutex>

using namespace otl;

class TimerPerformanceTest {
private:
    std::atomic<int> m_callbackCount{0};
    std::atomic<int> m_accuracyTestCount{0};
    std::vector<int64_t> m_timingErrors;
    std::mutex m_errorMutex;
    
    uint64_t getCurrentTimeMs() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }

public:
    // 测试定时器精度
    void testTimingAccuracy() {
        std::cout << "\n=== 定时器精度测试 ===" << std::endl;
        
        auto timerQueue = TimerQueue::create();
        std::thread timerThread([&timerQueue]() {
            timerQueue->runLoop();
        });
        
        const int testCount = 50;
        const uint32_t intervalMs = 100; // 100ms间隔
        
        m_timingErrors.clear();
        m_accuracyTestCount = 0;
        
        uint64_t startTime = getCurrentTimeMs();
        
        // 创建多个定时器测试精度
        for (int i = 0; i < testCount; ++i) {
            uint64_t expectedTime = startTime + (i + 1) * intervalMs;
            
            uint64_t timerId;
            timerQueue->createTimer(intervalMs, intervalMs * (i + 1), [this, expectedTime]() {
                uint64_t actualTime = getCurrentTimeMs();
                int64_t error = static_cast<int64_t>(actualTime) - static_cast<int64_t>(expectedTime);
                
                {
                    std::lock_guard<std::mutex> lock(m_errorMutex);
                    m_timingErrors.push_back(error);
                }
                
                m_accuracyTestCount++;
            }, 0, &timerId);
        }
        
        // 等待所有定时器执行完成
        while (m_accuracyTestCount < testCount) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        timerQueue->stop();
        timerThread.join();
        
        // 计算统计信息
        if (!m_timingErrors.empty()) {
            int64_t totalError = 0;
            int64_t maxError = m_timingErrors[0];
            int64_t minError = m_timingErrors[0];
            
            for (int64_t error : m_timingErrors) {
                totalError += std::abs(error);
                maxError = std::max(maxError, error);
                minError = std::min(minError, error);
            }
            
            double avgError = static_cast<double>(totalError) / m_timingErrors.size();
            
            std::cout << "定时器精度统计:" << std::endl;
            std::cout << "  测试次数: " << m_timingErrors.size() << std::endl;
            std::cout << "  平均误差: " << std::fixed << std::setprecision(2) << avgError << "ms" << std::endl;
            std::cout << "  最大误差: " << maxError << "ms" << std::endl;
            std::cout << "  最小误差: " << minError << "ms" << std::endl;
        }
    }
    
    // 测试高负载下的性能
    void testHighLoadPerformance() {
        std::cout << "\n=== 高负载性能测试 ===" << std::endl;
        
        auto timerQueue = TimerQueue::create();
        std::thread timerThread([&timerQueue]() {
            timerQueue->runLoop();
        });
        
        const int timerCount = 1000;
        const uint32_t baseInterval = 10; // 10ms基础间隔
        
        m_callbackCount = 0;
        
        auto startTime = std::chrono::steady_clock::now();
        
        // 创建大量定时器
        std::vector<uint64_t> timerIds;
        for (int i = 0; i < timerCount; ++i) {
            uint64_t timerId;
            uint32_t interval = baseInterval + (i % 100); // 10-109ms的间隔
            
            timerQueue->createTimer(interval, interval, [this]() {
                m_callbackCount++;
            }, 5, &timerId); // 每个定时器重复5次
            
            timerIds.push_back(timerId);
        }
        
        auto createEndTime = std::chrono::steady_clock::now();
        auto createDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            createEndTime - startTime).count();
        
        std::cout << "创建 " << timerCount << " 个定时器耗时: " 
                  << createDuration << " 微秒" << std::endl;
        
        // 等待一段时间让定时器执行
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // 删除一半定时器测试删除性能
        auto deleteStartTime = std::chrono::steady_clock::now();
        for (size_t i = 0; i < timerIds.size() / 2; ++i) {
            timerQueue->deleteTimer(timerIds[i]);
        }
        auto deleteEndTime = std::chrono::steady_clock::now();
        auto deleteDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            deleteEndTime - deleteStartTime).count();
        
        std::cout << "删除 " << timerIds.size() / 2 << " 个定时器耗时: " 
                  << deleteDuration << " 微秒" << std::endl;
        
        // 再等待一段时间
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        timerQueue->stop();
        timerThread.join();
        
        std::cout << "总回调执行次数: " << m_callbackCount.load() << std::endl;
        std::cout << "剩余定时器数量: " << timerQueue->count() << std::endl;
    }
    
    // 测试CPU使用率
    void testCPUUsage() {
        std::cout << "\n=== CPU使用率测试 ===" << std::endl;
        std::cout << "请使用系统监控工具观察CPU使用率变化" << std::endl;
        
        auto timerQueue = TimerQueue::create();
        std::thread timerThread([&timerQueue]() {
            timerQueue->runLoop();
        });
        
        std::cout << "阶段1: 空闲状态 (5秒)" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        std::cout << "阶段2: 少量定时器 (5秒)" << std::endl;
        std::vector<uint64_t> timerIds;
        for (int i = 0; i < 10; ++i) {
            uint64_t timerId;
            timerQueue->createTimer(100, 100, []() {
                // 简单的回调
            }, -1, &timerId);
            timerIds.push_back(timerId);
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        std::cout << "阶段3: 大量定时器 (5秒)" << std::endl;
        for (int i = 0; i < 500; ++i) {
            uint64_t timerId;
            timerQueue->createTimer(50 + (i % 100), 50 + (i % 100), []() {
                // 简单的回调
            }, -1, &timerId);
            timerIds.push_back(timerId);
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        std::cout << "阶段4: 清理定时器 (2秒)" << std::endl;
        for (uint64_t timerId : timerIds) {
            timerQueue->deleteTimer(timerId);
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        timerQueue->stop();
        timerThread.join();
        
        std::cout << "CPU使用率测试完成" << std::endl;
    }
    
    // 运行所有测试
    void runAllTests() {
        std::cout << "开始定时器性能测试..." << std::endl;
        
        testTimingAccuracy();
        testHighLoadPerformance();
        testCPUUsage();
        
        std::cout << "\n所有测试完成!" << std::endl;
    }
};

int main() {
    TimerPerformanceTest test;
    test.runAllTests();
    return 0;
}
