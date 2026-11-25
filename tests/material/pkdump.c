/* host: pkdump in.mkv out.kpk : the first audio stream's packets */
#include <libavformat/avformat.h>
#include "kpk.h"
int main(int argc, char **argv)
{
    AVFormatContext *ctx = NULL;
    if (argc < 3 || avformat_open_input(&ctx, argv[1], NULL, NULL) < 0) return 1;
    avformat_find_stream_info(ctx, NULL);
    int index = -1;
    for (unsigned i = 0; i < ctx->nb_streams; i++)
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { index = (int)i; break; }
    if (index < 0) return 2;
    AVCodecParameters *par = ctx->streams[index]->codecpar;
    FILE *out = fopen(argv[2], "wb");
    kpk_put(out, KPK_MAGIC);
    kpk_put(out, par->codec_id);
    kpk_put(out, (uint32_t)par->sample_rate);
    kpk_put(out, (uint32_t)par->ch_layout.nb_channels);
    kpk_put(out, (uint32_t)par->block_align);
    kpk_put(out, (uint32_t)par->extradata_size);
    fwrite(par->extradata, 1, (size_t)par->extradata_size, out);
    AVPacket *packet = av_packet_alloc();
    int count = 0;
    while (av_read_frame(ctx, packet) >= 0) {
        if (packet->stream_index == index) {
            kpk_put(out, (uint32_t)packet->size);
            fwrite(packet->data, 1, (size_t)packet->size, out);
            count++;
        }
        av_packet_unref(packet);
    }
    fclose(out);
    printf("%s: codec=%d rate=%d ch=%d packets=%d\n", argv[1], par->codec_id, par->sample_rate,
           par->ch_layout.nb_channels, count);
    return 0;
}
