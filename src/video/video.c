#include <stdbool.h>
#include <SDL.h>
#include <SDL_video.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include "video.h"
#include "../launcher.h"
#include "../debug.h"

#define FRAME_BLOCK 64

struct sFrame {
	uint8_t** buffer;
	Uint32 displayTimeEnd;
}
struct sFrame;
typedef struct sFrame sFrame;

struct sFrames{
	sFrame head[FRAME_BLOCK];
	int headCount;
	struct sFrames* tail;
};
struct sFrames;
typedef struct sFrames sFrames;

extern struct Config config;
extern struct State state;
extern struct Geometry geo;
extern SDL_Renderer *renderer;
SDL_Thread *video_load_thread = NULL;
SDL_Thread *video_render_thread = NULL;

//RENDERING:
SDL_Texture *video_texture = NULL;
Uint32 baseTicks = 0;
sFrames* renderingFrame = NULL;
int renderingFrameIndex = 0;
short showedFirstFrame = 0;
unsigned int renderedFrameCount = 0;

//SHARED:
volatile short RENDER_VIDEO = 0;
volatile sFrames *frames = NULL;
volatile Uint32 frameMs = UINT32_MAX;

//LOADING:
AVCodecContext *decoder = NULL;
sFrames *lastFrame = NULL;
int lastFrame_loadedFrames = FRAME_BLOCK;
unsigned int  loadedFrameCount = 0;
short dumpAudio = 0;

static sFrame* select_rendering_frame();
static int draw_video_async(void*);

static int load_video_async(void*);
static void ensure_loading_frame();
static int load_video_step();

/** FROM https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c */
/** FROM https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/scale_video.c */
AVFormatContext *input_ctx = NULL;
int video_stream, ret;
AVStream *video = NULL;
AVCodecContext *decoder_ctx = NULL;
const AVCodec *decoder = NULL;
AVPacket *packet = NULL;
enum AVHWDeviceType type;
struct SwsContext *sws_ctx;


static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type);
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts);
static sFrame decode_frame(AVCodecContext *avctx, AVPacket *packet);
static int init_ffmpeg_video(char* fileName);
static void cleanup_ffmpeg_video();
static sFrame decode_ffmpeg();
static int scale_init(int src_w, int src_h, enum AVPixelFormat src_pix_fmt)
static uint8_t** scale_frame(uint8_t** src_frame);

static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        printf("Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }

    printf("Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

static sFrame decode_frame(AVCodecContext *avctx, AVPacket *packet)
{
    AVFrame *frame = NULL, *sw_frame = NULL;
    AVFrame *tmp_frame = NULL;
	if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
		printf("Can not alloc frame\n");
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	const int ret = avcodec_receive_frame(avctx, frame);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
		av_frame_free(&frame);
		av_frame_free(&sw_frame);
		return NULL;
	} else if (ret < 0) {
		printf("Error while decoding\n");
		goto fail;
	}

	if (frame->format == hw_pix_fmt) {
		/* retrieve data from GPU to CPU */
		if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
			printf("Error transferring the data to system memory\n");
			goto fail;
		}
		tmp_frame = sw_frame;
	} else {
		tmp_frame = frame;
	}
	int size = av_image_get_buffer_size(tmp_frame->format, tmp_frame->width, tmp_frame->height, 1);
	uint8_t *buffer = av_malloc(size);
	if (!buffer) {
		printf("Can not alloc buffer\n");
		goto fail;
	}
	if (av_image_copy_to_buffer(buffer, size,
									(const uint8_t * const *)tmp_frame->data,
									(const int *)tmp_frame->linesize, tmp_frame->format,
									tmp_frame->width, tmp_frame->height, 1) < 0) {
		printf("Can not copy image to buffer\n");
		goto fail;
	}
	sFrame result;
	result.buffer = buffer;
	result.displayTimeEnd = tmp_frame -> time_base * (tmp_frame -> pts + tmp_frame->duration);
	return result;
    fail:
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_freep(&buffer);
    	return NULL;
            
}

static int init_ffmpeg_video(char* fileName) {
	type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
	if (type == AV_HWDEVICE_TYPE_NONE) {
		printf("Failed to find any Hardware Device!\n")
		return -1;
	}

    packet = av_packet_alloc();
    if (!packet) {
        printf("Failed to allocate AVPacket\n");
        return -2;
    }

    /* open the input file */
    if (avformat_open_input(&input_ctx, fileName, NULL, NULL) != 0) {
        printf("Cannot open input file '%s'\n", argv[2]);
        return -3;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        printf("Cannot find input stream information.\n");
        return -4;
    }

    /* find the video stream information */
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        printf("Cannot find a video stream in the input file\n");
        return -5;
    }
    video_stream = ret;
	
    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            printf("Decoder %s does not support device type %s.\n",
                    decoder->name, av_hwdevice_get_type_name(type));
            type = av_hwdevice_iterate_types(type);
			if (type == AV_HWDEVICE_TYPE_NONE) {
				printf("No more usable hardware device types!\n");
				return -6;
			}
			i = 0;
			continue;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            hw_pix_fmt = config->pix_fmt;
            break;
        }
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return -7; //AVERROR(ENOMEM);

    video = input_ctx->streams[video_stream];
    if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
        return -8;

    decoder_ctx->get_format = get_hw_format;

    if (hw_decoder_init(decoder_ctx, type) < 0)
        return -9;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
        printf("Failed to open codec for stream #%u\n", video_stream);
        return -10;
    }

	if(scale_init() < 0) {
		return -11;
	}

	return 0;
}

static void cleanup_ffmpeg_video() {
	av_packet_free(&packet);
	avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);
	sws_freeContext(sws_ctx);
}

static sFrame decode_ffmpeg() {
	/* actual decoding and dump the raw data */
	if (av_read_frame(input_ctx, packet) < 0) {
		printf("Received no frame from decoder\n");
       return NULL;
	}
	
    if (avcodec_send_packet(avctx, packet) < 0) {
        printf("Error during decoding\n");
        return NULL;
    }
	if (video_stream != packet->stream_index) {
		av_packet_unref(packet);
		return NULL;
	}
	sFrame result = decode_frame(decoder_ctx, packet);
	av_packet_unref(packet);
	const uint8_t** buffer = scale_frame(buffer);
	av_freep(result.buffer);
	result.buffer = buffer;
	return result;
}

void scale_init(int src_w, int src_h, enum AVPixelFormat src_pix_fmt) {
	const enum AVPixelFormat dst_pix_fmt = AV_PIX_FMT_RGB24;
	sws_ctx = sws_getContext(src_w, src_h, src_pix_fmt,
                             geo.screen_width, geo.screen_height, dst_pix_fmt,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        printf(
                "Impossible to create scale context for the conversion "
                "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                av_get_pix_fmt_name(src_pix_fmt), src_w, src_h,
                av_get_pix_fmt_name(dst_pix_fmt),
				geo.screen_width,
				geo.screen_height
		);
        goto end;
    }
	
	return;
	end:
    	sws_freeContext(sws_ctx);
}

uint8_t** scale_frame(uint8_t **src_frame) {
	uint8_t **dst_data;
	if ((av_image_alloc(
			dst_data,
			dst_linesize,
			geo.screen_width,
			geo.screen_height,
			dst_pix_fmt,
			1
		)) < 0) {
        printf("Could not allocate destination frame\n");
        return NULL;
    }
	if (sws_scale(
		sws_ctx,
		(const uint8_t * const*)src_frame,
		src_linesize,
		0,
		src_h,
		dst_data,
		dst_linesize
	) < 0) {
		printf("Failed to scale frame\n");
		av_freep(dst_data);
		return NULL;
	}
	return dst_data;
}

/**FFMPEG EXAMPLE COPY END */

void init_video(char* fileName) {
	if (fileName == NULL) {
		log_error("No file name was defined, but video mode selected");
		return;
	}
	RENDER_VIDEO = 1;
	video_load_thread = SDL_CreateThread(load_video_async, "Video Loading Thread", (void*) NULL);
	video_render_thread = SDL_CreateThread(draw_video_async, "Video Render Thread", (void*) NULL);
}

void cleanup_video() {
	RENDER_VIDEO = 0;
	SDL_WaitThread(video_load_thread, NULL);
	SDL_WaitThread(video_render_thread, NULL);
        video_load_thread = NULL;
	video_render_thread = NULL;
	free(video_texture);
	video_texture = NULL;
	sFrames* iter = (sFrames*) frames;
	while (iter != NULL) {
		sFrames* tmp = iter;
		iter = iter -> tail;
		for(int i=0;i<iter ->headCount;i++) {
			av_freep(tmp -> head[i]);
		}
		free(tmp);
	}
	frames = NULL;
	lastFrame = NULL;
	renderingFrame = NULL;
	THEORAPLAY_stopDecode(decoder);
	decoder = NULL;
}

static sFrame* select_rendering_frame() {
	selectFrameStart:
	if (renderingFrame == NULL) {
		if (frames == NULL) {
			//BUFFERING, Show nothing:
			return NULL;
		}
		if (frames -> headCount < 0) {
			//BUFFERING, Show first frame: (HACK)
			if (showedFirstFrame != 0 || frames -> head[0] == NULL) {
				return NULL;
			}
			log_debug("Selecting first frame to be rendered while buffering");
			showedFirstFrame = 1;
			return frames -> head[0];
		}
		//Show first frame of first block:
		renderingFrame = (sFrames*) frames;
		renderingFrameIndex = 0;
		renderedFrameCount = 0;
		baseTicks = SDL_GetTicks();
		log_debug("Rendering first frame of first block");
		return renderingFrame -> head[0];
	}
	const Uint32 currentTime = SDL_GetTicks() - baseTicks;
	sFrame currentFrame = renderingFrame -> head[renderingFrameIndex];
	if (currentTime > currentFrame.displayTimeEnd) {
			//No progress yet; Show same frame;
			return NULL;
		}
	if (nextIndex < renderingFrame -> headCount) {
		//Show next frame:
		log_debug("Render next frame %d", nextIndex);
		renderingFrameIndex++;
		return &renderingFrame -> head[renderingFrameIndex];
	}
	if (renderingFrame -> tail != NULL && renderingFrame -> tail -> headCount < 0) {
		//BUFFERING; Show same frame:
		return NULL;
	}
	//Reached end of block go to next:
	renderedFrameCount += renderingFrame -> headCount;
	log_debug("Rendered frames: %d", renderedFrameCount);
	renderingFrameIndex = -1;
	renderingFrame = renderingFrame -> tail;
	goto selectFrameStart;
}

static int draw_video_async(void* nothing) {
	void *texture_pixels = NULL;
	int pitch = 0;
	const sFrame* currentFrame = NULL;
	video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA, SDL_TEXTUREACCESS_STREAMING, geo -> screen_width, geo -> screen_height);
	if (video_texture == NULL) {
		log_error("Failed to load video texture");
		return -1;
	}
	while(RENDER_VIDEO) {
		if (currentFrame == NULL) {
			log_debug("Did not retrieve new frame, not updating texture");
			SDL_Delay(10);//TODO: WE COULD WAIT EXACTLY UNTIL THE FRAME ENDS...
			currentFrame = select_rendering_frame();
			continue;
		}
		log_debug("Starting draw to video texture");
		SDL_LockTexture(video_texture, NULL, &texture_pixels, &pitch);
		memcpy(texture_pixels, currentFrame, geo->screen_height * pitch);
		SDL_UnlockTexture(video_texture);
		log_debug("Finished drawing to texture, retrieving new frame");
		currentFrame = select_rendering_frame();
	}
	return 0;
}

void render_video_texture() {
	if(video_texture == NULL) {
		return;
	}
	SDL_RenderCopy(renderer, video_texture, NULL, NULL);
}

static int load_video_async(void* nothing) {
	const int startCode = init_ffmpeg_video(fileName);
	if (startCode != 0) {
		log_error("Failed to setup decoder");
		return -1;
	}
	while (RENDER_VIDEO && load_video_step() == 0) { ;; }
	loadedFrameCount += lastFrame_loadedFrames;
	log_debug("Finished decoding of video, frames: %d", loadedFrameCount);
	lastFrame -> headCount = lastFrame_loadedFrames;
	cleanup_ffmpeg_video();
	return 0;
}

static int load_video_step() {
	ensure_loading_frame();
	sFrame videoFrame = decode_ffmpeg();
	if (videoFrame == NULL) {
		return -1;
	}
	lastFrame -> head[lastFrame_loadedFrames++] = videoFrame;
	return 0;
}

static void ensure_loading_frame() {
	if (lastFrame_loadedFrames < FRAME_BLOCK) {
		return;
	}
	log_debug("Creating new frame block");
	sFrames *newFrame = (sFrames*) calloc(1, sizeof(sFrames));
	newFrame -> headCount = -1;
	if (lastFrame == NULL) {
		frames = newFrame;
	} else {
		lastFrame -> tail = newFrame;
		lastFrame -> headCount = lastFrame_loadedFrames;
		loadedFrameCount += lastFrame_loadedFrames;
		log_debug("Loaded frames %d", loadedFrameCount);
	}
	lastFrame = newFrame;
	lastFrame_loadedFrames = 0;
}
