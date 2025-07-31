//
// Created by hsyuan on 2019-02-28.
//

#include "stream_sei.h"
#include <memory.h>

namespace otl {

#define UUID_SIZE 16

//FFMPEG uuid
//static unsigned char uuid[] = { 0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7, 0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef };

//self UUID
static unsigned char uuid[] = { 0x54, 0x80, 0x83, 0x97, 0xf0, 0x23, 0x47, 0x4b, 0xb7, 0xf7, 0x4f, 0x32, 0xb5, 0x4e, 0x06, 0xac };

//��ʼ��
static unsigned char start_code[] = { 0x00,0x00,0x00,0x01 };


uint32_t reverseBytes(uint32_t value) {
    return ((value & 0x000000FF) << 24) |
           ((value & 0x0000FF00) << 8) |
           ((value & 0x00FF0000) >> 8) |
           ((value & 0xFF000000) >> 24);
}

uint32_t h264SeiCalcNaluSize(uint32_t content)
{
    //SEI payload size
    uint32_t seiPayloadSize = content + UUID_SIZE;
    //NALU + payload���� + ���ݳ��� + ����
    uint32_t seiSize = 1 + 1 + (seiPayloadSize / 0xFF + (seiPayloadSize % 0xFF != 0 ? 1 : 0)) + seiPayloadSize;
    //��ֹ��
    uint32_t tailSize = 2;
    if (seiSize % 2 == 1)
    {
        tailSize -= 1;
    }
    seiSize += tailSize;

    return seiSize;
}

uint32_t h264SeiCalcPacketSize(uint32_t size)
{
    // Packet size = NALUSize + StartCodeSize
    return h264SeiCalcNaluSize(size) + 4;
}

int h264SeiPacketWrite(uint8_t *oPacketBuf, bool isAnnexb, const uint8_t *content, uint32_t size)
{
    unsigned char * data = (unsigned char*)oPacketBuf;
    //unsigned int naluSize = (unsigned int)h264SeiCalcNaluSize(size);
    //uint32_t seiSize = naluSize;


    memcpy(data, start_code, sizeof(start_code));

    data += sizeof(unsigned int);

    //unsigned char * sei = data;
    //NAL header
    *data++ = 6; //SEI
    //sei payload type
    *data++ = 5; //unregister
    size_t seiPayloadSize = size + UUID_SIZE;

    while (true)
    {
        *data++ = (seiPayloadSize >= 0xFF ? 0xFF : (char)seiPayloadSize);
        if (seiPayloadSize < 0xFF) break;
        seiPayloadSize -= 0xFF;
    }

    //UUID
    memcpy(data, uuid, UUID_SIZE);
    data += UUID_SIZE;

    memcpy(data, content, size);
    data += size;

    *data = 0x80;
    data++;


    return data - oPacketBuf;
}

static int getSeiBuffer(unsigned char * data, uint32_t size, uint8_t * buff, int bufSize)
{
    unsigned char * sei = data;
    int seiType = 0;
    unsigned seiSize = 0;
    //payload type
    do {
        seiType += *sei;
    } while (*sei++ == 255);

    do {
        seiSize += *sei;
    } while (*sei++ == 255);

    if (seiSize >= UUID_SIZE && seiSize <= (data + size - sei) &&
        seiType == 5 && memcmp(sei, uuid, UUID_SIZE) == 0)
    {
        sei += UUID_SIZE;
        seiSize -= UUID_SIZE;

        if (buff != NULL && bufSize != 0)
        {
            if (bufSize > (int)seiSize)
            {
                memcpy(buff, sei, seiSize);
            }else{
                printf("ERROR:input buffer(%d) < SEI size(%d)\n", bufSize, seiSize);
                return -1;
            }
        }

        return seiSize;
    }
    return -1;
}

int h264SeiPacketRead(uint8_t *inPacket, uint32_t size, uint8_t *buffer, int buffSize)
{
    unsigned char ANNEXB_CODE_LOW[] = { 0x00,0x00,0x01 };
    unsigned char ANNEXB_CODE[] = { 0x00,0x00,0x00,0x01 };

    unsigned char *data = inPacket;
    bool isAnnexb = false;
    if ((size > 3 && memcmp(data, ANNEXB_CODE_LOW, 3) == 0) ||
        (size > 4 && memcmp(data, ANNEXB_CODE, 4) == 0)
            )
    {
        isAnnexb = true;
    }

    if (isAnnexb)
    {
        while (data < inPacket + size) {
            if ((inPacket + size - data) > 4 && data[0] == 0x00 && data[1] == 0x00)
            {
                int startCodeSize = 2;
                if (data[2] == 0x01)
                {
                    startCodeSize = 3;
                }
                else if (data[2] == 0x00 && data[3] == 0x01)
                {
                    startCodeSize = 4;
                }

                if (startCodeSize == 3 || startCodeSize == 4)
                {
                    data += startCodeSize;
                    if (*data == 6) //SEI
                    {
                        int seiSize = getSeiBuffer(data, inPacket + size - data, buffer, buffSize);
                        if (seiSize > 0)
                        {
                            return seiSize;
                        }
                    }
                }
            }
            data++;
        }
    }
    else
    {
        printf("can't find NALU startCode\n");
    }
    return -1;
}


int h265SeiPacketWrite(uint8_t *packet, bool isAnnexb, const uint8_t *content, uint32_t size)
{
    unsigned char * data = (unsigned char*)packet;
    //unsigned int naluSize = (unsigned int)h264SeiCalcNaluSize(size);
    //uint32_t seiSize = naluSize;


    memcpy(data, start_code, sizeof(start_code));

    data += sizeof(unsigned int);

    uint8_t nalUnitType = 39;
    //unsigned char * sei = data;
    //NAL header
    *data++ = (uint8_t)nalUnitType << 1;
    *data++ = 1 + (nalUnitType == 2);
    //sei payload type
    *data++ = 5; //unregister
    size_t seiPayloadSize = size + UUID_SIZE;

    while (true)
    {
        *data++ = (seiPayloadSize >= 0xFF ? 0xFF : (char)seiPayloadSize);
        if (seiPayloadSize < 0xFF) break;
        seiPayloadSize -= 0xFF;
    }

    //UUID
    memcpy(data, uuid, UUID_SIZE);
    data += UUID_SIZE;

    memcpy(data, content, size);
    data += size;

    *data = 0x80;
    data++;


    return data - packet;
}

int h265SeiPacketRead(uint8_t *packet, uint32_t size, uint8_t *buffer, int bufSize)
{
    unsigned char ANNEXB_CODE_LOW[] = { 0x00,0x00,0x01 };
    unsigned char ANNEXB_CODE[] = { 0x00,0x00,0x00,0x01 };

    unsigned char *data = packet;
    bool isAnnexb = false;
    if ((size > 3 && memcmp(data, ANNEXB_CODE_LOW, 3) == 0) ||
        (size > 4 && memcmp(data, ANNEXB_CODE, 4) == 0)
            )
    {
        isAnnexb = true;
    }

    if (isAnnexb)
    {
        while (data < packet + size) {
            if ((packet + size - data) > 4 && data[0] == 0x00 && data[1] == 0x00)
            {
                int startCodeSize = 2;
                if (data[2] == 0x01)
                {
                    startCodeSize = 3;
                }
                else if (data[2] == 0x00 && data[3] == 0x01)
                {
                    startCodeSize = 4;
                }

                if (startCodeSize == 3 || startCodeSize == 4)
                {
                    if ((packet + size - data) > (startCodeSize + 2))
                    {
                        //SEI
                        unsigned char * sei = data + startCodeSize + 2;

                        int ret = getSeiBuffer(sei, (packet + size - sei), buffer, bufSize);
                        if (ret != -1)
                        {
                            return ret;
                        }
                    }
                    data += startCodeSize + 2;
                }
                else
                {
                    data += startCodeSize + 2;
                }
            }
            else
            {
                data++;
            }
        }
    }else{
        //TODO: mp4 format
    }
    printf("can't find NALU startCode\n");
    return -1;
}

} // namespace otl
