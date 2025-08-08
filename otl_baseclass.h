//
// Created by yuan on 2025/7/31.
//

#ifndef OTL_BASECLASS_H
#define OTL_BASECLASS_H

#ifdef __linux__
#include <netinet/in.h>
#endif

#ifdef WIN32
#include <WinSock2.h>
#include <Windows.h>
#endif

#include <cassert>
#include <memory>
#include <cstring>
#include <functional>

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

    class ByteBuffer : public NoCopyable
    {
        char* m_bytes;
        int m_back_offset;
        int m_front_offset;
        size_t m_size;

        bool check_buffer(int len);

        bool check_buffer2(int len);

        int push_internal(int8_t* p, int len);

        int pop_internal(int8_t* p, int len);

        int pop_front_internal(int8_t* p, int len);

        uint64_t bm_htonll(uint64_t val);

        uint64_t bm_ntohll(uint64_t val);

        std::function<void(void*)> m_freeFunc;

    public:
        ByteBuffer(size_t size = 1024);

        ByteBuffer(char* buf, int size, std::function<void(void*)> free_func = nullptr);

        virtual ~ByteBuffer();


        int push_back(int8_t b);

        int push_back(uint8_t b);

        int push_back(int16_t b);

        int push_back(uint16_t b);

        int push_back(int32_t b);

        int push_back(uint32_t b);

        int push_back(int64_t b);

        int push_back(uint64_t b);

        int push_back(float f);

        int push_back(double d);

        int pop(int8_t& val);
        int pop(int16_t& val);

        int pop(int32_t& val);

        int pop(int64_t& val);

        int pop(uint8_t& val);
        int pop(uint16_t& val);

        int pop(uint32_t& val);

        int pop(uint64_t& val);

        int pop(float& val);
        int pop(double& val);
        int pop_front(int8_t& val);

        int pop_front(int16_t& val);

        int pop_front(int32_t& val);

        int pop_front(int64_t& val);

        int pop_front(uint8_t& val);

        int pop_front(uint16_t& val);

        int pop_front(uint32_t& val);

        int pop_front(uint64_t& val);

        int pop_front(float& val);
        int pop_front(double& val);
        char* data();
        int size();
    };
}


#endif //OTL_BASECLASS_H
