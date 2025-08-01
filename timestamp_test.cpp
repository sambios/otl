#include "timestamp_smoother.h"
#include <iostream>
#include <vector>
#include <random>

using namespace otl;

// 模拟各种问题时间戳的测试用例
class TimestampTestCase {
public:
    static void testNormalSequence() {
        std::cout << "\n=== 测试正常时间戳序列 ===" << std::endl;
        TimestampSmoother smoother;
        
        std::vector<int64_t> timestamps = {0, 3000, 6000, 9000, 12000, 15000};
        
        for (int64_t ts : timestamps) {
            AVPacket* pkt = av_packet_alloc();
            pkt->pts = pkt->dts = ts;
            
            std::cout << "输入: " << ts << " -> ";
            smoother.smoothTimestamp(pkt);
            std::cout << "输出: " << pkt->pts << std::endl;
            av_packet_free(&pkt);
        }
        
        smoother.printStatistics();
    }
    
    static void testJumpyTimestamps() {
        std::cout << "\n=== 测试跳跃时间戳 ===" << std::endl;
        TimestampSmoother smoother;
        
        // 模拟跳跃的时间戳：正常 -> 大跳跃 -> 正常
        std::vector<int64_t> timestamps = {0, 3000, 6000, 150000, 153000, 9000, 12000};
        
        for (int64_t ts : timestamps) {
            AVPacket* pkt = av_packet_alloc();
            pkt->pts = pkt->dts = ts;
            
            std::cout << "输入: " << ts << " -> ";
            smoother.smoothTimestamp(pkt);
            std::cout << "输出: " << pkt->pts << std::endl;
            av_packet_free(&pkt);
        }
        
        smoother.printStatistics();
    }
    
    static void testLoopingTimestamps() {
        std::cout << "\n=== 测试文件回环时间戳 ===" << std::endl;
        TimestampSmoother smoother;
        
        // 模拟文件回环：正常递增 -> 突然回到开始
        std::vector<int64_t> timestamps = {
            100000, 103000, 106000, 109000, 112000,  // 正常播放
            0, 3000, 6000, 9000,                     // 文件回环
            115000, 118000, 121000                   // 继续播放
        };
        
        for (int64_t ts : timestamps) {
            AVPacket* pkt = av_packet_alloc();
            pkt->pts = pkt->dts = ts;
            
            std::cout << "输入: " << ts << " -> ";
            smoother.smoothTimestamp(pkt);
            std::cout << "输出: " << pkt->pts << std::endl;
            av_packet_free(&pkt);
        }
        
        smoother.printStatistics();
    }
    
    static void testInvalidTimestamps() {
        std::cout << "\n=== 测试无效时间戳 ===" << std::endl;
        TimestampSmoother smoother;
        
        // 混合有效和无效时间戳
        std::vector<int64_t> timestamps = {
            0, AV_NOPTS_VALUE, 6000, AV_NOPTS_VALUE, 12000, 15000
        };
        
        for (int64_t ts : timestamps) {
            AVPacket* pkt = av_packet_alloc();
            pkt->pts = pkt->dts = ts;
            
            std::cout << "输入: " << (ts == AV_NOPTS_VALUE ? "NOPTS" : std::to_string(ts)) << " -> ";
            smoother.smoothTimestamp(pkt);
            std::cout << "输出: " << pkt->pts << std::endl;
            av_packet_free(&pkt);
        }
        
        smoother.printStatistics();
    }
    
    static void testRandomTimestamps() {
        std::cout << "\n=== 测试随机不准确时间戳 ===" << std::endl;
        TimestampSmoother smoother;
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> noise(-1000, 1000);
        
        int64_t baseTime = 0;
        for (int i = 0; i < 10; ++i) {
            // 基础时间戳 + 随机噪声
            int64_t noisyTimestamp = baseTime + noise(gen);
            
            AVPacket* pkt = av_packet_alloc();
            pkt->pts = pkt->dts = noisyTimestamp;
            
            std::cout << "输入: " << noisyTimestamp << " -> ";
            smoother.smoothTimestamp(pkt);
            std::cout << "输出: " << pkt->pts << std::endl;
            av_packet_free(&pkt);
            
            baseTime += 3000; // 期望的正常增量
        }
        
        smoother.printStatistics();
    }
    
    static void testCustomParameters() {
        std::cout << "\n=== 测试自定义平滑参数 ===" << std::endl;
        TimestampSmoother smoother;
        
        // 设置更激进的平滑参数
        smoother.setSmoothingParameters(0.3, 50000, 2000);
        
        std::vector<int64_t> timestamps = {0, 3000, 45000, 48000, 6000, 9000};
        
        for (int64_t ts : timestamps) {
            AVPacket* pkt = av_packet_alloc();
            pkt->pts = pkt->dts = ts;
            
            std::cout << "输入: " << ts << " -> ";
            smoother.smoothTimestamp(pkt);
            std::cout << "输出: " << pkt->pts << std::endl;
            av_packet_free(&pkt);
        }
        
        smoother.printStatistics();
    }
};

int main() {
    std::cout << "时间戳平滑器测试程序" << std::endl;
    std::cout << "=====================" << std::endl;
    
    TimestampTestCase::testNormalSequence();
    TimestampTestCase::testJumpyTimestamps();
    TimestampTestCase::testLoopingTimestamps();
    TimestampTestCase::testInvalidTimestamps();
    TimestampTestCase::testRandomTimestamps();
    TimestampTestCase::testCustomParameters();
    
    std::cout << "\n测试完成！" << std::endl;
    return 0;
}
