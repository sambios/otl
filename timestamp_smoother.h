#ifndef TIMESTAMP_SMOOTHER_H
#define TIMESTAMP_SMOOTHER_H

#include <deque>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
}

namespace otl {

class TimestampSmoother {
private:
    // 时间戳历史记录，用于平滑计算
    std::deque<int64_t> m_timestampHistory;
    static const size_t MAX_HISTORY_SIZE = 10;
    
    // 当前输出时间戳状态
    int64_t m_lastOutputPts{AV_NOPTS_VALUE};
    int64_t m_lastOutputDts{AV_NOPTS_VALUE};
    
    // 时间戳基准和偏移
    int64_t m_baseTimestamp{AV_NOPTS_VALUE};
    int64_t m_timestampOffset{0};
    
    // 平滑参数
    double m_smoothingFactor{0.1};  // 平滑系数，越小越平滑
    int64_t m_maxJumpThreshold{90000}; // 最大允许跳跃（1秒@90kHz）
    int64_t m_minIncrement{3000};   // 最小时间戳增量（约33ms@90kHz）
    
    // 统计信息
    int64_t m_totalPackets{0};
    int64_t m_correctedPackets{0};
    
    /**
     * 检测时间戳是否发生了大幅跳跃（可能是文件回环）
     */
    bool detectTimestampWrap(int64_t currentPts, int64_t lastPts) {
        if (lastPts == AV_NOPTS_VALUE) return false;
        
        // 检测向后跳跃（可能是文件回环）
        int64_t diff = currentPts - lastPts;
        return (diff < -m_maxJumpThreshold || diff > m_maxJumpThreshold * 2);
    }
    
    /**
     * 计算平滑后的时间戳增量
     */
    int64_t calculateSmoothIncrement(int64_t rawIncrement) {
        if (m_timestampHistory.empty()) {
            return std::max(rawIncrement, m_minIncrement);
        }
        
        // 计算历史增量的平均值
        int64_t avgIncrement = 0;
        if (m_timestampHistory.size() >= 2) {
            for (size_t i = 1; i < m_timestampHistory.size(); ++i) {
                avgIncrement += m_timestampHistory[i] - m_timestampHistory[i-1];
            }
            avgIncrement /= (m_timestampHistory.size() - 1);
        } else {
            avgIncrement = m_minIncrement;
        }
        
        // 使用指数移动平均进行平滑
        int64_t smoothedIncrement = static_cast<int64_t>(
            m_smoothingFactor * rawIncrement + 
            (1.0 - m_smoothingFactor) * avgIncrement
        );
        
        // 确保最小增量
        return std::max(smoothedIncrement, m_minIncrement);
    }
    
    /**
     * 更新时间戳历史记录
     */
    void updateHistory(int64_t timestamp) {
        m_timestampHistory.push_back(timestamp);
        if (m_timestampHistory.size() > MAX_HISTORY_SIZE) {
            m_timestampHistory.pop_front();
        }
    }

public:
    TimestampSmoother() = default;
    
    /**
     * 设置平滑参数
     */
    void setSmoothingParameters(double smoothingFactor, int64_t maxJumpThreshold, int64_t minIncrement) {
        // 使用兼容的方式替代std::clamp (C++17)
        m_smoothingFactor = std::max(0.01, std::min(smoothingFactor, 1.0));
        m_maxJumpThreshold = maxJumpThreshold;
        m_minIncrement = minIncrement;
    }
    
    /**
     * 重置时间戳状态（用于新的流或重新开始）
     */
    void reset() {
        m_timestampHistory.clear();
        m_lastOutputPts = AV_NOPTS_VALUE;
        m_lastOutputDts = AV_NOPTS_VALUE;
        m_baseTimestamp = AV_NOPTS_VALUE;
        m_timestampOffset = 0;
        m_totalPackets = 0;
        m_correctedPackets = 0;
    }
    
    /**
     * 处理并平滑时间戳
     * @param pkt 输入数据包
     * @return 是否成功处理
     */
    bool smoothTimestamp(AVPacket* pkt) {
        if (!pkt) return false;
        
        m_totalPackets++;
        
        int64_t originalPts = pkt->pts;
        int64_t originalDts = pkt->dts;
        
        // 如果没有有效的时间戳，生成一个
        if (originalPts == AV_NOPTS_VALUE) {
            if (m_lastOutputPts == AV_NOPTS_VALUE) {
                pkt->pts = pkt->dts = 0;
                m_lastOutputPts = m_lastOutputDts = 0;
                m_baseTimestamp = 0;
            } else {
                pkt->pts = pkt->dts = m_lastOutputPts + m_minIncrement;
                m_lastOutputPts = m_lastOutputDts = pkt->pts;
            }
            m_correctedPackets++;
            updateHistory(pkt->pts);
            return true;
        }
        
        // 初始化基准时间戳
        if (m_baseTimestamp == AV_NOPTS_VALUE) {
            m_baseTimestamp = originalPts;
            m_timestampOffset = 0;
            pkt->pts = pkt->dts = 0;
            m_lastOutputPts = m_lastOutputDts = 0;
            updateHistory(0);
            return true;
        }
        
        // 检测时间戳回环或大幅跳跃
        if (detectTimestampWrap(originalPts, m_timestampHistory.empty() ? 
                               m_baseTimestamp : m_timestampHistory.back())) {
            // 发生回环，重新调整基准
            m_timestampOffset += (m_lastOutputPts + m_minIncrement);
            m_baseTimestamp = originalPts;
            m_correctedPackets++;
        }
        
        // 计算相对于基准的时间戳
        int64_t relativePts = originalPts - m_baseTimestamp + m_timestampOffset;
        
        // 确保单调递增
        if (m_lastOutputPts != AV_NOPTS_VALUE) {
            if (relativePts <= m_lastOutputPts) {
                // 时间戳没有递增，强制递增
                relativePts = m_lastOutputPts + m_minIncrement;
                m_correctedPackets++;
            } else {
                // 计算增量并进行平滑
                int64_t rawIncrement = relativePts - m_lastOutputPts;
                if (rawIncrement > m_maxJumpThreshold) {
                    // 增量过大，进行平滑
                    int64_t smoothIncrement = calculateSmoothIncrement(rawIncrement);
                    relativePts = m_lastOutputPts + smoothIncrement;
                    m_correctedPackets++;
                }
            }
        }
        
        // 设置输出时间戳
        pkt->pts = pkt->dts = relativePts;
        m_lastOutputPts = m_lastOutputDts = relativePts;
        
        updateHistory(relativePts);
        return true;
    }
    
    /**
     * 获取统计信息
     */
    void getStatistics(int64_t& totalPackets, int64_t& correctedPackets, double& correctionRate) const {
        totalPackets = m_totalPackets;
        correctedPackets = m_correctedPackets;
        correctionRate = m_totalPackets > 0 ? 
                        static_cast<double>(m_correctedPackets) / m_totalPackets : 0.0;
    }
    
    /**
     * 打印统计信息
     */
    void printStatistics() const {
        if (m_totalPackets > 0) {
            double correctionRate = static_cast<double>(m_correctedPackets) / m_totalPackets * 100.0;
            std::cout <<"Timestamp Smoother Stats: Total=" << m_totalPackets
                << ", Corrected="  << m_correctedPackets
                    << std::setprecision(2) << correctionRate << std::endl;
        }
    }
};

} // namespace otl

#endif // TIMESTAMP_SMOOTHER_H
