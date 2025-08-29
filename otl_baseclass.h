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
#include <vector>
#include <string>
#include <type_traits>
#include <cstdint>
#include <sstream>

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

        bool check_buffer(size_t len);

        bool check_underflow(size_t len);

        int push_internal(const void* p, size_t len);

        int pop_internal(void* p, size_t len);

        int pop_front_internal(void* p, size_t len);

        uint64_t bm_htonll(uint64_t val);

        uint64_t bm_ntohll(uint64_t val);

        std::function<void(void*)> m_freeFunc;

    public:
        ByteBuffer(size_t size = 1024);

        ByteBuffer(char* buf, int size, std::function<void(void*)> free_func = nullptr);

        // Copy-initialization from const buffer (the data will be copied)
        ByteBuffer(const char* buf, size_t size);

        // Copy-initialization from std::string
        ByteBuffer(const std::string& s);

        virtual ~ByteBuffer();

        // Generic typed API (preferred): avoids implicit conversions at call sites.
        template <typename T>
        int push_back(const T& v)
        {
            if constexpr (std::is_same<T, float>::value || std::is_same<T, double>::value)
            {
                return push_internal(&v, sizeof(T));
            }
            else if constexpr (std::is_integral<T>::value)
            {
                using U = typename std::make_unsigned<T>::type;
                U tmp;
                memcpy(&tmp, &v, sizeof(T));
                if constexpr (sizeof(T) == 2)
                {
                    tmp = static_cast<U>(htons(static_cast<uint16_t>(tmp)));
                }
                else if constexpr (sizeof(T) == 4)
                {
                    tmp = static_cast<U>(htonl(static_cast<uint32_t>(tmp)));
                }
                else if constexpr (sizeof(T) == 8)
                {
                    tmp = static_cast<U>(bm_htonll(static_cast<uint64_t>(tmp)));
                }
                // 1-byte integers: no change
                return push_internal(&tmp, sizeof(T));
            }
            else
            {
                static_assert(!sizeof(T*), "ByteBuffer::push_back unsupported type");
                return -1;
            }
        }

        // Legacy overloads retained for compatibility; forward to template API
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

        template <typename T>
        int pop(T& val)
        {
            if constexpr (std::is_same<T, float>::value || std::is_same<T, double>::value)
            {
                return pop_internal(&val, sizeof(T));
            }
            else if constexpr (std::is_integral<T>::value)
            {
                using U = typename std::make_unsigned<T>::type;
                U tmp = 0;
                if (pop_internal(&tmp, sizeof(T)) != 0) return -1;
                if constexpr (sizeof(T) == 2)
                {
                    tmp = static_cast<U>(ntohs(static_cast<uint16_t>(tmp)));
                }
                else if constexpr (sizeof(T) == 4)
                {
                    tmp = static_cast<U>(ntohl(static_cast<uint32_t>(tmp)));
                }
                else if constexpr (sizeof(T) == 8)
                {
                    tmp = static_cast<U>(bm_ntohll(static_cast<uint64_t>(tmp)));
                }
                memcpy(&val, &tmp, sizeof(T));
                return 0;
            }
            else
            {
                static_assert(!sizeof(T*), "ByteBuffer::pop unsupported type");
                return -1;
            }
        }

        // Legacy overloads retained; forward to template API
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
        template <typename T>
        int pop_front(T& val)
        {
            if constexpr (std::is_same<T, float>::value || std::is_same<T, double>::value)
            {
                return pop_front_internal(&val, sizeof(T));
            }
            else if constexpr (std::is_integral<T>::value)
            {
                using U = typename std::make_unsigned<T>::type;
                U tmp = 0;
                if (pop_front_internal(&tmp, sizeof(T)) != 0) return -1;
                if constexpr (sizeof(T) == 2)
                {
                    tmp = static_cast<U>(ntohs(static_cast<uint16_t>(tmp)));
                }
                else if constexpr (sizeof(T) == 4)
                {
                    tmp = static_cast<U>(ntohl(static_cast<uint32_t>(tmp)));
                }
                else if constexpr (sizeof(T) == 8)
                {
                    tmp = static_cast<U>(bm_ntohll(static_cast<uint64_t>(tmp)));
                }
                memcpy(&val, &tmp, sizeof(T));
                return 0;
            }
            else
            {
                static_assert(!sizeof(T*), "ByteBuffer::pop_front unsupported type");
                return -1;
            }
        }

        // Legacy overloads retained; forward to template API
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

        // const accessors
        const char* data() const { return m_bytes; }
        int size() const { return m_back_offset - m_front_offset; }

        // Append raw bytes
        int append(const void* p, size_t len) { return push_internal(p, len); }
        // Append from const char* (length provided)
        int append(const char* p, size_t len) { return push_internal(p, len); }
        // Append zero-terminated C-string (strlen)
        int append(const char* p) { return push_internal(p, strlen(p)); }
        // Append from std::string
        int append(const std::string& s) { return push_internal(s.data(), s.size()); }
    };

    struct Bbox
    {
        int classId;
        float confidence;
        float x1,y1, x2,y2;
    };

    struct Serialable
    {
        virtual void fromByteBuffer(otl::ByteBuffer *buf) = 0;
        virtual std::shared_ptr<otl::ByteBuffer> toByteBuffer() = 0;
        virtual ~Serialable() = default;
    };

    struct Detection : public Serialable {
        std::vector<Bbox> m_bboxes;
        int32_t m_type;
    public:
        Detection()
        {
            m_type = 0;
        }

        int32_t type() {
            return m_type;
        }

        std::vector<Bbox>& bboxes() {
            return m_bboxes;
        }

        std::shared_ptr<otl::ByteBuffer> toByteBuffer() override
        {
            std::shared_ptr<otl::ByteBuffer> buf = std::make_shared<otl::ByteBuffer>();
            buf->push_back((int32_t)m_type);
            buf->push_back((uint32_t)m_bboxes.size());
            for (auto o : m_bboxes)
            {
                buf->push_back(o.x1);
                buf->push_back(o.y1);
                buf->push_back(o.x2);
                buf->push_back(o.y2);
                buf->push_back(o.confidence);
                buf->push_back(o.classId);
            }

            return buf;
        }

        void fromByteBuffer(otl::ByteBuffer* buf) override
        {
            int32_t type;
            buf->pop_front(type);
            m_type = type;

            uint32_t size = 0;
            buf->pop_front(size);
            for (int i = 0; i < size; ++i)
            {
                Bbox o;
                buf->pop_front(o.x1);
                buf->pop_front(o.y1);
                buf->pop_front(o.x2);
                buf->pop_front(o.y2);
                buf->pop_front(o.confidence);
                buf->pop_front(o.classId);
                m_bboxes.push_back(o);
            }
        }

        void clear()
        {
            m_bboxes.clear();
        }

        void push_back(const Bbox& box)
        {
            m_bboxes.push_back(box);
        }

        size_t size()
        {
            return m_bboxes.size();
        }

        std::string toString()
        {
            std::stringstream ss;
            for (int i = 0; i < m_bboxes.size(); ++i)
            {
                ss << i << ":" <<  m_bboxes[i].classId << "," << m_bboxes[i].confidence << " (" << m_bboxes[i].x1 << "," << m_bboxes[i].y1 << ","
                << m_bboxes[i].x2 << "," << m_bboxes[i].y2  << ")" << std::endl;
            }
            return ss.str();
        }

    };


}


#endif //OTL_BASECLASS_H
