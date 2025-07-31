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

    class TimerQueueImpl: public TimerQueue {
        int generateTimer(uint32_t delayMsec, std::function<void()> func, int repeat, uint64_t *pTimerId);
        std::unordered_map<uint64_t, TimerPtr> mMapTimers;
        MinHeap<TimerPtr> mQTimers;
        uint64_t mNTimerSN;
        std::mutex mMLock;
        bool mIsRunning;
        bool mStopped;

    public:
        TimerQueueImpl():mNTimerSN(0), mIsRunning(false), mStopped(false) {
            std::cout << "TimerQueueImpl ctor" << std::endl;
        }

        virtual ~TimerQueueImpl() {
            std::cout << "TimerQueueImpl dtor" << std::endl;
            stop();
        }

        virtual int createTimer(uint32_t delayMsec, uint32_t skew, std::function<void()> func, int repeat, uint64_t *pTimerId) override
        {
            RTC_RETURN_EXP_IF_FAIL(func != nullptr , return -1);
            TimerPtr timer = std::make_shared<Timer>();
            if (NULL == timer)
            {
                return -1;
            }

            timer->lamdaCb = func;
            timer->timeout = getTimeMsec() + skew;
            timer->delay_msec = delayMsec;
            timer->start_id = mNTimerSN ++;
            timer->repeat = repeat;
            {
                std::lock_guard<std::mutex> lock(mMLock);
                mMapTimers[timer->start_id] = timer;
                mQTimers.push(timer);
            }
            if (pTimerId) *pTimerId = timer->start_id;
            return 0;
        }

        virtual int deleteTimer(uint64_t timerId) override
        {
            std::lock_guard<std::mutex> lock(mMLock);
            auto it = mMapTimers.find(timerId);
            if (it != mMapTimers.end()) {
                mQTimers.remove(it->second);
                mMapTimers.erase(it);
                return 0;
            }
            return -1;
        }

        virtual size_t count() override
        {
            std::lock_guard<std::mutex> lock(mMLock);
            return mMapTimers.size();
        }

        virtual int runLoop() override
        {
            mIsRunning = true;
            mStopped = false;
            while (mIsRunning) {

                uint64_t now = getTimeMsec();
                mMLock.lock();
                if (mQTimers.empty()) {
                    mMLock.unlock();
                    msleep(1);
                    continue;
                }

                TimerPtr timer = mQTimers.top();
                if (timer->timeout > now) {
                    mMLock.unlock();
                    continue;
                }

                mQTimers.pop();
                if (timer->repeat) {
                    // readd it
                    timer->timeout = now + timer->delay_msec;
                    mQTimers.push(timer);
                }else {
                    auto it = mMapTimers.find(timer->start_id);
                    if (it != mMapTimers.end()) {
                        mMapTimers.erase(it);
                    }
                }
                mMLock.unlock();
                timer->lamdaCb();
            }

            std::cout << "rtc_timer_queue exit!" << std::endl;
            mStopped = true;
            return 1;
        }

        virtual int stop() override {
            mIsRunning = false;
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