#ifndef STOPWATCH_H
#define STOPWATCH_H

#include <unistd.h>
#include<string>
#include <chrono>
#include <vector>
#include <map>
#include <unordered_map>

#include <iostream>     // std::cout, std::fixed
#include <iomanip>                // std::setprecision


namespace otl{

enum DumpFlag
{
    DUMP_NONE = 0,
    DUMP_STOP_DURATION = 0x1,
    DUMP_ALL = 0x8
};

struct TDuration {
    std::chrono::steady_clock::time_point start_time;
    std::chrono::duration<double, std::micro> duration;
    TDuration(std::chrono::steady_clock::time_point t, std::chrono::duration<double, std::micro> d){
        start_time = t;
        duration = d;
    }
};
// typedef std::pair<std::chrono::steady_clock::time_point,std::chrono::duration<double, std::micro>> TDuration;
typedef std::pair<TDuration, std::string> TLog;
struct finalResult_t{
    double sumPeriod;
    double max;
    double min;
    double average;
    uint totolCount;
    finalResult_t(double initValue) : sumPeriod(initValue),
        max(initValue), min(initValue), average(0), totolCount(1){ }
    bool updateAverage(){
        if (totolCount > 0) {
            average = sumPeriod / totolCount;
        } else {
            std::cout<< "Error!! Average calculation: divide by zero!" << std::endl;
            return false;
        }
        return true;
    }
    void updateMaxMin(double value){
        max = max > value ? max : value;
        min = min < value ? min : value;
    }
    void updateSum(double value){
        sumPeriod += value;
    }
};


class Watch{
    public:
        Watch();
        virtual ~Watch();
        void start();
        void stop(const std::string msg = "");
        void multiStop(const std::string msg = "");
        std::unordered_map<std::string, finalResult_t>* calculate();

        std::vector<TLog> logs;
        std::unordered_map<std::string, finalResult_t>  fresult;
        DumpFlag dump_flag;

    private:
        #ifdef COMPILEDWITHC11
        std::chrono::steady_clock::time_point t0;
        std::chrono::steady_clock::time_point t1;
        std::chrono::steady_clock::time_point t2;
        #else
        std::chrono::monotonic_clock::time_point t0;
        std::chrono::monotonic_clock::time_point t1;
        std::chrono::monotonic_clock::time_point t2;
        #endif
        std::uint32_t count;
        std::uint32_t start_flag;
};   // class Watch;

class WatchMgr
{
public:
    WatchMgr();
    virtual ~WatchMgr();
    Watch* applyWatch(std::string name, DumpFlag dump=DUMP_ALL);
    Watch* getWatch(std::string name);
    void printFinalResults();
    void dumpLogCsv(std::string fn);
    //deleteWatch(std::string name);
    //printResult();
    std::unordered_map<std::string, Watch *> watchStack;


        #ifdef COMPILEDWITHC11
        std::chrono::steady_clock::time_point t0;
        #else
        std::chrono::monotonic_clock::time_point t0;
        #endif


}; // class


template<typename K, typename V>
void dump_map(std::unordered_map<K,V> const &m)
{
        for (auto const& pair: m) {
                std::cout << "{" << pair.first << ": " << pair.second << "}\n";
        }
};

}
#endif //STOPWATCH_H