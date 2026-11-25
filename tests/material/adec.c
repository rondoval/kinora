/* probe: adec in.kpk out.raw [passes] : decode to S16 big-endian interleaved, time the decode */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include "kpk.h"
#ifdef __amigaos__
#include <proto/dos.h>
unsigned long __stack = 1000000;
static long now_ms(void)
{
    struct DateStamp stamp;
    DateStamp(&stamp);
    return stamp.ds_Minute * 60000L + stamp.ds_Tick * 20L;
}
#else
#include <time.h>
static long now_ms(void) { return (long)(clock() / (CLOCKS_PER_SEC / 1000)); }
#endif

static void log_out(void *avcl, int level, const char *format, va_list args)
{
    (void)avcl;
    if (level <= AV_LOG_WARNING) vprintf(format, args);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 20;
    av_log_set_callback(log_out);
    int passes = argc > 3 ? atoi(argv[3]) : 1;
    FILE *in = fopen(argv[1], "rb");
    if (!in) { printf("no input\n"); return 20; }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size + 64);
    if (fread(data, 1, (size_t)size, in) != (size_t)size) return 20;
    fclose(in);
    const uint8_t *cursor = data, *end = data + size;
    if (kpk_get(&cursor) != KPK_MAGIC) return 20;
    enum AVCodecID id = kpk_get(&cursor);
    int rate = (int)kpk_get(&cursor), channels = (int)kpk_get(&cursor), align = (int)kpk_get(&cursor);
    int extra = (int)kpk_get(&cursor);
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) { printf("no decoder for %d\n", id); return 10; }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = rate;
    ctx->block_align = align;
    av_channel_layout_default(&ctx->ch_layout, channels);
    ctx->extradata = av_mallocz((size_t)extra + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, cursor, (size_t)extra);
    ctx->extradata_size = extra;
    cursor += extra;
    int rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0) { printf("open failed %d\n", rc); return 10; }
    const uint8_t *first = cursor;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwrContext *swr = NULL;
    FILE *out = fopen(argv[2], "wb");
    long samples = 0, frames = 0, errors = 0, decode_ms = 0;
    int16_t *pcm = malloc(16384 * 8 * sizeof(int16_t));
    for (int pass = 0; pass < passes; pass++) {
        long start = now_ms();
        samples = frames = 0;
        for (cursor = first; cursor + 4 <= end;) {
            uint32_t length = kpk_get(&cursor);
            av_new_packet(packet, (int)length);
            memcpy(packet->data, cursor, length);
            cursor += length;
            if (avcodec_send_packet(ctx, packet) < 0) errors++;
            av_packet_unref(packet);
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                frames++;
                samples += frame->nb_samples;
                if (pass == 0 && out) {
                    if (!swr) {
                        swr_alloc_set_opts2(&swr, &frame->ch_layout, AV_SAMPLE_FMT_S16, frame->sample_rate,
                                            &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
                        swr_init(swr);
                        printf("format=%s rate=%d channels=%d\n", av_get_sample_fmt_name(frame->format),
                               frame->sample_rate, frame->ch_layout.nb_channels);
                    }
                    uint8_t *planes[1] = { (uint8_t *)pcm };
                    int got = swr_convert(swr, planes, 16384, (const uint8_t **)frame->extended_data,
                                          frame->nb_samples);
                    for (int i = 0; i < got * frame->ch_layout.nb_channels; i++) {
                        fputc((pcm[i] >> 8) & 0xFF, out);
                        fputc(pcm[i] & 0xFF, out);
                    }
                }
                av_frame_unref(frame);
            }
        }
        avcodec_flush_buffers(ctx);
        if (pass > 0 || passes == 1) decode_ms += now_ms() - start;
    }
    if (out) fclose(out);
    int timed = passes > 1 ? passes - 1 : 1;
    long audio_ms = (long)((double)samples * 1000.0 / rate);
    printf("codec=%s frames=%ld samples=%ld errors=%ld audio_ms=%ld decode_ms_per_pass=%ld\n", codec->name,
           frames, samples, errors, audio_ms, decode_ms / timed);
    printf("cpu_permille=%ld\nDONE\n", audio_ms ? decode_ms / timed * 1000 / audio_ms : -1);
    return 0;
}
