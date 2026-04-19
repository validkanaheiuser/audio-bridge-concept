/**
 * Opus Wrapper - Audio Bridge
 * Simplified Opus encoder/decoder interface
 */

#ifndef OPUS_WRAPPER_H
#define OPUS_WRAPPER_H

#include <opus/opus.h>
#include <stdint.h>

class OpusEncoderWrapper {
public:
    OpusEncoderWrapper(int sample_rate, int channels, int bitrate);
    ~OpusEncoderWrapper();
    
    int encode(const int16_t* pcm, int frame_size, uint8_t* output, int max_output);
    bool isValid() const { return encoder_ != nullptr; }

private:
    OpusEncoder* encoder_ = nullptr;
};

class OpusDecoderWrapper {
public:
    OpusDecoderWrapper(int sample_rate, int channels);
    ~OpusDecoderWrapper();
    
    int decode(const uint8_t* data, int len, int16_t* pcm, int frame_size);
    int decodePLC(int16_t* pcm, int frame_size);
    bool isValid() const { return decoder_ != nullptr; }

private:
    OpusDecoder* decoder_ = nullptr;
};

#endif // OPUS_WRAPPER_H
