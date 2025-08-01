//
// Created by hsyuan on 2019-02-28.
//

#ifndef STREAM_SEI_H
#define STREAM_SEI_H

#include <stdio.h>
#include <stdlib.h>
#include <iostream>

namespace otl {

uint32_t reverseBytes(uint32_t value);

uint32_t h264SeiCalcPacketSize(uint32_t size);
int h264SeiPacketWrite(uint8_t *oPacketBuf, bool isAnnexb, const uint8_t *content, uint32_t size);
int h264SeiPacketRead(uint8_t *inPacket, uint32_t size, uint8_t *buffer, int buffSize);

int h265SeiPacketWrite(uint8_t *packet, bool isAnnexb, const uint8_t *content, uint32_t size);
int h265SeiPacketRead(uint8_t *packet, uint32_t size, uint8_t *buffer, int bufSize);

} // namespace otl

#endif // STREAM_SEI_H
