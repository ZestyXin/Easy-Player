#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#define PROJECT_NAME "easy-player"

AVFormatContext *fmt_ctx = NULL;
int vstream_inx = -1, astream_inx = -1;
AVCodecContext *video_dec = NULL;
AVCodecContext *audio_dec = NULL;
struct SwsContext *sws_ctx;
struct SwrContext *swr_ctx;

double video_clock;
double audio_clock;

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_AudioSpec audio;


#define LOG(priority, message...) do {SDL_LogMessage(0, priority, message);} while(0)

static const char * const LOG_LEVEL_STRING[] = {
    NULL,
    "VERBOSE",
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR",
    "CRITICAL",
};

static void log_print(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
    FILE *log_file = (FILE*)userdata;

    char time_str[20] = {0};
    time_t timestamp = time((NULL));
    strftime(time_str, 20, "%F %X", localtime(&timestamp));

    fprintf(log_file, "%s [%s]: (%s, %d) - %s\n", time_str, LOG_LEVEL_STRING[priority], __FILE__, __LINE__, message);
}

void log_init()
{
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_INFO);
    SDL_LogSetOutputFunction(log_print, stderr);
}


typedef struct _QueueItem
{
    void *data;
    struct _QueueItem *prev;
    struct _QueueItem *next;
} QueueItem;

typedef struct {
    QueueItem *front;
    QueueItem *back;
    size_t size;
} Queue;

Queue video_queue = {0};
Queue audio_queue = {0};
pthread_mutex_t video_mutex;
pthread_mutex_t audio_mutex;

QueueItem *quque_new_item(void *data)
{
    QueueItem *q = NULL;
    q = (QueueItem*) malloc(sizeof(QueueItem));
    if (!q) {
        return NULL;
    }

    q->data = data;
    q->prev = NULL;
    q->next = NULL;

    return q;
}
void queue_del_item(QueueItem *q)
{
    free(q);
}

void queue_push(Queue *q, void* data)
{
    QueueItem *qi = quque_new_item(data);

    if (q->size) {
        q->back->next = qi;
        qi->prev = q->back;
        q->back = qi;
    }
    else {
        q->front = qi;
        q->back = qi;
    }

    ++q->size;
}
void *queue_pop(Queue *q)
{
    QueueItem *qi;

    if (q->size < 1) {
        return NULL;
    }
    else if (q->size < 2) {
        qi = q->front;
        q->front = NULL;
        q->back = NULL;
    }
    else {
        qi = q->front;
        q->front = qi->next;
        q->front->prev = NULL;
    }

    void *data = qi->data;
    queue_del_item(qi);

    return data;
}

void init_rescaler()
{
    enum AVPixelFormat src_fmt;
    sws_ctx = sws_alloc_context();
    if (!sws_ctx) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to allocate rescaler");
        return ;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Allocated rescaler");

    switch (video_dec->pix_fmt) {
    case AV_PIX_FMT_YUVJ420P:
        src_fmt = AV_PIX_FMT_YUV420P;
        break;
    case AV_PIX_FMT_YUVJ422P:
        src_fmt = AV_PIX_FMT_YUV422P;
        break;
    case AV_PIX_FMT_YUVJ444P:
        src_fmt = AV_PIX_FMT_YUV444P;
        break;
    case AV_PIX_FMT_YUVJ440P:
        src_fmt = AV_PIX_FMT_YUV440P;
        break;
    case AV_PIX_FMT_YUVJ411P:
        src_fmt = AV_PIX_FMT_YUV411P;
        break;
    default:
        src_fmt = video_dec->pix_fmt;
    }

    av_opt_set_int(sws_ctx, "sws_flags",  SWS_BICUBIC,0);
	av_opt_set_int(sws_ctx, "srcw",       video_dec->width, 0);
	av_opt_set_int(sws_ctx, "srch",       video_dec->height, 0);
	av_opt_set_int(sws_ctx, "src_format", src_fmt,1);
	av_opt_set_int(sws_ctx, "src_range",  1,1);

	av_opt_set_int(sws_ctx, "dstw",       video_dec->width, 0);
	av_opt_set_int(sws_ctx, "dsth",       video_dec->height, 0);
	av_opt_set_int(sws_ctx, "dst_format", AV_PIX_FMT_RGB24, 1);
	av_opt_set_int(sws_ctx, "dst_range",  1, 1);

    if (sws_init_context(sws_ctx, NULL, NULL) < 0) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to initital rescaler");
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
        return ;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Inititaled rescaler");

}

void init_resampler()
{
    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to allocate resampler");
        return;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Allocated resampler");

    av_opt_set_int(swr_ctx, "in_channel_layout",    audio_dec->channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate",       audio_dec->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_dec->sample_fmt, 0);

    av_opt_set_int(swr_ctx, "out_channel_layout",    audio_dec->channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate",       audio_dec->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(swr_ctx) < 0) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to initital resampler");
        swr_free(&swr_ctx);
        swr_ctx = NULL;
        return;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Inititaled resampler");
}

void deinit_converter()
{
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (swr_ctx) swr_free(&swr_ctx);
}

static AVCodecContext *_open_decoder(int stream_index)
{
    AVCodecContext *dec_ctx;

    const AVCodecParameters *dec_par = fmt_ctx->streams[stream_index]->codecpar;
    AVCodec *dec = avcodec_find_decoder(dec_par->codec_id);
    if (!dec) {
        LOG(SDL_LOG_PRIORITY_WARN, "Failed to find %s codec", av_get_media_type_string(dec_par->codec_type));
        return NULL;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Found %s codec", av_get_media_type_string(dec_par->codec_type));

    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
        LOG(SDL_LOG_PRIORITY_WARN, "Could not allocate the %s codec context", av_get_media_type_string(dec_par->codec_type));
        return NULL;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Allocated %s codec", av_get_media_type_string(dec_par->codec_type));

    if (avcodec_parameters_to_context(dec_ctx, dec_par) < 0) {
        LOG(SDL_LOG_PRIORITY_WARN, "Failed to copy %s codec parameters to decoder context", av_get_media_type_string(dec_par->codec_type));
        avcodec_free_context(&dec_ctx);
        return NULL;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Copied codec parameters to %s codec context", av_get_media_type_string(dec_par->codec_type));

    if (avcodec_open2(dec_ctx, dec, (AVDictionary **)0) < 0) {
        LOG(SDL_LOG_PRIORITY_WARN, "Failed to open %s codec", av_get_media_type_string(dec_par->codec_type));
        avcodec_free_context(&dec_ctx);
        return NULL;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Open %s codec context Success", av_get_media_type_string(dec_par->codec_type));

    return dec_ctx;
}

void open_decoder()
{
    video_dec = _open_decoder(vstream_inx);
    audio_dec = _open_decoder(astream_inx);
}

void close_decoder()
{
    if (video_dec) avcodec_free_context(&video_dec);
    if (audio_dec) avcodec_free_context(&audio_dec);
}

int open_file(const char* file_name)
{
    int ret;

    if (avformat_open_input(&fmt_ctx, file_name, NULL, NULL) < 0) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Could not open video file %s", file_name);
        return -1;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Open file success");

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Could not find stream information");
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Found stream information");

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOG(SDL_LOG_PRIORITY_WARN, "Could not Found video stream");
        return -1;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Found video stream index");
    LOG(SDL_LOG_PRIORITY_DEBUG, "Video Stream Index: %d", ret);
    vstream_inx = ret;

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOG(SDL_LOG_PRIORITY_WARN, "Could not found audio stream");
        return -1;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Found audio stream index");
    LOG(SDL_LOG_PRIORITY_DEBUG, "Audio Stream Index: %d", ret);
    astream_inx = ret;

    return 0;
}

void close_file()
{
    avformat_close_input(&fmt_ctx);
}

static AVFrame *_decoding(AVCodecContext *dec_ctx, AVPacket *pkt)
{
    AVFrame *frm = av_frame_alloc();

    if (avcodec_send_packet(dec_ctx, pkt) < 0) {
        LOG(SDL_LOG_PRIORITY_WARN, "Could not send packet to %s codec", av_get_media_type_string(dec_ctx->codec_type));
        return NULL;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Send one Packet to %s codec", av_get_media_type_string(dec_ctx->codec_type));

    if (avcodec_receive_frame(dec_ctx, frm)) {
        LOG(SDL_LOG_PRIORITY_WARN, "Could not receive frame from %s codec", av_get_media_type_string(dec_ctx->codec_type));
        return NULL;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Receive one Frame from %s codec", av_get_media_type_string(dec_ctx->codec_type));

    return frm;
}

int decoding()
{
    AVPacket pkt = {0};
    AVFrame *frm = NULL;

    int ret = av_read_frame(fmt_ctx, &pkt);
    if (ret == AVERROR_EOF) {
        LOG(SDL_LOG_PRIORITY_INFO, "End of video file");
        return AVERROR_EOF;
    }
    else if (ret < 0) {
        LOG(SDL_LOG_PRIORITY_WARN, "Could not read one packet");
        return -1;
    }
    LOG(SDL_LOG_PRIORITY_VERBOSE, "Demuxed one packet");

    if (pkt.stream_index == vstream_inx) {
        frm = _decoding(video_dec, &pkt);
        if (frm) {
            queue_push(&video_queue, (void*)frm);
        }
    }

    if (pkt.stream_index == astream_inx) {
        frm = _decoding(audio_dec, &pkt);
        if (frm) {
            queue_push(&audio_queue, (void*)frm);
        }
    }

    return 0;
}

void play_audio(void *data, uint8_t *stream, int len)
{
    AVFrame *frm = NULL;

    SDL_memset(stream, 0, len);

    pthread_mutex_lock(&audio_mutex);
    if (audio_queue.size == 0) {
        pthread_mutex_unlock(&audio_mutex);
        return ;
    }
    frm = queue_pop(&audio_queue);
    audio_clock = av_q2d(audio_dec->time_base)*frm->pts;
    pthread_mutex_unlock(&audio_mutex);

	SDL_MixAudio(stream, frm->data[0], len, SDL_MIX_MAXVOLUME);
    av_frame_free(&frm);
}

void play_video()
{
    AVFrame *frm = NULL;
    static int64_t last_pts;

    pthread_mutex_lock(&video_mutex);
    if (video_queue.size == 0) {
        pthread_mutex_unlock(&video_mutex);
        return ;
    }
    frm = queue_pop(&video_queue);
    video_clock = av_q2d(video_dec->time_base)*frm->pts;
    pthread_mutex_unlock(&video_mutex);

    double delay = (frm->pts - last_pts)*av_q2d(video_dec->time_base);
    double diff = video_clock - audio_clock;

    if (diff < 0) {
        delay = 0;
    }
    else if (diff > delay) {
        delay *= 2;
    }

    usleep(delay*1000*1000);

    SDL_UpdateTexture(texture, NULL, frm->data[0], frm->linesize[0]);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

int open_device()
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to Init SDL2");
        goto DEVICE_EXIT_0;
    }

    window = SDL_CreateWindow(PROJECT_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, video_dec->width, video_dec->height, 0);
    if (!window) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to create window");
        goto DEVICE_EXIT_1;
    }

    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to create renderer");
        goto DEVICE_EXIT_2;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, 0, video_dec->width, video_dec->height);
    if (!texture) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Failed to create texture");
        goto DEVICE_EXIT_3;
    }

    audio.freq      = audio_dec->sample_rate;
	audio.format    = AUDIO_S16SYS;
	audio.channels  = audio_dec->channels;
	audio.silence   = audio_dec->slices;
	audio.samples   = audio_dec->frame_size;
	audio.callback  = play_audio;

    if (SDL_OpenAudio(&(audio), NULL) < 0) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Could not open audio device");
		goto DEVICE_EXIT_4;
	}

    return 0;

DEVICE_EXIT_4:
    SDL_DestroyTexture(texture);
DEVICE_EXIT_3:
    SDL_DestroyRenderer(renderer);
DEVICE_EXIT_2:
    SDL_DestroyWindow(window);
DEVICE_EXIT_1:
    SDL_Quit();
DEVICE_EXIT_0:
    return -1;
}

void close_device()
{
    SDL_CloseAudio();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void *syncing(void *arg)
{
    int *exit_flag = (int *)arg;
    while(!(*exit_flag)) {
        play_video();
    }

    return NULL;
}

void *save(void *arg)
{
    int *exit_flag = (int*)arg;
    FILE *video, *audio;
    AVFrame *frm;

    video = fopen("video.rgb", "wb");
    audio = fopen("audio.pcm", "wb");

    while(!(*exit_flag) && video_queue.size == 0 && audio_queue.size == 0) {
        pthread_mutex_lock(&video_mutex);
        frm = (AVFrame*)queue_pop(&video_queue);
        fwrite(frm->data[0], frm->linesize[0], frm->height, video);
        pthread_mutex_unlock(&video_mutex);

        pthread_mutex_lock(&audio_mutex);
        frm = (AVFrame*)queue_pop(&audio_queue);
        fwrite(frm->data[0], frm->linesize[0], frm->height, audio);
        pthread_mutex_unlock(&audio_mutex);
    }
}

void play(const char* file_name)
{
    int exit_flag = 0;
    pthread_t play_pid;

    if (open_file(file_name) < 0) {
        exit(-1);
    }

    open_decoder();

    if (video_dec) {
        init_rescaler();
    }
    else if (audio_dec) {
        init_resampler();
    }
    else {
        exit(-1);
    }

    pthread_mutex_init(&video_mutex, NULL);
    pthread_mutex_init(&audio_mutex, NULL);
    if (pthread_create(&play_pid, NULL, &syncing, &exit_flag) < 0) {
        LOG(SDL_LOG_PRIORITY_ERROR, "Create thread failed");
        exit(-1);
    }

    while(!exit_flag) {
        if (video_queue.size > 4 && audio_queue.size > 4) {
            // sleep
        }
        pthread_mutex_lock(&video_mutex);
        pthread_mutex_lock(&audio_mutex);
        exit_flag = decoding();
        pthread_mutex_unlock(&video_mutex);
        pthread_mutex_unlock(&audio_mutex);
    }

    pthread_mutex_destroy(&video_mutex);
    pthread_mutex_destroy(&audio_mutex);
}

int main(int argc, char const *argv[])
{
    if (argc != 2) {
        printf("Usage: %s <video file>\n", argv[0]);
        return 0;
    }

    play(argv[1]);

    return 0;
}
