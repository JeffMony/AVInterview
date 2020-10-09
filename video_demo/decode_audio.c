#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#define AUDIO_INBUF_SIZE     20480
#define AUDIO_REFILL_THRESH  4096
#define MAX_AUDIO_FRAME_SIZE 192000

int main(int argc, char **argv)
{
    int i, ret;

    //error message
    int err_code;
    char errors[1024];

    int audiostream_index = -1;

    //codec parameters
    int             len         = 0;
    const AVCodec   *codec      = NULL;
    AVCodecContext  *c          = NULL;
    AVFormatContext *pFormatCtx = NULL;

    //file parameters
    FILE *f, *outfile;
    const char *outfilename, *filename;

    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];

    AVPacket avpkt;
    AVFrame *decoded_frame = NULL;

    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(0);
    }

    filename    = argv[1];
    outfilename = argv[2];

    av_init_packet(&avpkt);
    decoded_frame = av_frame_alloc();
    if (!decoded_frame) {
        fprintf(stderr, "Could not allocate audio frame\n");
        exit(1);
    }

    /* open input file, and allocate format context */
    if ((err_code=avformat_open_input(&pFormatCtx, filename, NULL, NULL)) < 0) {
        av_strerror(err_code, errors, 1024);
        fprintf(stderr, "Could not open source file %s, %d(%s)\n", filename, err_code, errors);
        return -1;
    }

    //Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information

    //Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, filename, 0);

    //get index of audio stream
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO) {
            audiostream_index=i;
        }
    }

    c = avcodec_alloc_context3(NULL);
    if (!c) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        exit(1);
    }

    ret = avcodec_parameters_to_context(c, pFormatCtx->streams[audiostream_index]->codecpar);
    if (ret < 0) {
        return -1;
    }

    codec = avcodec_find_decoder(c->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    
    //Out Audio Param
    uint64_t out_channel_layout=AV_CH_LAYOUT_STEREO;

    //AAC:1024  MP3:1152
    int out_nb_samples  = c->frame_size;
    int out_sample_rate = 44100;
    int out_channels    = av_get_channel_layout_nb_channels(out_channel_layout);

    //Out Buffer Size
    int out_buffer_size = av_samples_get_buffer_size(NULL,
                                                     out_channels,
                                                     out_nb_samples,
                                                     AV_SAMPLE_FMT_S16,
                                                     1);

    uint8_t *out_buffer=(uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE*2);
    int64_t in_channel_layout=av_get_default_channel_layout(c->channels);

    struct SwrContext *audio_convert_ctx;
    audio_convert_ctx = swr_alloc();
    audio_convert_ctx = swr_alloc_set_opts(audio_convert_ctx,
                                         out_channel_layout,
                                         AV_SAMPLE_FMT_S16,
                                         out_sample_rate,
                                         in_channel_layout,
                                         c->sample_fmt,
                                         c->sample_rate,
                                         0,
                                         NULL);
    swr_init(audio_convert_ctx);

    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    outfile = fopen(outfilename, "wb");
    if (!outfile) {
        av_free(c);
        exit(1);
    }

    while (1) {

        int ret;
        int i, ch;
        int got_frame = 0;

        ret = av_read_frame(pFormatCtx, &avpkt);
        if (ret == AVERROR(EAGAIN)) {
            av_usleep(100);
            continue;
        }else if (ret < 0) {
            break;
        }

        //如果不是音频则跳过
        if(avpkt.stream_index != audiostream_index){
            av_packet_unref(&avpkt);
            continue;
        }

        len = avcodec_decode_audio4(c, decoded_frame, &got_frame, &avpkt);
        if (len < 0) {
            av_strerror(len, errors, 1024);
            fprintf(stderr, "Error while decoding, err_code:%d, err:%s\n", len, errors);
            exit(1);
        }
        if (got_frame) {
            /* if a frame has been decoded, output it */
            int data_size = av_get_bytes_per_sample(c->sample_fmt);
            if (data_size < 0) {
                /* This should not occur, checking just for paranoia */
                fprintf(stderr, "Failed to calculate data size\n");
                exit(1);
            }
            swr_convert(audio_convert_ctx,
                        &out_buffer, 
                        MAX_AUDIO_FRAME_SIZE,
                        (const uint8_t **)decoded_frame->data, 
                        decoded_frame->nb_samples);

            fwrite(out_buffer, 1, out_buffer_size, outfile);

        }
        avpkt.size -= len;
        avpkt.data += len;
        avpkt.dts = avpkt.pts = AV_NOPTS_VALUE;
    }

    fclose(outfile);

    avcodec_free_context(&c);
    av_frame_free(&decoded_frame);

    return 0;
}