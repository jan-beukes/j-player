#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include <raylib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define MIN_WINDOW_HEIGHT 200
#define DEFAULT_WINDOW_HEIGHT 600

#define TIME_FONT_SCALE 0.04f
#define PAUSE_FONT_SCALE 0.1f

#define ERROR(fmt, ...) ({ fprintf(stderr, "ERROR: "fmt"\n", ##__VA_ARGS__); exit(1); })
#define LOG(fmt, ...) printf("LOG: "fmt"\n", ##__VA_ARGS__)
#define WARN(fmt, ...) printf("WARN: "fmt"\n", ##__VA_ARGS__)

// Queue stuff
#define QUEUE_SIZE(Q) ({ \
    int __S; \
    if (Q.windex > Q.rindex) __S = Q.windex - Q.rindex; \
    else __S = Q.cap - Q.rindex + Q.windex + 1; \
    __S; \
})

#define QUEUE_EMPTY(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    bool _V = Q.rindex == Q.windex; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

#define QUEUE_FULL(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    bool _V = (Q.windex + 1) % Q.cap == Q.rindex; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

#define QUEUE_BACK(Q, W) ({ \
    pthread_mutex_lock(&Q.mutex); \
    W = Q.items[Q.windex]; \
    pthread_mutex_unlock(&Q.mutex); \
})

#define QUEUE_INC(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    if (++Q.windex >= Q.cap) Q.windex = 0; \
    pthread_mutex_unlock(&Q.mutex); \
})

#define DEQUEUE(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    __typeof__(*Q.items) _V = Q.items[Q.rindex]; \
    if (++Q.rindex >= Q.cap) Q.rindex = 0; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

#define QUEUE_PEEK(Q) ({ \
    pthread_mutex_lock(&Q.mutex); \
    __typeof__(*Q.items) _V = Q.items[Q.rindex]; \
    pthread_mutex_unlock(&Q.mutex); \
    _V; \
})

typedef struct {
    AVFormatContext *format_ctx; // The context of the opened the file/stream
    AVFormatContext *format_ctx2; // used for split audio

    // Codecs
    AVCodecContext *v_ctx;
    int v_index;
    AVCodecContext *a_ctx;
    int a_index;

    AVFrame *out_frame;
    struct SwsContext *sws_ctx;

    bool video_active;
    bool decoding_active;
    bool paused;
    int fps;
    double video_time;
    double pause_time;
    double start_time;
} VideoContext;

#define FRAME_QUEUE_CAP 16
typedef struct FrameQueue {
    AVFrame **items;
    int cap;
    int windex;
    int rindex;
    pthread_mutex_t mutex;
} FrameQueue;

#define PACKET_QUEUE_CAP 32
typedef struct PacketQueue {
    AVPacket **items;
    int cap;
    int windex;
    int rindex;
    pthread_mutex_t mutex;
} PacketQueue;

// Globals
PacketQueue packets = {0};
FrameQueue v_queue = {0};
FrameQueue a_queue = {0};

bool pressed_last_frame = false;
int press_frame_count = 0;

char *get_time_string(char *buf, int seconds)
{
    if (seconds < 60*60) {
        int s = seconds % 60;
        int m = seconds / 60;
        sprintf(buf, "%02d:%02d", m, s);
        return buf;
    } else {
        int s = seconds % 60;
        int m = (seconds / 60) % 60;
        int h = seconds / (60*60) % 60;
        sprintf(buf, "%02d:%02d:%02d", h, m, s);
        return buf;
    }

}
 
// initialize format context from youtube url
#define BUF_MAX_LEN 2048
#define DEFAULT_ARGS "-f 'b*[height<=1080]+ba'"
void init_format_yt(VideoContext *ctx, char *video_file, char *yt_dlp_args)
{
    LOG("initializing youtube streaming...");

    char cmd[BUF_MAX_LEN];
    if (yt_dlp_args == NULL) yt_dlp_args = DEFAULT_ARGS;
    snprintf(cmd, BUF_MAX_LEN, "yt-dlp %s --get-url %s", yt_dlp_args, video_file);
    FILE *yt_stdout = popen(cmd, "r");
    if (yt_stdout == NULL) ERROR("popen");

    // Read the urls from yt-dlp stdout
    char url_buf[BUF_MAX_LEN];
    char url_buf2[BUF_MAX_LEN];
    fgets(url_buf, BUF_MAX_LEN, yt_stdout);
    char *ret = fgets(url_buf2, BUF_MAX_LEN, yt_stdout);

    if (pclose(yt_stdout) != 0) ERROR("failed to retrieve video with yt-dlp");

    // open the video file from url
    ctx->format_ctx = avformat_alloc_context();
    if (avformat_open_input(&ctx->format_ctx, url_buf, NULL, NULL) != 0)
        ERROR("Could not open youtube video %s", video_file);
    if (avformat_find_stream_info(ctx->format_ctx, NULL) < 0)
        ERROR("Could not find stream info");
    LOG("Format %s", ctx->format_ctx->iformat->long_name);

    // if two streams were returned open the audio file into format_ctx2
    if (ret != NULL) {
        ctx->format_ctx2 = avformat_alloc_context();
        if (avformat_open_input(&ctx->format_ctx2, url_buf2, NULL, NULL) != 0)
            ERROR("Could not open youtube video %s", video_file);
        if (avformat_find_stream_info(ctx->format_ctx2, NULL) < 0)
            ERROR("Could not find stream info");
    }
}

// youtube also has the youtu.be domain
#define YT_DOMAINS {"https://www.youtu", "https://youtu", "youtu"}
void init_av_streaming(VideoContext *ctx, char *video_file, char *yt_dlp_args)
{
    av_log_set_level(AV_LOG_ERROR);
    LOG("LOADING VIDEO");

    //---Format---
    bool yt_url = false;
    const char *domains[] = YT_DOMAINS;
    for (int i = 0; i < 3; i++) {
        if (strncmp(video_file, domains[i], strlen(domains[i])) == 0)
            yt_url = true;
    }
    if (yt_url) {
        init_format_yt(ctx, video_file, yt_dlp_args);
    } else {
        ctx->format_ctx = avformat_alloc_context();
        // allocate format context and read format from file
        if (avformat_open_input(&ctx->format_ctx, video_file, NULL, NULL) != 0)
            ERROR("Could not open video file %s", video_file);

        LOG("Format %s", ctx->format_ctx->iformat->long_name);
        // find the streams in the format
        if (avformat_find_stream_info(ctx->format_ctx, NULL) < 0)
            ERROR("Could not find stream info");
    }

    //---Codecs---
    int ret;
    const AVCodec *codec;

    // find video stream
    ret = av_find_best_stream(ctx->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ret < 0 && ctx->format_ctx2) {
        ret = av_find_best_stream(ctx->format_ctx2, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (ret < 0) ERROR("Could not find a video stream");
        // swap so that video is in ctx 1
        AVFormatContext *tmp = ctx->format_ctx;
        ctx->format_ctx = ctx->format_ctx2;
        ctx->format_ctx2 = tmp;
    }
    ctx->v_index = ret;
    ctx->v_ctx = avcodec_alloc_context3(codec);
    // create vido codec ctx
    if (avcodec_parameters_to_context(ctx->v_ctx, 
        ctx->format_ctx->streams[ctx->v_index]->codecpar) < 0)
        ERROR("could not create video codec context");

    // setup fps and time_base for vido ctx
    AVRational framerate = ctx->format_ctx->streams[ctx->v_index]->avg_frame_rate;
    ctx->fps = framerate.num / ctx->v_ctx->framerate.den;
    ctx->v_ctx->time_base = ctx->format_ctx->streams[ctx->v_index]->time_base;
    LOG("Video %dx%d at %dfps", ctx->v_ctx->width, ctx->v_ctx->height, ctx->fps);
    LOG("Codec %s ID %d bitrate %ld", codec->long_name, codec->id, ctx->v_ctx->bit_rate);

    // initialize audio codec
    // if we are using seperated streams then audio must be in context 2
    AVFormatContext *audio_ctx = ctx->format_ctx2 ? ctx->format_ctx2 : ctx->format_ctx;

    ctx->a_index = av_find_best_stream(audio_ctx, AVMEDIA_TYPE_AUDIO, -1, ctx->v_index, &codec, 0);
    if (ctx->a_index < 0) ERROR("Could not find audio stream");
    ctx->a_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(ctx->a_ctx,
        audio_ctx->streams[ctx->a_index]->codecpar) < 0)
        ERROR("could not create audio codec context");

    LOG("Audio %d chanels, sample rate %dHZ", 
        ctx->a_ctx->ch_layout.nb_channels, ctx->a_ctx->sample_rate);
    LOG("Codec %s ID %d bitrate %ld", codec->long_name, codec->id, ctx->v_ctx->bit_rate);

    // open the initialized codecs for use
    if (avcodec_open2(ctx->v_ctx, ctx->v_ctx->codec, NULL) < 0)
        ERROR("Could not open video codec");
    if (avcodec_open2(ctx->a_ctx, ctx->a_ctx->codec, NULL) < 0)
        ERROR("Could not open audio codec");
     
    ctx->decoding_active = true;
    ctx->video_active = true;
    return;
}

void deinit_av_streaming(VideoContext *ctx)
{
    // free queues
    for (int i = 0; i < v_queue.cap; i++)
        av_frame_free(&v_queue.items[i]);
    for (int i = 0; i < a_queue.cap; i++)
        av_frame_free(&a_queue.items[i]);
    for (int i = 0; i < packets.cap; i++)
        av_packet_free(&packets.items[i]);

    av_frame_free(&ctx->out_frame);
    avcodec_close(ctx->v_ctx);
    avcodec_close(ctx->a_ctx);
    avcodec_free_context(&ctx->v_ctx);
    avcodec_free_context(&ctx->a_ctx);
    avformat_close_input(&ctx->format_ctx);
    avformat_free_context(ctx->format_ctx);
    if (ctx->format_ctx2) {
        avformat_close_input(&ctx->format_ctx2);
        avformat_free_context(ctx->format_ctx2);
    }
    sws_freeContext(ctx->sws_ctx);
}

void init_frame_conversion(VideoContext *ctx)
{
    enum AVPixelFormat format = ctx->v_ctx->pix_fmt;
    int vid_width = ctx->v_ctx->width, vid_height = ctx->v_ctx->height;
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    ctx->sws_ctx = sws_getContext(vid_width, vid_height, format, 
                                                vid_width, vid_height, AV_PIX_FMT_RGB24,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (ctx->sws_ctx == NULL)
        ERROR("Failed to get sws context");

    ctx->out_frame = av_frame_alloc();
    unsigned char *frame_buf = malloc(num_bytes);
    int ret = av_image_fill_arrays(ctx->out_frame->data, ctx->out_frame->linesize,
                         frame_buf, AV_PIX_FMT_RGB24, vid_width, vid_height, 1);
    if (ret < 0) ERROR("Could not initialize out frame image");
    ctx->out_frame->width = vid_width;
    ctx->out_frame->height = vid_height;
}

void update_frames(Texture surface, VideoContext *ctx)
{
    if (QUEUE_EMPTY(v_queue)) {
        return;
    }

    // Time updates
    if (ctx->start_time == 0.0) ctx->start_time = GetTime();
    double time = GetTime() - ctx->start_time;

    AVFrame *frame = QUEUE_PEEK(v_queue);
    assert(frame != NULL);
    double next_ts = frame->pts * av_q2d(ctx->v_ctx->time_base);

    if (time >= next_ts) {
        ctx->video_time = next_ts;
        DEQUEUE(v_queue);

        // convert to rgb
        if (frame->data[0] == NULL) ERROR("NULL Frame");
        sws_scale_frame(ctx->sws_ctx, ctx->out_frame, frame);
        UpdateTexture(surface, ctx->out_frame->data[0]);
        av_frame_unref(frame);
    }

}

void *io_thread_func(void *arg)
{
    VideoContext *ctx = (VideoContext *)arg;
    AVPacket *packet;
    int ret = 0;

    while (ret != AVERROR_EOF) {
        if (IsWindowReady() && WindowShouldClose()) break;
        if (QUEUE_FULL(packets)) continue;
        QUEUE_BACK(packets, packet);
        ret = av_read_frame(ctx->format_ctx, packet);
        // TODO: ctx2 reading
        if (ret != 0 && ret != AVERROR_EOF) {
            WARN("reading frame, %s", av_err2str(ret));
        } else {
            QUEUE_INC(packets);
        }
    }

    return NULL;
}

void *decode_thread_func(void *arg)
{
    VideoContext *ctx = (VideoContext *)arg;
    int ret;

    while (ctx->decoding_active) {
        if (IsWindowReady() && WindowShouldClose()) break;
        if (QUEUE_EMPTY(packets)) continue;
        if (QUEUE_FULL(v_queue)) continue;

        AVPacket *packet = DEQUEUE(packets);

        // TODO: Audio
        if (packet->stream_index != ctx->v_index) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(ctx->v_ctx, packet);
        av_packet_unref(packet);
        if (ret != 0 && ret != AVERROR_EOF) {
            WARN("sending packet, %s", av_err2str(ret));
            continue;
        }

        AVFrame *frame;
        QUEUE_BACK(v_queue, frame);
        ret = avcodec_receive_frame(ctx->v_ctx, frame);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret == AVERROR_EOF) {
            // Video done
            ctx->decoding_active = false;
            LOG("VIDEO DECODING DONE");
            break;
        } else if (ret < 0) {
            WARN("receiving frame, %s", av_err2str(ret));
            continue;
        }
        QUEUE_INC(v_queue);
    }
    return NULL;
}

void start_threads(VideoContext *ctx)
{
    pthread_t io_thread, decode_thread;
    pthread_mutex_init(&packets.mutex, NULL);
    pthread_mutex_init(&v_queue.mutex, NULL);
    pthread_mutex_init(&a_queue.mutex, NULL);
    pthread_create(&io_thread, NULL, io_thread_func, ctx);
    pthread_create(&decode_thread, NULL, decode_thread_func, ctx);
    pthread_detach(io_thread);
    pthread_detach(decode_thread);
}

#define USAGE() fprintf(stderr, "USAGE: %s <input file/url>\nyt-dlp: %s [-- OPTIONS] <url>", argv[0], argv[0])

int main(int argc, char *argv[])
{
    //---Arguments---
    if (argc < 2 || argc == 3) {
        USAGE();
        return 1;
    }
    char *video_file;
    char yt_dlp_buf[1024];
    char *yt_dlp_args = NULL;
    if (argc > 3) {
        char *sep = argv[1];
        if (strcmp(sep, "--") != 0) {
            USAGE();
            return 1;
        }
        strncpy(yt_dlp_buf, argv[2], 1024);
        for (int i = 3; i < argc - 1; i++) {
            size_t len = strlen(yt_dlp_buf);
            if (len < 1023) {
                yt_dlp_buf[len] = ' ';
                yt_dlp_buf[len + 1] = '\0';
            }
            size_t n = 1024 - len;
            strncat(yt_dlp_buf, argv[i], n);
        }
        video_file = argv[argc - 1];
        yt_dlp_args = yt_dlp_buf;
    } else {
        video_file = argv[1];
    }

    // Initialization
    VideoContext ctx = {0};
    init_av_streaming(&ctx, video_file, yt_dlp_args);
    init_frame_conversion(&ctx);

    // packets
    packets.items = av_malloc_array(PACKET_QUEUE_CAP, sizeof(AVPacket *));
    packets.cap = PACKET_QUEUE_CAP;
    for (int i = 0; i < packets.cap; i++) packets.items[i] = av_packet_alloc();
    // video queue
    v_queue.items = av_malloc_array(FRAME_QUEUE_CAP, sizeof(AVFrame *));
    v_queue.cap = FRAME_QUEUE_CAP;
    for (int i = 0; i < v_queue.cap; i++) v_queue.items[i] = av_frame_alloc();
    // audio queue
    a_queue.items = av_malloc_array(FRAME_QUEUE_CAP, sizeof(AVFrame *));
    a_queue.cap = FRAME_QUEUE_CAP;
    for (int i = 0; i < a_queue.cap; i++) a_queue.items[i] = av_frame_alloc();

    start_threads(&ctx);

    // Initialize raylib
    int vid_width = ctx.v_ctx->width, vid_height = ctx.v_ctx->height;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE|FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(DEFAULT_WINDOW_HEIGHT * vid_width / vid_height,
               DEFAULT_WINDOW_HEIGHT, video_file);
    SetWindowMinSize(MIN_WINDOW_HEIGHT * vid_width / vid_height, MIN_WINDOW_HEIGHT);

    // Frame buffer
    Image img = {
        .width = vid_width,
        .height = vid_height,
        .mipmaps = 1,        
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8,
        .data = ctx.out_frame->data[0],
    };
    Texture surface = LoadTextureFromImage(img);
    SetTextureFilter(surface, TEXTURE_FILTER_BILINEAR);

    LOG("PLAYING...");
    while (!WindowShouldClose()) {

        if (!ctx.decoding_active && ctx.video_active && QUEUE_EMPTY(v_queue)) {
            // video finished
            deinit_av_streaming(&ctx);
            ctx.video_active = false;
        }

        if (!ctx.paused && ctx.video_active)
            update_frames(surface, &ctx);

        // Rendering
        ClearBackground(BLACK);

        // Handle Window resizing
        Rectangle src = {0, 0, surface.width, surface.height};
        int screen_height = GetScreenHeight();
        int screen_width = GetScreenWidth();
        int height = screen_height;
        int width = screen_height * vid_width / vid_height;
        if (screen_width < width) {
            width = screen_width;
            height = screen_width * vid_height / vid_width;
        }
        int x = (screen_width - width) / 2;
        int y = (screen_height - height) / 2;
        Rectangle dst = {x, y, width, height};
        if (ctx.video_active)
            DrawTexturePro(surface, src, dst, (Vector2){0}, 0, WHITE);

        //---UI-OVERLAY---
        float font_size = height * TIME_FONT_SCALE;
        // Time
        int duration = ctx.format_ctx->duration / AV_TIME_BASE;
        int current_time = ctx.video_time;
        char buf1[128], buf2[128];
        char *cur_time_str = get_time_string(buf1, current_time);
        char *dur_str = get_time_string(buf2, duration);
        const char *text = TextFormat("%s/%s", cur_time_str, dur_str);
        DrawText(text, x, y, font_size, RAYWHITE);

        // Pause
        if (ctx.paused) {
            font_size = height * PAUSE_FONT_SCALE;
            text = TextFormat("Paused");
            int text_width = MeasureText(text, font_size);
            x = (screen_width - text_width) / 2, y = (screen_height - (int)font_size) / 2;
            DrawText(text, x, y, font_size, RED);
        }

        EndDrawing();

        // Events
        if (IsKeyPressed(KEY_SPACE)) {
            if (ctx.paused) {
                ctx.start_time += (GetTime() - ctx.pause_time);
                ctx.paused = false;
            } else {
                ctx.pause_time = GetTime();
                ctx.paused = true;
            }
        }
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (pressed_last_frame) {
                if (IsWindowState(FLAG_WINDOW_MAXIMIZED))
                    ClearWindowState(FLAG_WINDOW_MAXIMIZED);
                else
                    SetWindowState(FLAG_WINDOW_MAXIMIZED);
            }
            pressed_last_frame = true;

        } else if (pressed_last_frame) {
            int fps = (int)(1 / GetFrameTime());
            if (++press_frame_count >= fps / 4) {
                press_frame_count = 0;
                pressed_last_frame = false;
            }
        }

    }
    //if (ctx.video_time) deinit_av_streaming(&ctx);

    return 0;

}
