//
// 优化的 TimerQueue 实现
// 主要改进：
// 1. 使用条件变量替代忙等待，减少CPU消耗
// 2. 精确计算睡眠时间，提高定时精度
// 3. 减少锁的持有时间，降低锁竞争
// 4. 优化数据结构，提高删除操作效率
//

#include "otl_timer.h"
#include <vector>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <assert.h>
#include <memory.h>
#include <atomic>

namespace otl {

// 优化的定时器结构
struct OptimizedTimer {
    std::function<void()> callback;
    uint64_t nextTimeout;     // 下次触发时间
    uint64_t intervalMsec;    // 重复间隔
    int repeatCount;          // 重复次数 (-1表示无限重复)
    uint64_t timerId;         // 定时器ID
    bool isValid;             // 是否有效（用于标记删除）
    
    OptimizedTimer() : isValid(true) {}
};

using OptimizedTimerPtr = std::shared_ptr<OptimizedTimer>;

// 定时器比较器（最小堆）
struct OptimizedTimerComp {
    bool operator()(const OptimizedTimerPtr& a, const OptimizedTimerPtr& b) const {
        return a->nextTimeout > b->nextTimeout;
    }
};

class OptimizedTimerQueueImpl : public TimerQueue {
private:
    // 使用优先队列作为最小堆
    std::priority_queue<OptimizedTimerPtr, std::vector<OptimizedTimerPtr>, OptimizedTimerComp> m_timerHeap;
    
    // 定时器映射表，用于快速查找和删除
    std::unordered_map<uint64_t, OptimizedTimerPtr> m_timerMap;
    
    // 同步原语
    std::mutex m_mutex;
    std::condition_variable m_condition;
    
    // 运行状态
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopped{true};
    
    // 定时器ID生成器
    std::atomic<uint64_t> m_nextTimerId{1};
    
    // 获取当前时间（毫秒）
    uint64_t getCurrentTimeMs() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }
    
    // 清理无效的定时器（在堆顶）
    void cleanupInvalidTimers() {
        while (!m_timerHeap.empty() && !m_timerHeap.top()->isValid) {
            m_timerHeap.pop();
        }
    }

public:
    OptimizedTimerQueueImpl() {
        std::cout << "OptimizedTimerQueueImpl ctor" << std::endl;
    }
    
    virtual ~OptimizedTimerQueueImpl() {
        std::cout << "OptimizedTimerQueueImpl dtor" << std::endl;
        stop();
    }
    
    virtual int createTimer(uint32_t delayMsec, uint32_t skew, 
                           std::function<void()> func, int repeat, 
                           uint64_t* pTimerId) override {
        if (!func) {
            return -1;
        }
        
        auto timer = std::make_shared<OptimizedTimer>();
        timer->callback = std::move(func);
        timer->nextTimeout = getCurrentTimeMs() + skew;
        timer->intervalMsec = delayMsec;
        timer->repeatCount = repeat;
        timer->timerId = m_nextTimerId.fetch_add(1);
        timer->isValid = true;
        
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_timerMap[timer->timerId] = timer;
            m_timerHeap.push(timer);
        }
        
        // 通知运行线程有新的定时器
        m_condition.notify_one();
        
        if (pTimerId) {
            *pTimerId = timer->timerId;
        }
        
        return 0;
    }
    
    virtual int deleteTimer(uint64_t timerId) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto it = m_timerMap.find(timerId);
        if (it != m_timerMap.end()) {
            // 标记为无效，而不是立即从堆中删除
            it->second->isValid = false;
            m_timerMap.erase(it);
            return 0;
        }
        
        return -1;
    }
    
    virtual size_t count() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_timerMap.size();
    }
    
    virtual int runLoop() override {
        m_running = true;
        m_stopped = false;
        
        std::cout << "OptimizedTimerQueue runLoop started" << std::endl;
        
        while (m_running) {
            std::unique_lock<std::mutex> lock(m_mutex);
            
            // 清理无效的定时器
            cleanupInvalidTimers();
            
            if (m_timerHeap.empty()) {
                // 没有定时器，等待新的定时器添加
                m_condition.wait(lock, [this] { 
                    return !m_running || !m_timerHeap.empty(); 
                });
                continue;
            }
            
            auto nextTimer = m_timerHeap.top();
            uint64_t currentTime = getCurrentTimeMs();
            
            if (nextTimer->nextTimeout > currentTime) {
                // 计算精确的等待时间
                uint64_t waitTime = nextTimer->nextTimeout - currentTime;
                
                // 等待指定时间或直到有新的定时器添加
                auto waitResult = m_condition.wait_for(lock, 
                    std::chrono::milliseconds(waitTime),
                    [this] { return !m_running; });
                
                if (!m_running) {
                    break;
                }
                
                // 重新检查时间，可能有新的更早的定时器
                continue;
            }
            
            // 定时器到期，执行回调
            m_timerHeap.pop();
            
            // 检查是否需要重新调度
            bool needReschedule = false;
            if (nextTimer->isValid) {
                if (nextTimer->repeatCount == -1) {
                    // 无限重复
                    needReschedule = true;
                } else if (nextTimer->repeatCount > 0) {
                    // 有限重复
                    nextTimer->repeatCount--;
                    needReschedule = (nextTimer->repeatCount > 0);
                }
                
                if (needReschedule) {
                    nextTimer->nextTimeout = currentTime + nextTimer->intervalMsec;
                    m_timerHeap.push(nextTimer);
                } else {
                    // 定时器完成，从映射表中删除
                    m_timerMap.erase(nextTimer->timerId);
                }
            }
            
            // 释放锁后执行回调，避免回调中的长时间操作阻塞定时器
            lock.unlock();
            
            if (nextTimer->isValid) {
                try {
                    nextTimer->callback();
                } catch (const std::exception& e) {
                    std::cerr << "Timer callback exception: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Timer callback unknown exception" << std::endl;
                }
            }
        }
        
        std::cout << "OptimizedTimerQueue runLoop exit" << std::endl;
        m_stopped = true;
        return 0;
    }
    
    virtual int stop() override {
        m_running = false;
        m_condition.notify_all();
        return 0;
    }
};

// 工厂函数 - 创建优化版本的TimerQueue
std::shared_ptr<TimerQueue> createOptimizedTimerQueue() {
    return std::make_shared<OptimizedTimerQueueImpl>();
}

} // namespace otl
