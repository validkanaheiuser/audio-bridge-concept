/**
 * Opus Wrapper - Audio Bridge
 * Simplified Opus encoder/decoder implementation
 */

#include "opus_wrapper.h"
#include "audio_bridge.h"
#include <string.h>

// ──────────────────────────────────────────────────────────────────────────
// Encoder
// ──────────────────────────────────────────────────────────────────────────

OpusEncoderWrapper::OpusEncoderWrapper(int sample_rate, int channels, int bitrate) {
    int err;
    encoder_ = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
    if (encoder_ && err == OPUS_OK) {
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
        opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10));
    } else {
        encoder_ = nullptr;
    }
}

OpusEncoderWrapper::~OpusEncoderWrapper() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
    }
}

int OpusEncoderWrapper::encode(const int16_t* pcm, int frame_size, 
                                uint8_t* output, int max_output) {
    if (!encoder_) return -1;
    return opus_encode(encoder_, pcm, frame_size, output, max_output);
}

// ──────────────────────────────────────────────────────────────────────────
// Decoder
// ──────────────────────────────────────────────────────────────────────────

OpusDecoderWrapper::OpusDecoderWrapper(int sample_rate, int channels) {
    int err;
    decoder_ = opus_decoder_create(sample_rate, channels, &err);
    if (err != OPUS_OK) {
        decoder_ = nullptr;
    }
}

OpusDecoderWrapper::~OpusDecoderWrapper() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
    }
}

int OpusDecoderWrapper::decode(const uint8_t* data, int len, 
                                int16_t* pcm, int frame_size) {
    if (!decoder_) return -1;
    return opus_decode(decoder_, data, len, pcm, frame_size, 0);
}

int OpusDecoderWrapper::decodePLC(int16_t* pcm, int frame_size) {
    if (!decoder_) return -1;
    return opus_decode(decoder_, nullptr, 0, pcm, frame_size, 0);
}
