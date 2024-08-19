#include <stdbool.h>
#include <SDL.h>
#include <SDL_video.h>
#include "theoraplay.h"
#include "video.h"
#include "../launcher.h"
#include "../debug.h"

#define FRAME_BLOCK 64

struct sFrames{
	const THEORAPLAY_VideoFrame* head[FRAME_BLOCK];
	int headCount;
	struct sFrames* tail;
};
struct sFrames;
typedef struct sFrames sFrames;

extern struct Config config;
extern struct State state;
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
THEORAPLAY_Decoder *decoder = NULL;
sFrames *lastFrame = NULL;
int lastFrame_loadedFrames = FRAME_BLOCK;
unsigned int  loadedFrameCount = 0;
short dumpAudio = 0;

static const THEORAPLAY_VideoFrame* select_rendering_frame();
static int draw_video_async(void*);

static int load_video_async(void*);
static void ensure_loading_frame();
static int load_video_step();
static const THEORAPLAY_VideoFrame* load_next_frame();

void init_video(char* fileName) {
	if (fileName == NULL) {
		log_error("No file name was defined, but video mode selected");
		return;
	}
// Already done by launcher.c:
//	if (SDL_VideoInit(NULL) != 0) {
//		log_error("Failed to setup Video");
//		return;
//	}
	decoder = THEORAPLAY_startDecodeFile(fileName, UINT32_MAX, THEORAPLAY_VIDFMT_IYUV, NULL, 1);
	if (decoder == NULL) {
		log_error("Failed to decode file in theora");
		return;
	}
	RENDER_VIDEO = 1;
	video_load_thread = SDL_CreateThread(load_video_async, "Video Loading Thread", (void*) NULL);
	video_render_thread = SDL_CreateThread(draw_video_async, "Video Render Thread", (void*) NULL);
}

void cleanup_video() {
	RENDER_VIDEO = 0;
//	SDL_WaitThread(video_load_thread, NULL);
	SDL_WaitThread(video_render_thread, NULL);
//	SDL_DestroyTexture(video_texture);
//	SDL_VideoQuit();
        video_load_thread = NULL;
	video_render_thread = NULL;
	free(video_texture);
	video_texture = NULL;
	sFrames* iter = (sFrames*) frames;
	while (iter != NULL) {
		sFrames* tmp = iter;
		iter = iter -> tail;
		for(int i=0;i<iter ->headCount;i++) {
			THEORAPLAY_freeVideo(tmp -> head[i]);
		}
		free(tmp);
	}
	frames = NULL;
	lastFrame = NULL;
	renderingFrame = NULL;
	THEORAPLAY_stopDecode(decoder);
	decoder = NULL;
}

static const THEORAPLAY_VideoFrame* select_rendering_frame() {
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
	const THEORAPLAY_VideoFrame* currentFrame = renderingFrame -> head[renderingFrameIndex];
	if ((currentTime - currentFrame -> playms) < frameMs) {
		//No progress yet; Show same frame;
		return NULL;
	}
	if (renderingFrameIndex + 1 < renderingFrame -> headCount) {
		//Show next frame:
		log_debug("Render next frame %d", renderingFrameIndex);
		renderingFrameIndex++;
		return renderingFrame -> head[renderingFrameIndex];
	}
	if (renderingFrame -> tail != NULL && renderingFrame -> tail -> headCount < 0) {
		//BUFFERING; Show same frame:
		return NULL;
	}
	//Reached end of block go to next:
	renderedFrameCount += renderingFrame -> headCount;
	log_debug("Rendered frames: %d", renderedFrameCount);
	renderingFrameIndex = 0;
	renderingFrame = renderingFrame -> tail;
	goto selectFrameStart;
}

static int draw_video_async(void* nothing) {
	void *pixels = NULL;
	int pitch = 0;
	const THEORAPLAY_VideoFrame* currentFrame = NULL;
	while (RENDER_VIDEO) {
		currentFrame = select_rendering_frame();
		if (currentFrame != NULL) {
			log_debug("Received first frame");
			break;
		}
		//log_debug("Waiting for first frame");
		SDL_Delay(10);
	}
	if (!RENDER_VIDEO) {
		return 0;
	}
	log_debug("Retrieved first frame from selector, video has w: %d h: %d", currentFrame -> width, currentFrame -> height);
	video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, currentFrame -> width, currentFrame -> height);
	if (video_texture == NULL) {
		log_error("Failed to load video texture");
		return -1;
	}
	//TODO: SCALE TEXTURE TO WINDOW SIZE
	while(RENDER_VIDEO) {
		if (currentFrame == NULL) {
			log_debug("Did not retrieve new frame, not updating texture");
			SDL_Delay(10);
			currentFrame = select_rendering_frame();
			continue;
		}
		log_debug("Starting draw to video texture");
		SDL_LockTexture(video_texture, NULL, &pixels, &pitch);
		const int w = currentFrame->width;
		const int h = currentFrame->height;
		const Uint8 *y = (const Uint8 *)currentFrame->pixels;
		const Uint8 *u = y + (w * h);
		const Uint8 *v = u + ((w / 2) * (h / 2));
		Uint8 *dst = (Uint8*)pixels;
		int i;

		//memcpy(pixels, video->pixels, video->height * pitch); // For RGBA texture

		for (i = 0; i < h; i++, y += w, dst += pitch) {
			memcpy(dst, y, w);
		}

		for (i = 0; i < h / 2; i++,	u += w / 2, dst += pitch / 2) {
			memcpy(dst, u, w / 2);
		}

		for (i = 0; i < h / 2; i++,	v += w / 2,	dst += pitch / 2) {
			memcpy(dst, v, w / 2);
		}
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
	while (!THEORAPLAY_isInitialized(decoder)) {
		SDL_Delay(10);
		//THEORAPLAY_pumpDecode(decoder, 5);
	}
//	if (!THEORAPLAY_isInitialized(decoder)) {
///		log_debug("Preparing THEORAPLAY");
//		THEORAPLAY_pumpDecode(decoder, 0);
//	}
	if (!THEORAPLAY_hasVideoStream(decoder)) {
		log_error("File was not recognized to contian a video!");
		return -5;
	}
	dumpAudio = THEORAPLAY_hasAudioStream(decoder);
	if (dumpAudio) {
		log_debug("Removing audio from video file");
	}
	if (load_video_step() != 0) {
		log_error("Failed to retrieve first frame from theora");
		return -3;
	}
	const THEORAPLAY_VideoFrame* firstVideoFrame = frames -> head[0];
	if (firstVideoFrame == NULL) {
		log_error("Failed to retrieve any video frame");
		return -1;
	}
	if (firstVideoFrame -> fps == 0.0) {
		frameMs = UINT32_MAX;
		log_error("Failed to determine fps of video");
		return -2;
	}
	frameMs = (Uint32)(1000.0 / firstVideoFrame -> fps);
	while (RENDER_VIDEO && load_video_step() == 0) { ;; }
	loadedFrameCount += lastFrame_loadedFrames;
	log_debug("Finished decoding of video, frames: %d", loadedFrameCount);
	lastFrame -> headCount = lastFrame_loadedFrames;
	return 0;
}

static int load_video_step() {
	ensure_loading_frame();
	while (RENDER_VIDEO && THEORAPLAY_isDecoding(decoder)) {
		const THEORAPLAY_VideoFrame* videoFrame = load_next_frame();
		if (videoFrame == NULL) {
			SDL_Delay(10);
			continue;
		}
		lastFrame -> head[lastFrame_loadedFrames++] = videoFrame;
		return 0;
	}
	return -1;
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

//TODO: ABORT AT SOME POINT?!
static const THEORAPLAY_VideoFrame* load_next_frame() {
	while (1) {
		//THEORAPLAY_pumpDecode(decoder, 5);
		if (THEORAPLAY_decodingError(decoder)) {
			log_error("Encountered decoding error in THEORAPLAY %d", THEORAPLAY_decodingError(decoder));
			return NULL;
		}
		const THEORAPLAY_VideoFrame* video = THEORAPLAY_getVideo(decoder);
		if (video != NULL) {
			return video;
		}
		//log_debug("Still no video... %d", THEORAPLAY_availableVideo(decoder));
		if (dumpAudio) {
			int i = 0;
			while (i++ < 10) {
				const THEORAPLAY_AudioPacket *audio = THEORAPLAY_getAudio(decoder);
				if (audio == NULL) {
					break;
				}
				THEORAPLAY_freeAudio(audio);
			}
		}
		SDL_Delay(10);
	}
	return NULL;
}
