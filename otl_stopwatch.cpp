#include "StopWatch.h"
#include <sys/stat.h>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <limits.h>



namespace otl{

Watch::Watch(){
    #ifdef COMPILEDWITHC11
    t0 = t1 = std::chrono::steady_clock::now();
    #else
    t0 = t1 = std::chrono::monotonic_clock::now();
    #endif
    count = 0;
    start_flag = 1;
    // auto result = new std::vector<std::pair<Dur, std::string>>;
}

Watch::~Watch(){}

void Watch::start(){
    #ifdef COMPILEDWITHC11
    t0 = t1 = std::chrono::steady_clock::now();
    #else
    t0 = t1 = std::chrono::monotonic_clock::now();
    #endif
    count = 0;
    start_flag = 1;
}

void Watch::stop(const std::string msg){
    if (start_flag == 0) {
        std::cout << "Error! Stop Watch not start, please call start() first!" << std::endl;
        return;
    }
    #ifdef COMPILEDWITHC11
    t2 = std::chrono::steady_clock::now();
    #else
    t2 = std::chrono::monotonic_clock::now();
    #endif

    start_flag = 0;
    count = 0;
    double period = std::chrono::duration_cast<std::chrono::duration<double, std::micro> >(t2 - t0).count();
    auto duration = std::chrono::duration_cast<std::chrono::duration<double, std::micro> >(t2 - t0);

    std::stringstream saved_msg;
    saved_msg << "@- " << msg << " T:"<< std::hex << std::this_thread::get_id();

    logs.push_back(TLog(TDuration(t0, duration), saved_msg.str()));

    if ((dump_flag & DUMP_ALL) || (dump_flag & DUMP_STOP_DURATION)){
        double t = std::chrono::time_point_cast<std::chrono::duration<double, std::micro>>(t0).time_since_epoch().count();
        std::cout << std::setw(20) << std::setprecision(4) << std::fixed << t << " us";
        std::cout << "  Duration: " << std::setw(12) <<std::fixed << std::setprecision(4) <<period << " us : ";
        std::cout << saved_msg.str() << std::endl;
    }
}

void Watch::multiStop(const std::string msg){
    #ifdef COMPILEDWITHC11
    t2 = std::chrono::steady_clock::now();
    #else
    t2 = std::chrono::monotonic_clock::now();
    #endif
    double period = std::chrono::duration_cast<std::chrono::duration<double, std::micro> >(t2 - t1).count();
    auto duration = std::chrono::duration_cast<std::chrono::duration<double, std::micro> >(t2 - t1);
    count++;

    std::stringstream saved_msg;
    saved_msg << "[" + std::to_string(count) + "] " << msg << " T:"<< std::hex << std::this_thread::get_id();

    logs.push_back(TLog(TDuration(t1, duration), saved_msg.str()));

    if (dump_flag & DUMP_ALL){
        double t = std::chrono::time_point_cast<std::chrono::duration<double, std::micro>>(t1).time_since_epoch().count();
        std::cout << std::setw(20) << std::setprecision(4) << std::fixed << t << " us";
        std::cout << "  Duration: " << std::setw(12) <<std::fixed << std::setprecision(4) << period << " us : ";
        std::cout << saved_msg.str() << std::endl;
    }

    #ifdef COMPILEDWITHC11
    t1 = std::chrono::steady_clock::now();
    #else
    t1 = std::chrono::monotonic_clock::now();
    #endif
}

std::unordered_map<std::string, finalResult_t>* Watch::calculate(){
    for (auto& log_it : logs){
        std::unordered_map<std::string, finalResult_t>::iterator res_it;
        if ((res_it = fresult.find(log_it.second)) != fresult.end()) {
            double dur = log_it.first.duration.count();
            res_it->second.updateSum(dur);
            res_it->second.updateMaxMin(dur);
            res_it->second.totolCount += 1;
        } else {  //new log
            double dur = log_it.first.duration.count();
            auto tmp = finalResult_t(dur);
            fresult.insert(std::make_pair(log_it.second, tmp));
        }
    }
    for (auto& it: fresult){
        it.second.updateAverage();
        //std::cout << it.first << std::endl;
    }
    return &fresult;
}

/*
   stop watch manager
*/
WatchMgr::WatchMgr(){
    t0 = std::chrono::steady_clock::now();
}

WatchMgr::~WatchMgr(){}

Watch* WatchMgr::applyWatch(std::string name, DumpFlag dump){
    // typedef std::chrono::duration<double, std::micro> Dur;
    auto * watch = new Watch();
    watchStack.insert(std::make_pair(name, watch));
    watch->dump_flag = dump;
    watch->start();
    return watch;
}

Watch* WatchMgr::getWatch(std::string name){
    std::unordered_map<std::string, Watch *>::iterator it;
    if ((it = watchStack.find(name)) != watchStack.end()){
        return it->second;
    } else {
        std::cout << "Error! No this watch: " << name << std::endl;
        dump_map(watchStack);
        exit (EXIT_FAILURE);
    }
}
void WatchMgr::printFinalResults(){
    // table header
    std::cout << std::setw(15) <<"      Average(us) " << \
    std::setw(15)<<"    Max(us)     " << \
    std::setw(15)<<"    Min(us)     " << \
    std::setw(8)<<"Counts  " << \
    std::setw(28)<<"    Name    " << \
    "   Comments" <<std::endl;

    for (auto& wm: watchStack){
        //std::cout << wm.first << std::endl;
        wm.second->calculate();

        for(auto& w : wm.second->fresult){
            std::cout << std::setw(15) << std::setprecision(4) << w.second.average << \
            std::setw(15) << std::setprecision(4) << w.second.max << \
            std::setw(15) << std::setprecision(4) << w.second.min << \
            std::setw(8) << std::dec << w.second.totolCount << "   " << \
            std::setw(28)<< wm.first << "   " <<w.first << std::endl;
        }
    }
}// WatchMgr::printFinalResults
void WatchMgr::dumpLogCsv(std::string fn){
    std::ofstream logfile;
    struct stat buffer;
    std::string name;
    if (getenv("USER") != NULL) {
        name = getenv("USER");}
    else {
        name="erdou";}

    time_t t = time(0);   // get time now
    struct tm * now = localtime( & t );
    char tbuf[80];
    strftime (tbuf,80,"%Y-%m-%d_%H_%M_%S",now);

    std::string newfn = fn + '_' + name + '@' + std::string(tbuf);

    if ( stat(newfn.c_str(), &buffer) == 0){
        srand(time(NULL));
        int count = rand()%10000;
        newfn = newfn + "_" + std::to_string(count);
    }
    logfile.open(newfn);
    if (logfile.is_open()){
        logfile << "time,end_time,duration,name,msg_type,thread_id,comments" << std::endl;
        for (auto& wm : watchStack){
            std::string name = wm.first;
            for (auto& e: wm.second->logs){
                double time = std::chrono::duration_cast<std::chrono::duration<double, std::micro> >(e.first.start_time - t0).count();
                double end_time = std::chrono::duration_cast<std::chrono::duration<double, std::micro> >(e.first.start_time - t0 + e.first.duration).count();
                double duration = std::chrono::duration_cast<std::chrono::duration<double, std::micro> >(e.first.duration).count();

                // time
                logfile << std::setw(20) << std::setprecision(4) << std::fixed << time << ",";
                // end_time
                logfile << std::setw(20) << std::setprecision(4) << std::fixed << end_time << ",";
                // duration
                logfile << std::setprecision(4) << std::fixed << duration << ",";
                // name
                logfile << name << ",";

                // msg_type
                char c = e.second.at(0);
                if (c == '[') { logfile << "multistop,";}
                else if (c == '@') { logfile << "stop,";}
                else { logfile << ',';}

                // thread_id
                std::size_t last_T = e.second.find_last_of("T:");
                if (last_T != e.second.npos) { logfile << e.second.substr(last_T+2) << ',';}
                else { logfile << ',';}

                // comments
                std::size_t first_space = e.second.find_first_of(' ');
                std::size_t last_space = e.second.find_last_of(" T:");
                if ((first_space != e.second.npos) && (last_space != e.second.npos)){
                    logfile << e.second.substr(first_space+1, last_space-5) << std::endl;
                } else { logfile << ' ';}

            }
        }
        logfile.close();
    }else {
        std::cout << "Open log file failed! " << fn << std::endl;
        return;
    }
}
}//namespace
