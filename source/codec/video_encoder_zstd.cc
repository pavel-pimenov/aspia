//
// Aspia Project
// Copyright (C) 2018 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "codec/video_encoder_zstd.h"

#include "base/logging.h"
#include "codec/pixel_translator.h"
#include "codec/video_util.h"
#include "desktop/desktop_frame.h"

namespace codec {

namespace {

// Retrieves a pointer to the output buffer in |update| used for storing the
// encoded rectangle data. Will resize the buffer to |size|.
uint8_t* outputBuffer(proto::desktop::VideoPacket* packet, size_t size)
{
    packet->mutable_data()->resize(size);
    return reinterpret_cast<uint8_t*>(packet->mutable_data()->data());
}

} // namespace

VideoEncoderZstd::VideoEncoderZstd(std::unique_ptr<PixelTranslator> translator,
                                   const desktop::PixelFormat& target_format,
                                   int compression_ratio)
    : target_format_(target_format),
      compress_ratio_(compression_ratio),
      stream_(ZSTD_createCStream()),
      translator_(std::move(translator))
{
    // Nothing
}

// static
VideoEncoderZstd* VideoEncoderZstd::create(
    const desktop::PixelFormat& target_format, int compression_ratio)
{
    if (compression_ratio > ZSTD_maxCLevel())
        compression_ratio = ZSTD_maxCLevel();
    else if (compression_ratio < 1)
        compression_ratio = 1;

    std::unique_ptr<PixelTranslator> translator =
        PixelTranslator::create(desktop::PixelFormat::ARGB(), target_format);
    if (!translator)
    {
        LOG(LS_WARNING) << "Unsupported pixel format";
        return nullptr;
    }

    return new VideoEncoderZstd(std::move(translator), target_format, compression_ratio);
}

void VideoEncoderZstd::compressPacket(proto::desktop::VideoPacket* packet,
                                      const uint8_t* input_data,
                                      size_t input_size)
{
    size_t ret = ZSTD_initCStream(stream_.get(), compress_ratio_);
    DCHECK(!ZSTD_isError(ret)) << ZSTD_getErrorName(ret);

    const size_t output_size = ZSTD_compressBound(input_size);
    uint8_t* output_data = outputBuffer(packet, output_size);

    ZSTD_inBuffer input = { input_data, input_size, 0 };
    ZSTD_outBuffer output = { output_data, output_size, 0 };

    while (input.pos < input.size)
    {
        ret = ZSTD_compressStream(stream_.get(), &output, &input);
        if (ZSTD_isError(ret))
        {
            LOG(LS_WARNING) << "ZSTD_compressStream failed: " << ZSTD_getErrorName(ret);
            return;
        }
    }

    ret = ZSTD_endStream(stream_.get(), &output);
    DCHECK(!ZSTD_isError(ret)) << ZSTD_getErrorName(ret);

    packet->mutable_data()->resize(output.pos);
}

void VideoEncoderZstd::encode(const desktop::Frame* frame, proto::desktop::VideoPacket* packet)
{
    fillPacketInfo(proto::desktop::VIDEO_ENCODING_ZSTD, frame, packet);

    if (packet->has_format())
    {
        VideoUtil::toVideoPixelFormat(
            target_format_, packet->mutable_format()->mutable_pixel_format());
    }

    size_t data_size = 0;

    for (const auto& rect : frame->constUpdatedRegion())
    {
        data_size += rect.width() * rect.height() * target_format_.bytesPerPixel();
        VideoUtil::toVideoRect(rect, packet->add_dirty_rect());
    }

    if (translate_buffer_size_ < data_size)
    {
        translate_buffer_.reset(static_cast<uint8_t*>(base::alignedAlloc(data_size, 32)));
        translate_buffer_size_ = data_size;
    }

    uint8_t* translate_pos = translate_buffer_.get();

    for (const auto& rect : frame->constUpdatedRegion())
    {
        const int stride = rect.width() * target_format_.bytesPerPixel();

        translator_->translate(frame->frameDataAtPos(rect.topLeft()),
                               frame->stride(),
                               translate_pos,
                               stride,
                               rect.width(),
                               rect.height());

        translate_pos += rect.height() * stride;
    }

    // Compress data with using Zstd compressor.
    compressPacket(packet, translate_buffer_.get(), data_size);
}

} // namespace codec
