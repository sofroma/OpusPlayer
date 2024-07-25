#include "SDL.h"
#include "stdio.h"
#include "stdlib.h"
#include "opusfile.h"

#define MAX_TAG_KEY_SIZE 40
#define MAX_TAG_VALUE_SIZE 200

#define TAG_ARTIST_SIZE 80
#define TAG_TITLE_SIZE 200
#define TAG_ALBUM_SIZE 80
#define TAG_DATE_SIZE 16
#define NOW_PLAYING_STR_SIZE 400

/*
48 samples per ms (48000Hz / 1000ms)
16 bits per sample (2 bytes)
2 channels (Stereo)
120 ms (recommended length by opusfile docs)
48 * 2 * 2 * 120 = 23040
*/
#define DECODE_BUFFER_SIZE 23040
#define SOURCE_BUFFER_SIZE 32768

typedef struct App
{
	SDL_AudioFormat format;
	ogg_int64_t pcmTotal;
	ogg_int64_t pcmPos;
	OggOpusFile* of;
	opus_int16* sourceBuffer;
	opus_int16* decodeBuffer;
	char* nowPlayingStr;
	SDL_AudioDeviceID deviceID;
	int samples;
	int savedOffset;
	int preSamples;
	int channels;
	int volume;
	char endReached;
	char closeOnNextTick;
	char shouldClose;
} App;

void streamCallback(void* uData, Uint8* stream, int len);
void parseTags(const OpusTags* tags, char* nowPlayingStr);
void freeMemory(App* app);

int main(int argc, char** argv)
{
	if(argc < 2) {
		SDL_Log("Usage: main [OPTIONS] filename");
		SDL_Log("Options:");
		SDL_Log("-volume (1-100)");
		return 1;
	}
	int userVolume = 0;
	int argIndex = 1;
	for(int a = argIndex; a < argc; a++) {
		if(strcmp(argv[a], "-volume") == 0) {
			if(argc > a + 1) {
				char* volumeEnd;
				userVolume = strtol(argv[a+1], &volumeEnd, 10);
				if(*volumeEnd != '\0') {
					SDL_Log("Invalid volume value! %c", *volumeEnd);
					return 1;
				}
				userVolume = 1.28f * userVolume;
				if(userVolume > SDL_MIX_MAXVOLUME) {
					userVolume = SDL_MIX_MAXVOLUME;
				}
				if(userVolume < 1) {
					userVolume = 1;
				}
				a++;
				argIndex += 2;
			} else {
				SDL_Log("not enough arguments!");
				return 1;
			}
		}
	}
	if(argIndex >= argc) {
		SDL_Log("not enough arguments!");
		return 1;
	}
	if(SDL_Init(SDL_INIT_AUDIO) < 0) {
		SDL_Log("%s", SDL_GetError());
		return 1;
	}
	App app;
	memset(&app, 0, sizeof(app));
	app.volume = userVolume;

	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq = 48000;
	want.format = AUDIO_S16;
	want.channels = 2;
	want.samples = 4096;
	want.userdata = &app;
	want.callback = streamCallback;
	app.deviceID = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if(app.deviceID == 0) {
		SDL_Log("%s", SDL_GetError());
		freeMemory(&app);
		return 1;
	}
	
	app.format = have.format;
	app.samples = have.samples;

	int result;
	app.of = op_open_file(argv[argIndex], &result);
	if(result != 0) {
		SDL_Log("op_open_file failed");
		freeMemory(&app);
		return 2;
	}

	if(!op_seekable(app.of)) {
		SDL_Log("not seekable stream!");
		freeMemory(&app);
		return 2;
	}

	app.channels = op_channel_count(app.of, -1);
	ogg_int64_t pcmTotal = op_pcm_total(app.of, -1);
	app.pcmTotal = pcmTotal;
	if(pcmTotal < 0) {
		SDL_Log("op_pcm_total failed");
		freeMemory(&app);
		return 2;
	}
	int sec = (int)(pcmTotal / 48000);
	opus_int32 bitrate = op_bitrate(app.of, -1);
	if(bitrate < 0) {
		SDL_Log("op_bitrate failed");
		freeMemory(&app);
		return 2;
	}
	char modeStr[7];
	if(app.channels == 1) {
		strcpy(modeStr, "Mono");
	} else {
		strcpy(modeStr, "Stereo");
	}
	int dMins = sec / 60;
	int dSecs = sec - dMins * 60;
	SDL_Log("Duration: %.2i:%.2i, Mode: %s, Bitrate: %i kbps",
		dMins, dSecs, modeStr, bitrate / 1000);
	app.nowPlayingStr = (char*)malloc(NOW_PLAYING_STR_SIZE);
	if(app.nowPlayingStr == NULL) {
		SDL_Log("memory allocation failed!");
		freeMemory(&app);
		return 3;
	}
	app.nowPlayingStr[NOW_PLAYING_STR_SIZE - 1] = '\0';
	strncpy(app.nowPlayingStr, argv[argIndex], NOW_PLAYING_STR_SIZE - 1);
	const OpusTags* tags = op_tags(app.of, -1);
	if(tags != NULL) {
		parseTags(tags, app.nowPlayingStr);
	}

	app.sourceBuffer = (opus_int16*)malloc(SOURCE_BUFFER_SIZE);
	app.decodeBuffer = (opus_int16*)malloc(DECODE_BUFFER_SIZE);
	if(app.sourceBuffer == NULL || app.decodeBuffer == NULL) {
		SDL_Log("memory allocation failed!");
		freeMemory(&app);
		return 3;
	}
	SDL_Event event;
	SDL_PauseAudioDevice(app.deviceID, 0);
	SDL_Log("Now playing: %s", app.nowPlayingStr);
	while(!app.shouldClose)
	{
		while(SDL_PollEvent(&event))
		{
			if(event.type == SDL_QUIT) {
				app.shouldClose = 1;
				printf("\n");
			}
		}
		if(app.closeOnNextTick) {
			app.shouldClose = 1;
		}
		SDL_Delay(10);
	}
	SDL_PauseAudioDevice(app.deviceID, 1);
	freeMemory(&app);
	return 0;
}

void streamCallback(void* uData, Uint8* stream, int len)
{
	memset(stream, 0, len);
	App* app = (App*) uData;
	if(app->endReached) {
		app->closeOnNextTick = 1;
		return;
	}
	int samplesRead = app->preSamples;
	app->preSamples = 0;
	int bufferOffset = app->savedOffset;
	app->savedOffset = 0;
	
	int decRes;
	while(samplesRead < app->samples)
	{
		if(app->channels > 1) {
			decRes = op_read_stereo(app->of, app->decodeBuffer, DECODE_BUFFER_SIZE / 2);
		} else {
			decRes = op_read(app->of, app->decodeBuffer, DECODE_BUFFER_SIZE / 4, NULL);
		}
		samplesRead += decRes;
		if(decRes < 0) {
			app->shouldClose = 1;
			SDL_Log("opusfile: decode error");
			return;
		}
		if(app->channels > 1) {
			memcpy(app->sourceBuffer + bufferOffset / 2, app->decodeBuffer, decRes * 4);
		} else {
			int q = bufferOffset / 2;
			// dublicate mono to stereo
			for(int s = 0; s < decRes; s++) {
				app->sourceBuffer[q] = app->decodeBuffer[s];
				app->sourceBuffer[q+1] = app->decodeBuffer[s];
				q+=2;
			}
		}
		bufferOffset += decRes * 4;
		app->pcmPos = op_pcm_tell(app->of);
		if(app->pcmPos == app->pcmTotal) {
			app->endReached = 1;
			app->preSamples = 0;
			break;
		}
	}
	if(samplesRead > app->samples) {
		app->preSamples = samplesRead - app->samples;
	}
	int bytes = samplesRead * 4;
	int min = len < bytes ? len : bytes;
	if(app->volume == 0) {
		// If no user volume specified, just copy stream as is
		memcpy(stream, app->sourceBuffer, min);
	} else {
		const Uint8* src = (Uint8*) app->sourceBuffer;
		SDL_MixAudioFormat(stream, src, app->format, min, app->volume);
	}
	
	// If we read extra samples, move them to the beginning of buffer
	if(len < bytes) {
		int elems = (bytes - len) / 2;
		for(int i = 0; i < elems; i++) {
			app->sourceBuffer[i] = app->sourceBuffer[len / 2 + i];
		}
		app->savedOffset = elems * 2;
	}
}

void parseTags(const OpusTags* tags, char* nowPlayingStr)
{
	int delimerOffset;

	char key[MAX_TAG_KEY_SIZE];
	char value[MAX_TAG_VALUE_SIZE];
	char artist[TAG_ARTIST_SIZE];
	char title[TAG_TITLE_SIZE];
	char album[TAG_ALBUM_SIZE];
	char date[TAG_DATE_SIZE];

	memset(artist, 0, sizeof(artist));
	memset(title, 0, sizeof(title));
	memset(album, 0, sizeof(album));
	memset(date, 0, sizeof(date));
	for(int i = 0; i < tags->comments; i++) {
		if(tags->comment_lengths[i] > MAX_TAG_KEY_SIZE + MAX_TAG_VALUE_SIZE) {
			continue;
		}
		delimerOffset = 0;
		for(int c = 0; c < tags->comment_lengths[i]; c++) {
			if(tags->user_comments[i][c] == '=') {
				break;
			}
			delimerOffset++;
		}
		if(delimerOffset == 0 || delimerOffset > MAX_TAG_KEY_SIZE - 1) {
			continue;
		}
		strncpy(key, tags->user_comments[i], delimerOffset);
		key[delimerOffset] = '\0';
			int valueLength = tags->comment_lengths[i] - (delimerOffset + 1);
		if(valueLength > MAX_TAG_VALUE_SIZE - 1) {
			continue;
		}
		strncpy(value, tags->user_comments[i] + delimerOffset + 1, tags->comment_lengths[i] - delimerOffset);
		value[tags->comment_lengths[i] - delimerOffset] = '\0';
		if(strcmp(key, "Artist") == 0) {
			strncpy(artist, value, TAG_ARTIST_SIZE - 1);
		}
		if(strcmp(key, "Title") == 0) {
			strncpy(title, value, TAG_TITLE_SIZE - 1);
		}
		if(strcmp(key, "Album") == 0) {
			strncpy(album, value, TAG_ALBUM_SIZE - 1);
		}
		if(strcmp(key, "Date") == 0) {
			strncpy(date, value, TAG_DATE_SIZE - 1);
		}
	}

	int endPos;
	if(title[0] != 0 && artist[0] != 0) {
		endPos = sprintf(nowPlayingStr, "%s - %s", artist, title);
	}
	if(album[0] != 0) {
		nowPlayingStr += endPos;
		endPos = sprintf(nowPlayingStr, " (%s)", album);
	}
	if(date[0] != 0) {
		nowPlayingStr += endPos;
		if(album[0] != 0) {
			nowPlayingStr -= 1;
			sprintf(nowPlayingStr, ", %s)", date);
		} else {
			sprintf(nowPlayingStr, " (%s)", date);
		}
	}
}

void freeMemory(App* app)
{
	if(app->sourceBuffer != NULL) {
		free(app->sourceBuffer);
	}
	if(app->decodeBuffer != NULL) {
		free(app->decodeBuffer);
	}
	if(app->nowPlayingStr != NULL) {
		free(app->nowPlayingStr);
	}
	if(app->of != NULL) {
		op_free(app->of);
	}
	if(app->deviceID != 0) {
		SDL_CloseAudioDevice(app->deviceID);
	}
	SDL_Quit();
}
