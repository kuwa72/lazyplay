#include "audio_decoder.h"

#include <aacdecoder_lib.h>
#include <cstring>

AacEldDecoder::AacEldDecoder() {}

AacEldDecoder::~AacEldDecoder() {
    Shutdown();
}

bool AacEldDecoder::Init() {
    m_dec = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!m_dec) return false;

    // AudioSpecificConfig for AAC-ELD 44100 Hz stereo (same value UxPlay uses)
    uint8_t asc[] = { 0xF8, 0xE8, 0x50, 0x00 };
    UCHAR* conf[] = { asc };
    UINT confLen[] = { sizeof(asc) };
    if (aacDecoder_ConfigRaw(static_cast<HANDLE_AACDECODER>(m_dec), conf, confLen) != AAC_DEC_OK) {
        Shutdown();
        return false;
    }
    return true;
}

void AacEldDecoder::Shutdown() {
    if (m_dec) {
        aacDecoder_Close(static_cast<HANDLE_AACDECODER>(m_dec));
        m_dec = nullptr;
    }
}

bool AacEldDecoder::Decode(const uint8_t* frame, size_t len, std::vector<int16_t>& pcm) {
    if (!m_dec || !frame || len == 0) return false;

    HANDLE_AACDECODER dec = static_cast<HANDLE_AACDECODER>(m_dec);
    uint8_t* inBuf[] = { const_cast<uint8_t*>(frame) };
    UINT inLen[] = { static_cast<UINT>(len) };
    UINT valid = static_cast<UINT>(len);

    if (aacDecoder_Fill(dec, inBuf, inLen, &valid) != AAC_DEC_OK) return false;

    pcm.resize(8192); // comfortably holds one ELD frame (480 samples x 2ch)
    AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(dec, pcm.data(),
                                                 static_cast<UINT>(pcm.size()), 0);
    if (err == AAC_DEC_NOT_ENOUGH_BITS || err == AAC_DEC_TRANSPORT_SYNC_ERROR) {
        return false; // incomplete/corrupt frame: skip
    }
    if (err != AAC_DEC_OK) return false;

    const CStreamInfo* info = aacDecoder_GetStreamInfo(dec);
    if (!info || info->frameSize <= 0) return false;
    pcm.resize(info->frameSize * info->numChannels);
    return true;
}
