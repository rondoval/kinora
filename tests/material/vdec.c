/* host: vdec in.kpv out.md5: a video stream's packets (pkdump ... video) decoded as ffmpegvideo.codec
 * decodes them, a picture a packet; a line a picture: the MD5 of its visible luma rows, then of each
 * chroma plane's, as FilmInfo VIDEO MD5 prints them */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/md5.h>
#include "kpk.h"

static int pictures;

static void digest_put(FILE *out, const AVFrame *frame)
{
    struct AVMD5 *md5 = av_md5_alloc();
    av_md5_init(md5);
    for (int plane = 0; plane < 3; plane++) {
        int shift = plane == 0 ? 0 : 1;
        int width = (frame->width + shift) >> shift, rows = (frame->height + shift) >> shift;
        for (int y = 0; y < rows; y++)
            av_md5_update(md5, frame->data[plane] + (ptrdiff_t)y * frame->linesize[plane], (size_t)width);
    }
    uint8_t digest[16];
    av_md5_final(md5, digest);
    av_free(md5);
    for (int i = 0; i < 16; i++)
        fprintf(out, "%02x", digest[i]);
    fprintf(out, "\n");
    pictures++;
}

static void receive_all(AVCodecContext *decoder, AVFrame *frame, FILE *out)
{
    while (avcodec_receive_frame(decoder, frame) == 0) {
        digest_put(out, frame);
        av_frame_unref(frame);
    }
}

int main(int argc, char **argv)
{
    FILE *in = argc > 2 ? fopen(argv[1], "rb") : NULL;
    if (in == NULL) return 1;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)size);
    if (fread(file, 1, (size_t)size, in) != (size_t)size) return 1;
    const uint8_t *cursor = file, *end = file + size;
    if (kpk_get(&cursor) != KPK_VIDEO_MAGIC) return 2;
    enum AVCodecID id = (enum AVCodecID)kpk_get(&cursor);
    const AVCodec *codec = avcodec_find_decoder(id);
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    decoder->codec_tag = kpk_get(&cursor);
    decoder->width = (int)kpk_get(&cursor);
    decoder->height = (int)kpk_get(&cursor);
    decoder->extradata_size = (int)kpk_get(&cursor);
    if (decoder->extradata_size > 0) {
        decoder->extradata = av_mallocz((size_t)decoder->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(decoder->extradata, cursor, (size_t)decoder->extradata_size);
        cursor += decoder->extradata_size;
    }
    decoder->thread_count = 1;
    if (codec == NULL || avcodec_open2(decoder, codec, NULL) < 0) return 3;
    FILE *out = fopen(argv[2], "w");
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int packets = 0, damaged = 0;
    while (cursor + 4 <= end) {
        uint32_t length = kpk_get(&cursor);
        av_new_packet(packet, (int)length);
        memcpy(packet->data, cursor, length);
        cursor += length;
        if (length != 0 && avcodec_send_packet(decoder, packet) != 0)
            damaged++;
        av_packet_unref(packet);
        packets++;
        receive_all(decoder, frame, out);
    }
    avcodec_send_packet(decoder, NULL);
    receive_all(decoder, frame, out);
    fclose(out);
    printf("%s: codec=%s packets=%d damaged=%d pictures=%d\n", argv[1], codec->name, packets, damaged, pictures);
    return 0;
}
