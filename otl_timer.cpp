//
// Created by yuan on 2/26/21.
//


#include "otl_timer.h"
#include <vector>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <assert.h>
#include <memory.h>
#include <condition_variable>
#include <atomic>

namespace otl {

#define RTC_RETURN_EXP_IF_FAIL(cond, exp) if (!(cond)) { fprintf(stderr, "Assert failed: %s in %s:%d\n", #cond, __FUNCTION__, __LINE__); exp;}

#define rtc_container_of(ptr, type, member)  ((type *) ((char *) (ptr) - offsetof(type, member)))
    uint64_t getTimeMsec() {
        auto tnow = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(tnow.time_since_epoch()).count();
    }

    uint64_t getTimeUsec() {
        auto tnow = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(tnow.time_since_epoch()).count();
    }

    uint64_t getTimeSec() {
        auto tnow = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(tnow.time_since_epoch()).count();
    }



    void msleep(int msec) {
        std::this_thread::sleep_for(std::chrono::milliseconds(msec));
    }

    void usleep(int usec) {
        std::this_thread::sleep_for(std::chrono::microseconds(usec));
    }

    std::string timeToString(time_t sec) {
        struct tm* tm1;
        tm1 = std::localtime(&sec);
        char strtmp[64];
        sprintf(strtmp, "%d-%.2d-%.2d:%.2d:%.2d:%.2d", tm1->tm_year+1900,
                tm1->tm_mon+1, tm1->tm_mday, tm1->tm_hour,
                tm1->tm_min, tm1->tm_sec);
        return strtmp;
    }

    struct Timer {
        std::function<void()> lamdaCb;
        uint64_t timeout;
        uint64_t delay_msec;
        int repeat;
        uint64_t start_id;
    };

    using TimerPtr=std::shared_ptr<Timer>;

    class TimerComp {
    public:
        bool operator()(const TimerPtr &a, const TimerPtr &b)
        {
            return a->timeout > b->timeout;
        }
    };

    template<typename T>
    class MinHeap : public std::priority_queue<T, std::vector<T>, TimerComp> {
    public:
        bool remove(const T &value) {
            auto it = std::find(this->c.begin(), this->c.end(), value);
            if (it != this->c.end()) {
                this->c.erase(it);
                std::make_heap(this->c.begin(), this->c.end(), this->comp);
                return true;
            } else {
                return false;
            }
        }
    };

    // �Ż��Ķ�ʱ���ṹ
    struct OptimizedTimer {
        std::function<void()> callback;
        uint64_t nextTimeout;     // �´δ���ʱ��
        uint64_t intervalMsec;    // �ظ����
        int repeatCount;          // �ظ����� (-1��ʾ�����ظ�)
        uint64_t timerId;         // ��ʱ��ID
        bool isValid;             // �Ƿ���Ч�����ڱ��ɾ����
        
        OptimizedTimer() : isValid(true) {}
    };

    using OptimizedTimerPtr = std::shared_ptr<OptimizedTimer>;

    // ��ʱ���Ƚ�������С�ѣ�
    struct OptimizedTimerComp {
        bool operator()(const OptimizedTimerPtr& a, const OptimizedTimerPtr& b) const {
            return a->nextTimeout > b->nextTimeout;
        }
    };

    class TimerQueueImpl : public TimerQueue {
    private:
        // ʹ�����ȶ�����Ϊ��С��
        std::priority_queue<OptimizedTimerPtr, std::vector<OptimizedTimerPtr>, OptimizedTimerComp> m_timerHeap;
        
        // ��ʱ��ӳ������ڿ��ٲ��Һ�ɾ��
        std::unordered_map<uint64_t, OptimizedTimerPtr> m_timerMap;
        
        // ͬ��ԭ��
        std::mutex m_mutex;
        std::condition_variable m_condition;
        
        // ����״̬
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopped{true};
        
        // ��ʱ��ID������
        std::atomic<uint64_t> m_nextTimerId{1};
        
        // ������Ч�Ķ�ʱ�����ڶѶ���
        void cleanupInvalidTimers() {
            while (!m_timerHeap.empty() && !m_timerHeap.top()->isValid) {
                m_timerHeap.pop();
            }
        }

    public:
        TimerQueueImpl() {
            std::cout << "TimerQueueImpl ctor (optimized)" << std::endl;
        }

        virtual ~TimerQueueImpl() {
            std::cout << "TimerQueueImpl dtor (optimized)" << std::endl;
            stop();
        }

        virtual int createTimer(uint32_t delayMsec, uint32_t skew, std::function<void()> func, int repeat, uint64_t *pTimerId) override
        {
            if (!func) {
                return -1;
            }
            
            auto timer = std::make_shared<OptimizedTimer>();
            timer->callback = std::move(func);
            timer->nextTimeout = getTimeMsec() + skew;
            timer->intervalMsec = delayMsec;
            timer->repeatCount = repeat;
            timer->timerId = m_nextTimerId.fetch_add(1);
            timer->isValid = true;
            
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_timerMap[timer->timerId] = timer;
                m_timerHeap.push(timer);
            }
            
            // ֪ͨ�����߳����µĶ�ʱ��
            m_condition.notify_one();
            
            if (pTimerId) {
                *pTimerId = timer->timerId;
            }
            
            return 0;
        }

        virtual int deleteTimer(uint64_t timerId) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            
            auto it = m_timerMap.find(timerId);
            if (it != m_timerMap.end()) {
                // ���Ϊ��Ч�������������Ӷ���ɾ��
                it->second->isValid = false;
                m_timerMap.erase(it);
                return 0;
            }
            
            return -1;
        }

        virtual size_t count() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_timerMap.size();
        }

        virtual int runLoop() override
        {
            m_running = true;
            m_stopped = false;
            
            std::cout << "TimerQueue runLoop started (optimized)" << std::endl;
            
            while (m_running) {
                std::unique_lock<std::mutex> lock(m_mutex);
                
                // ������Ч�Ķ�ʱ��
                cleanupInvalidTimers();
                
                if (m_timerHeap.empty()) {
                    // û�ж�ʱ�����ȴ��µĶ�ʱ�����
                    m_condition.wait(lock, [this] { 
                        return !m_running || !m_timerHeap.empty(); 
                    });
                    continue;
                }
                
                auto nextTimer = m_timerHeap.top();
                uint64_t currentTime = getTimeMsec();
                
                if (nextTimer->nextTimeout > currentTime) {
                    // ���㾫ȷ�ĵȴ�ʱ��
                    uint64_t waitTime = nextTimer->nextTimeout - currentTime;
                    
                    // �ȴ�ָ��ʱ���ֱ�����µĶ�ʱ�����
                    auto waitResult = m_condition.wait_for(lock, 
                        std::chrono::milliseconds(waitTime),
                        [this] { return !m_running; });
                    
                    if (!m_running) {
                        break;
                    }
                    
                    // ���¼��ʱ�䣬�������µĸ���Ķ�ʱ��
                    continue;
                }
                
                // ��ʱ�����ڣ�ִ�лص�
                m_timerHeap.pop();
                
                // ����Ƿ���Ҫ���µ���
                bool needReschedule = false;
                if (nextTimer->isValid) {
                    if (nextTimer->repeatCount == -1) {
                        // �����ظ�
                        needReschedule = true;
                    } else if (nextTimer->repeatCount > 0) {
                        // �����ظ�
                        nextTimer->repeatCount--;
                        needReschedule = (nextTimer->repeatCount > 0);
                    }
                    
                    if (needReschedule) {
                        nextTimer->nextTimeout = currentTime + nextTimer->intervalMsec;
                        m_timerHeap.push(nextTimer);
                    } else {
                        // ��ʱ����ɣ���ӳ�����ɾ��
                        m_timerMap.erase(nextTimer->timerId);
                    }
                }
                
                // �ͷ�����ִ�лص�������ص��еĳ�ʱ�����������ʱ��
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
            
            std::cout << "TimerQueue runLoop exit (optimized)" << std::endl;
            m_stopped = true;
            return 0;
        }

        virtual int stop() override {
            m_running = false;
            m_condition.notify_all();
            return 0;
        }
    };

    std::shared_ptr<TimerQueue> TimerQueue::create() {
        return std::make_shared<TimerQueueImpl>();
    }


    class StatToolImpl: public StatTool {
        struct StatisLayer {
            uint64_t bytes;
            uint64_t timeMsec;
            StatisLayer() :bytes(0),timeMsec(0){}
        };
        StatisLayer *mLayers;
        int mCurrentIndex;
        uint32_t mTotalLayers;
        uint32_t mRecordCount{0};
        int64_t mStatisCount{0};
        int64_t mStatisUpdateLastTime;

    public:
        StatToolImpl(int range=5):mCurrentIndex(0),mRecordCount(0) {
            mTotalLayers = range;
            mLayers = new StatisLayer[range];
            assert(NULL != mLayers);
        }
        virtual ~StatToolImpl(){
            delete []mLayers;
        };

        virtual void update(uint64_t currentStatis) override {
            mStatisCount += currentStatis;
            auto now = getTimeMsec();
            if (mStatisUpdateLastTime > 0 && now - mStatisUpdateLastTime < 1000) {
                return;
            }

            mStatisUpdateLastTime = now;
            uint32_t currentIndex = mCurrentIndex;
            mLayers[currentIndex].timeMsec = now;
            mLayers[currentIndex].bytes = mStatisCount;

            currentIndex = (currentIndex+1) % mTotalLayers;
            mCurrentIndex = currentIndex;

            if (mRecordCount < mTotalLayers)
            {
                mRecordCount++;
            }
        }
        virtual void reset() override {
            mCurrentIndex = 0;
            mRecordCount = 0;

            memset(mLayers, 0, sizeof(mLayers[0]) * mTotalLayers);
        }

        virtual double getkbps() override {
            return getSpeed()*8*0.001;
        }

        virtual double getSpeed() override {
            uint32_t currentIndex = 0;
            uint32_t newest, oldest;
            double bps = 0.0;
            uint64_t timeDiff = 0, byteDiff;

            currentIndex = mCurrentIndex;
            if (mRecordCount < mTotalLayers)
            {
                newest = currentIndex > 0 ? currentIndex - 1 : 0;
                oldest = 0;
            }
            else
            {
                newest = (mTotalLayers + (currentIndex - 1)) % mTotalLayers;
                oldest = currentIndex;
            }


            timeDiff = mLayers[newest].timeMsec - mLayers[oldest].timeMsec;
            byteDiff = mLayers[newest].bytes - mLayers[oldest].bytes;

            bps = (double)(byteDiff) * 1000 / (timeDiff);
            return bps;
        }
    };

    std::shared_ptr<StatTool> StatTool::create(int range) {
        return std::make_shared<StatToolImpl>(range);
    }

} // namespace otl