//
// Created by huaishan on 8/8/25.
//
#include "otl_baseclass.h"

namespace otl
{
    bool ByteBuffer::check_buffer(int len)
    {
        if (m_back_offset + len > m_size)
        {
            char* pNew = nullptr;
            pNew = (char*)realloc(m_bytes, m_size + 1024);
            assert(pNew != nullptr);
            if (pNew == nullptr)
            {
                return false;
            }
            else
            {
                m_bytes = pNew;
                m_size += 1024;
            }
        }

        return true;
    }

    bool ByteBuffer::check_buffer2(int len)
    {
        if (m_back_offset - len < 0)
        {
            return false;
        }

        return true;
    }

    int ByteBuffer::push_internal(int8_t* p, int len)
    {
        if (!check_buffer(len)) return -1;
        memcpy((uint8_t*)m_bytes + m_back_offset, p, len);
        m_back_offset += len;
        return 0;
    }

    int ByteBuffer::pop_internal(int8_t* p, int len)
    {
        if (check_buffer2(len) != 0) return -1;
        memcpy(p, &m_bytes[m_back_offset], len);
        m_back_offset -= len;
        return 0;
    }

    int ByteBuffer::pop_front_internal(int8_t* p, int len)
    {
        if (m_front_offset + len > m_back_offset) return -1;
        memcpy(p, &m_bytes[m_front_offset], len);
        m_front_offset += len;
        return 0;
    }

    uint64_t ByteBuffer::bm_htonll(uint64_t val)
    {
        return (((uint64_t)htonl(val)) << 32) + htonl(val >> 32);
    }

    uint64_t ByteBuffer::bm_ntohll(uint64_t val)
    {
        return (((uint64_t)ntohl(val)) << 32) + ntohl(val >> 32);
    }

    std::function<void(void*)> m_freeFunc;

    ByteBuffer::ByteBuffer(size_t size): m_size(size)
    {
        m_bytes = new char[size];
        assert(m_bytes != nullptr);
        m_front_offset = 0;
        m_back_offset = 0;

        m_freeFunc = [this](void* p)
        {
            delete [] m_bytes;
        };
    }

    ByteBuffer::ByteBuffer(char* buf, int size, std::function<void(void*)> free_func)
    {
        m_bytes = buf;
        m_front_offset = 0;
        m_back_offset = size;
        m_size = size;
        m_freeFunc = free_func;
    }

    ByteBuffer::~ByteBuffer()
    {
        if (m_freeFunc) m_freeFunc(m_bytes);
    }


    int ByteBuffer::push_back(int8_t b)
    {
        return push_internal(&b, sizeof(b));
    }

    int ByteBuffer::push_back(uint8_t b)
    {
        return push_internal((int8_t*)&b, sizeof(b));
    }

    int ByteBuffer::push_back(int16_t b)
    {
        b = htons(b);
        int8_t* p = (int8_t*)&b;
        return push_internal(p, sizeof(b));
    }

    int ByteBuffer::push_back(uint16_t b)
    {
        b = htons(b);
        int8_t* p = (int8_t*)&b;
        return push_internal(p, sizeof(b));
    }

    int ByteBuffer::push_back(int32_t b)
    {
        b = htonl(b);
        int8_t* p = (int8_t*)&b;
        return push_internal(p, sizeof(b));
    }

    int ByteBuffer::push_back(uint32_t b)
    {
        b = htonl(b);
        int8_t* p = (int8_t*)&b;
        return push_internal(p, sizeof(b));
    }

    int ByteBuffer::push_back(int64_t b)
    {
        b = bm_htonll(b);
        int8_t* p = (int8_t*)&b;
        return push_internal(p, sizeof(b));
    }

    int ByteBuffer::push_back(uint64_t b)
    {
        b = bm_htonll(b);
        int8_t* p = (int8_t*)&b;
        return push_internal(p, sizeof(b));
    }

    int ByteBuffer::push_back(float f)
    {
        int8_t* p = (int8_t*)&f;
        return push_internal(p, sizeof(f));
    }

    int ByteBuffer::push_back(double d)
    {
        int8_t* p = (int8_t*)&d;
        return push_internal(p, sizeof(d));
    }

    int ByteBuffer::pop(int8_t& val)
    {
        return pop_internal(&val, sizeof(val));
    }

    int ByteBuffer::pop(int16_t& val)
    {
        int16_t t;
        if (pop_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohs(t);
        return 0;
    }

    int ByteBuffer::pop(int32_t& val)
    {
        int32_t t;
        if (pop_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohl(t);
        return 0;
    }

    int ByteBuffer::pop(int64_t& val)
    {
        int64_t t;
        if (pop_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = bm_ntohll(t);
        return 0;
    }

    int ByteBuffer::pop(uint8_t& val)
    {
        return pop_internal((int8_t*)&val, sizeof(val));
    }

    int ByteBuffer::pop(uint16_t& val)
    {
        uint16_t t;
        if (pop_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohs(t);
        return 0;
    }

    int ByteBuffer::pop(uint32_t& val)
    {
        uint32_t t;
        if (pop_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohl(t);
        return 0;
    }

    int ByteBuffer::pop(uint64_t& val)
    {
        uint64_t t;
        if (pop_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = bm_ntohll(t);
        return 0;
    }

    int ByteBuffer::pop(float& val)
    {
        return pop_internal((int8_t*)&val, sizeof(val));
    }

    int ByteBuffer::pop(double& val)
    {
        return pop_internal((int8_t*)&val, sizeof(val));
    }

    int ByteBuffer::pop_front(int8_t& val)
    {
        return pop_front_internal(&val, sizeof(val));
    }

    int ByteBuffer::pop_front(int16_t& val)
    {
        int16_t t;
        if (pop_front_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohs(t);
        return 0;
    }

    int ByteBuffer::pop_front(int32_t& val)
    {
        int32_t t;
        if (pop_front_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohl(t);
        return 0;
    }

    int ByteBuffer::pop_front(int64_t& val)
    {
        int64_t t;
        if (pop_front_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = bm_ntohll(t);
        return 0;
    }

    int ByteBuffer::pop_front(uint8_t& val)
    {
        return pop_front_internal((int8_t*)&val, sizeof(val));
    }

    int ByteBuffer::pop_front(uint16_t& val)
    {
        uint16_t t;
        if (pop_front_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohs(t);
        return 0;
    }

    int ByteBuffer::pop_front(uint32_t& val)
    {
        uint32_t t;
        if (pop_front_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = ntohl(t);
        return 0;
    }

    int ByteBuffer::pop_front(uint64_t& val)
    {
        uint64_t t;
        if (pop_front_internal((int8_t*)&t, sizeof(val)) != 0)
        {
            return -1;
        }

        val = bm_ntohll(t);
        return 0;
    }

    int ByteBuffer::pop_front(float& val)
    {
        return pop_front_internal((int8_t*)&val, sizeof(val));
    }

    int ByteBuffer::pop_front(double& val)
    {
        return pop_front_internal((int8_t*)&val, sizeof(val));
    }

    char* ByteBuffer::data()
    {
        return m_bytes;
    }

    int ByteBuffer::size()
    {
        return m_back_offset - m_front_offset;
    }
}
