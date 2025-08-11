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
    // SEI payload size (UUID + content)
    uint32_t seiPayloadSize = content + UUID_SIZE;
    // payloadSize field uses bytes of 0xFF and a last byte < 0xFF
    uint32_t payloadSizeFieldBytes = (seiPayloadSize / 0xFF) + 1;
    // NAL header(1) + payloadType(1 for user_data_unregistered=5) + payloadSizeField + payload + rbsp_trailing_bits(1 byte 0x80)
    uint32_t seiSize = 1 + 1 + payloadSizeFieldBytes + seiPayloadSize + 1;
    return seiSize;
}

uint32_t h264SeiCalcPacketSize(uint32_t size)
{
    // Backward compatible: default to Annex B (start code size = 4)
    return h264SeiCalcNaluSize(size) + 4;
}

uint32_t h264SeiCalcPacketSize(uint32_t size, bool isAnnexb, uint8_t lengthSizeBytes)
{
    uint32_t nalu = h264SeiCalcNaluSize(size);
    if (isAnnexb) {
        return nalu + 4; // 0x00000001
    } else {
        return nalu + (uint32_t)lengthSizeBytes; // AVCC length prefix (typically 4)
    }
}

int h264SeiPacketWrite(uint8_t *oPacketBuf, bool isAnnexb, const uint8_t *content, uint32_t size)
{
    unsigned char * data = (unsigned char*)oPacketBuf;
    //unsigned int naluSize = (unsigned int)h264SeiCalcNaluSize(size);
    //uint32_t seiSize = naluSize;

    // Write AnnexB start code or AVCC length prefix
    if (isAnnexb) {
        memcpy(data, start_code, sizeof(start_code));
        data += sizeof(start_code);
    } else {
        // Reserve 4 bytes for length; we will compute length after writing NAL
        data += sizeof(uint32_t);
    }

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


    // Compute and write length for AVCC if needed (length excludes the 4-byte length field)
    if (!isAnnexb) {
        uint32_t naluSize = (uint32_t)(data - (oPacketBuf + sizeof(uint32_t)));
        // big-endian length
        uint32_t be = reverseBytes(naluSize);
        memcpy(oPacketBuf, &be, sizeof(be));
    }

    return (int)(data - oPacketBuf);
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
                    if (*data == 6) // SEI
                    {
                        // Skip NAL header byte before parsing SEI payload
                        int seiSize = getSeiBuffer(data + 1, inPacket + size - (data + 1), buffer, buffSize);
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
        // AVCC format: 4-byte big-endian length prefix per NALU
        unsigned char *ptr = inPacket;
        while ((uint32_t)(ptr - inPacket) + 4 <= size) {
            uint32_t beLen;
            memcpy(&beLen, ptr, 4);
            uint32_t naluLen = reverseBytes(beLen);
            ptr += 4;
            if ((uint32_t)(ptr - inPacket) + naluLen > size) break;
            if (naluLen >= 1) {
                // H.264 NAL header is 1 byte
                if ( (ptr[0] & 0x1F) == 6 ) { // SEI
                    int seiSize = getSeiBuffer(ptr + 1, (uint32_t)naluLen - 1, buffer, buffSize);
                    if (seiSize > 0) return seiSize;
                }
            }
            ptr += naluLen;
        }
        printf("can't find SEI in AVCC\n");
    }
    return -1;
}


int h265SeiPacketWrite(uint8_t *packet, bool isAnnexb, const uint8_t *content, uint32_t size)
{
    unsigned char * data = (unsigned char*)packet;
    //unsigned int naluSize = (unsigned int)h264SeiCalcNaluSize(size);
    //uint32_t seiSize = naluSize;

    // Write AnnexB start code or AVCC length prefix
    if (isAnnexb) {
        memcpy(data, start_code, sizeof(start_code));
        data += sizeof(start_code);
    } else {
        // Reserve 4 bytes for length; we will compute after writing NAL
        data += sizeof(uint32_t);
    }

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


    // Compute and write length for AVCC if needed (length excludes the 4-byte length field)
    if (!isAnnexb) {
        uint32_t naluSize = (uint32_t)(data - (packet + sizeof(uint32_t)));
        uint32_t be = reverseBytes(naluSize);
        memcpy(packet, &be, sizeof(be));
    }

    return (int)(data - packet);
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
        // AVCC format (length-prefixed)
        unsigned char *ptr = packet;
        while ((uint32_t)(ptr - packet) + 4 <= size) {
            uint32_t beLen;
            memcpy(&beLen, ptr, 4);
            uint32_t naluLen = reverseBytes(beLen);
            ptr += 4;
            if ((uint32_t)(ptr - packet) + naluLen > size) break;
            if (naluLen >= 2) {
                // HEVC NAL header is 2 bytes; nal_unit_type in bits[1..6] of first byte
                uint8_t nalUnitType = (ptr[0] >> 1) & 0x3F;
                if (nalUnitType == 39 || nalUnitType == 40) { // SEI prefix(39)/suffix(40)
                    unsigned char *sei = ptr + 2;
                    int ret = getSeiBuffer(sei, naluLen - 2, buffer, bufSize);
                    if (ret != -1) return ret;
                }
            }
            ptr += naluLen;
        }
    }
    printf("can't find NALU startCode or SEI\n");
    return -1;
}

} // namespace otl
