/* host: pkdump in.mkv out.kpk [video]: the first audio stream's packets, or with "video" the first
 * video stream's, through the filter demuxer.library gives MPEG-4 Part 2 (packed B-pictures unpacked) */
#include <string.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include "kpk.h"

static void packet_put(FILE *out, const AVPacket *packet, int *count)
{
    kpk_put(out, (uint32_t)packet->size);
    fwrite(packet->data, 1, (size_t)packet->size, out);
    (*count)++;
}

int main(int argc, char **argv)
{
    AVFormatContext *ctx = NULL;
    if (argc < 3 || avformat_open_input(&ctx, argv[1], NULL, NULL) < 0) return 1;
    avformat_find_stream_info(ctx, NULL);
    int video = argc > 3 && strcmp(argv[3], "video") == 0;
    enum AVMediaType type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    int index = -1;
    for (unsigned i = 0; i < ctx->nb_streams; i++)
        if (ctx->streams[i]->codecpar->codec_type == type) { index = (int)i; break; }
    if (index < 0) return 2;
    AVCodecParameters *par = ctx->streams[index]->codecpar;
    AVBSFContext *filter = NULL;
    if (video && par->codec_id == AV_CODEC_ID_MPEG4) {
        if (av_bsf_alloc(av_bsf_get_by_name("mpeg4_unpack_bframes"), &filter) < 0) return 3;
        avcodec_parameters_copy(filter->par_in, par);
        filter->time_base_in = ctx->streams[index]->time_base;
        if (av_bsf_init(filter) < 0) return 3;
    }
    FILE *out = fopen(argv[2], "wb");
    kpk_put(out, video ? KPK_VIDEO_MAGIC : KPK_MAGIC);
    kpk_put(out, par->codec_id);
    if (video) {
        kpk_put(out, par->codec_tag);
        kpk_put(out, (uint32_t)par->width);
        kpk_put(out, (uint32_t)par->height);
    } else {
        kpk_put(out, (uint32_t)par->sample_rate);
        kpk_put(out, (uint32_t)par->ch_layout.nb_channels);
        kpk_put(out, (uint32_t)par->block_align);
    }
    kpk_put(out, (uint32_t)par->extradata_size);
    fwrite(par->extradata, 1, (size_t)par->extradata_size, out);
    AVPacket *packet = av_packet_alloc();
    int count = 0;
    while (av_read_frame(ctx, packet) >= 0) {
        if (packet->stream_index != index) {
            av_packet_unref(packet);
            continue;
        }
        if (filter == NULL) {
            packet_put(out, packet, &count);
            av_packet_unref(packet);
            continue;
        }
        /* as demuxer.library: one packet in, at most one out */
        if (av_bsf_send_packet(filter, packet) == 0 && av_bsf_receive_packet(filter, packet) == 0)
            packet_put(out, packet, &count);
        av_packet_unref(packet);
    }
    fclose(out);
    printf("%s: codec=%d packets=%d\n", argv[1], par->codec_id, count);
    return 0;
}
