//
// Created by yuan on 2025/7/31.
//

#ifndef OTL_BASECLASS_H
#define OTL_BASECLASS_H

namespace otl
{
    class NoCopyable
    {
    public:
        // 允许派生类构造和析构
        NoCopyable() = default;
        ~NoCopyable() = default;

        NoCopyable(const NoCopyable&) = delete;
        NoCopyable& operator=(const NoCopyable&) = delete;
    };
}



#endif //OTL_BASECLASS_H