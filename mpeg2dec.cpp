/*
 *
 * mpeg2dec.c
 * Copyright (C) 2000-2003 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * See http://libmpeg2.sourceforge.net/ for updates.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#define __STDC_CONSTANT_MACROS

#include <config.h>

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <conio.h>
#include<excpt.h>
#endif

//#include "config.h"

#define SELFTEST

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#ifdef HAVE_IO_H
#include <fcntl.h>
#endif
#ifdef LIBVO_SDL
#include <SDL/SDL.h>
#endif
#include <inttypes.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
}

#define NOPTS ((int64_t)AV_NOPTS_VALUE)
//#include "mpeg2convert.h"
#include "comskip.h"

#include <algorithm>

namespace CS {
int pass;
double test_pts = 0.0;
int av_log_level;
int	framenum;
bool is_h264;
#ifdef PROCESS_CC
bool reorderCC;
#endif
int ms_audio_delay = 5;
int demux_pid;
int selected_video_pid;
int selected_audio_pid;
int selected_subtitle_pid;
int64_t headerpos;
struct stat instat;
#ifdef _WIN32
bool soft_seeking;
#endif

} // namespace CS

#if 0
#undef AV_TIME_BASE_Q
static AVRational AV_TIME_BASE_Q = {1, AV_TIME_BASE};
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0
#define SAMPLE_CORRECTION_PERCENT_MAX 30
#define AUDIO_DIFF_AVG_NB 10
#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)
#define VIDEO_PICTURE_QUEUE_SIZE 1
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_ADUIO_MASTER

namespace {

using namespace CS;

class VideoPicture
{
public:
	int width;
	int height;
	int allocated;
	double pts;
};

#define AUDIOBUFFER_SIZE 800000
#define MAX_FRAMES_WITHOUT_SOUND 100

class VideoState;

class Audio
{
public:
	Audio(CommercialSkipper *pcs)
		: pcs(pcs)
		, clock(0.0)
		, stream(0)
		, frame()
		, aptsBase(0.0)
		, apts(0.0)
		, bufS16(0)
		, bufS16Ptr(0)
		, bufF32(0)
		, bufF32Ptr(0)
		, soundFrameCounter(0)
		, volumeMax(0)
		, framesWithoutSound(0)
		, framesWithLoudSound(0)
		// , tracks_without_sound(0)
	{
	}

	~Audio()
	{
		if ( bufS16 )
			delete[] bufS16;

		if ( bufF32 )
			delete[] bufF32;
	}

	void soundToFrames(VideoState *is)
	{
		if ( frame.format == AV_SAMPLE_FMT_S16 \
				|| frame.format == AV_SAMPLE_FMT_S16P )
			soundToFramesS16(is);
		else if ( frame.format == AV_SAMPLE_FMT_FLT \
				|| frame.format == AV_SAMPLE_FMT_FLTP )
			soundToFramesF32(is);
		else
		{
			std::string err("Unable to handle frame format " + getSampleFormatString());
			throw FormatException(err);
		}
	}

	int getSampleCount(void) const
	{
		return frame.nb_samples;
	}

	int getChannelCount(void) const
	{
		return stream->codec->channels;
	}

	int getSampleRate(void) const
	{
		return stream->codec->sample_rate;
	}

	int getBytesPerSample(void) const
	{
		return ::av_get_bytes_per_sample((AVSampleFormat)frame.format);
	}

	int getSamplesPerSecond(void) const
	{
		return stream->codec->channels * getSampleRate();
	}

	int getBytesPerSecond(void) const
	{
		return getBytesPerSample() * getSamplesPerSecond();
	}

	int getSamplesPerFrame(void) const
	{
		return (int)((double)getSamplesPerSecond() / pcs->getFps());
	}

	void setCommercialSkipper(CommercialSkipper *pcs)
	{
		this->pcs = pcs;
	}

	AVStream *getStream(void)
	{
		return stream;
	}

	void setStream(AVStream *stream)
	{
		this->stream = stream;
	}

	double getClock(void) const
	{
		return clock;
	}

	void setClock(double t)
	{
		clock = t;
	}

	void resetClock(void)
	{
		clock = 0.0;
	}

	void incrementClock(void)
	{
		clock += (double)getSampleCount() / (double)getSampleRate();
	}

	void incrementClock(int byteCount)
	{
		clock += (double)byteCount / (double)getBytesPerSecond();
	}

	AVFrame &getFrame(void)
	{
		return frame;
	}

	std::string getSampleFormatString(void) const
	{
		return ::av_get_sample_fmt_name((AVSampleFormat)frame.format);
	}

	void allocBufS16(void)
	{
		if ( bufS16 )
			delete[] bufS16;

		bufS16 = new int16_t[AUDIOBUFFER_SIZE];
		bufS16Ptr = bufS16;
	}

	void allocBufF32(void)
	{
		if ( bufF32 )
			delete[] bufF32;

		bufF32 = new float[AUDIOBUFFER_SIZE];
		bufF32Ptr = bufF32;
	}

	int getSoundFrameCounter(void) const
	{
		return soundFrameCounter;
	}

	void resetSoundFrameCounter(void)
	{
		soundFrameCounter = 0;
	}

	int getVolumeMax(void) const
	{
		return volumeMax;
	}

private:
	void soundToFramesS16(VideoState *is);
	void soundToFramesF32(VideoState *is);

	CommercialSkipper *pcs;
	double clock;
	AVStream *stream;
	AVFrame frame;
	double aptsBase;
	double apts;
	int16_t *bufS16;
	int16_t *bufS16Ptr;
	float *bufF32;
	float *bufF32Ptr;
	int soundFrameCounter;
	int volumeMax;
	int framesWithoutSound;
	int framesWithLoudSound;
	// int tracks_without_sound;
};

#define AUDIO_BUFFER_SIZE 288000

class VideoState
{
public:
	uint8_t audio_buf[AUDIO_BUFFER_SIZE];

	CommercialSkipper *pcs;
	Audio *audio;
	AVFormatContext *pFormatCtx;
	int videoStream;
	int audioStream;
	int subtitleStream;

	int av_sync_type;
	// double external_clock; /* external clock base */
	// int64_t external_clock_time;
	int seek_req;
	int seek_flags;
	int64_t seek_pos;
	AVStream *subtitle_st;

	unsigned int audio_buf_size;
	unsigned int audio_buf_index;
	AVPacket audio_pkt;
	AVPacket audio_pkt_temp;
	// uint8_t *audio_pkt_data;
	// int audio_pkt_size;
	int audio_hw_buf_size;
	double audio_diff_cum;
	/* used for AV difference average computation */
	double audio_diff_avg_coef;
	double audio_diff_threshold;
	int audio_diff_avg_count;
	double frame_timer;
	double frame_last_pts;
	double frame_last_delay;
	//<pts of last decoded frame / predicted pts of next decoded frame
	double video_clock;
	// <current displayed pts (different from video_clock
	// if frame fifos are used)
	double video_current_pts;
	//<time (av_gettime) at which we updated video_current_pts
	// - used to have running video pts
	int64_t video_current_pts_time;
	AVStream *video_st;
	AVFrame *pFrame;

	char filename[1024];
	int quit;
	double duration;
	double fps;
};

VideoState *is;

enum {
     AV_SYNC_AUDIO_MASTER,
     AV_SYNC_VIDEO_MASTER,
     AV_SYNC_EXTERNAL_MASTER,
};

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;
#if 0
AVPacket flush_pkt;
#endif

bool video_packet_process(VideoState *, AVPacket *);

#if 0
int video_stream_index = -1;
int audio_stream_index = -1;
bool have_frame_rate;
int stream_index;
#endif

int64_t best_effort_timestamp;

#define USE_ASF 1

FlagFile *sampleFile;

int demux_asf;
bool initial_pts_set;
int64_t initial_pts;
int64_t final_pts;
double pts_offset = 0.0;
double initial_apts;
bool initial_apts_set;

#ifdef _WIN32
int byterate(10000);
#endif

//#define PTS_FRAME (double)(1.0 / is->pcs->getFps())
#define SAMPLE_TO_FRAME 2.8125

#if 0
/*
 * The following two functions are undocumented
 * and not included in any public header,
 * so we need to declare them ourselves
 */
#define FSEEK _fseeki64
#define FTELL _ftelli64
extern int  _fseeki64(FILE *, int64_t, int);
extern int64_t _ftelli64(FILE *);
#endif

FlagFile *timingFile;

#define DUMP_OPEN if (commercialSkipper.getTimingUse()) { timingFile = new FlagFile(); timingFile->open(commercialSkipper.getBasename() + ".timing.csv", "w"); DUMP_HEADER }
#define DUMP_HEADER if ( timingFile ) timingFile->printf("type   ,dts         ,pts         ,clock       ,delta       ,offset\n");
#define DUMP_TIMING(T, D, P, C) if ( timingFile && !csStepping && !csJumping && !csStartJump) timingFile->printf("%7s, %12.3f,%12.3f, %12.3f, %12.3f, %12.3f,\n", \
	T, (double) (D)/frame_delay, (double) (P)/frame_delay, (double) (C)/frame_delay, ((double) (P) - (double) (C))/frame_delay, pts_offset/frame_delay );
#define DUMP_CLOSE if ( timingFile ) { delete timingFile; timingFile = 0; }

int sigint;

#if 0
#ifdef _DEBUG
int dump_seek = 1; // Set to 1 to dump the seeking process
#else
int dump_seek = 0; // Set to 1 to dump the seeking process
#endif
#endif

#include <sys/stat.h>
#include <sys/time.h>
//#include <unistd.h>

#ifdef _WIN32
fpos_t filepos;
fpos_t fileendpos;
#endif

int max_internal_repair_size = 40;
bool reviewing;

class CurrentTime
{
	public:

	CurrentTime() : hour(0), minute(0), second(0) {}
	CurrentTime(double seconds) : hour(0), minute(0), second(0) { set(seconds); }

	void set(double seconds)
	{
		set((int)seconds);
	}

	void set(int seconds)
	{
		second = seconds;
		hour = second / (60 * 60);
		second -= hour * 60 * 60;
		minute = second / 60;
		second -= minute * 60;
	}

	std::string getString(void) const
	{
		char buf[32];
		::snprintf(buf, sizeof(buf), "%2i:%.2i:%.2i", hour, minute, second);
		return buf;
	}

private:
	int hour;
	int minute;
	int second;
};

int csRestart;
int csStartJump;
int csStepping;
int csJumping;
unsigned int frame_period;
char cs_field_t;

void file_open(CommercialSkipper *pcs, Audio *audio);
// void ProcessCCData(void);

void signal_handler (int sig)
{
    sigint = 1;
    signal (sig, SIG_DFL);
    //return (RETSIGTYPE)0;
}

#if 1
#define USE_AUDIO4 1
#else
#if LIBAVCODEC_VERSION_MAJOR > 54 \
	|| ((LIBAVCODEC_VERSION_MAJOR == 54) \
		&& (LIBAVCODEC_VERSION_MINOR >= 86))
#define USE_AUDIO4 1
#endif
#endif

// #define PRINT_S_N 1

void Audio::soundToFramesS16(VideoState *is)
{
	const int sampPerFrame(getSamplesPerFrame());

	if ( sampPerFrame == 0 )
		return;

	if ( ! bufS16 )
		allocBufS16();

	const int16_t *b((const int16_t*)getFrame().data[0]);

	const int count(getSampleCount() * getChannelCount());
	const int sampPerSec(getSamplesPerSecond());

	int sampleCounter(bufS16Ptr - bufS16);
	double p(getClock() - ((double)sampleCounter / (double)(sampPerSec)));
	aptsBase = p;

#ifdef PRINT_S_N
	const std::string fmt(getSampleFormatString());
	::fprintf(stderr, "count=%d, sampPerSec=%d"
					", sampPerFrame=%d, bytesPerSample=%d, format=%s\n"
					, count, sampPerSec
					, sampPerFrame, getBytesPerSample(), fmt.c_str());
#endif

	if ( sampPerFrame == 0 )
		return;

	if ( count > 0 )
		for ( int i = 0; i < count; ++i )
			*bufS16Ptr++ = *b++;

	sampleCounter = bufS16Ptr - bufS16;
	const int16_t *buffer(bufS16);

	while ( sampleCounter >= sampPerFrame )
	{
		if ( sampleFile )
			sampleFile->printf("Frame %i\n", soundFrameCounter);

		int volume(0);

		for ( int i = 0; i < sampPerFrame; ++i )
		{
			if ( sampleFile )
				sampleFile->printf("%i\n", *buffer);

			volume += std::abs((int)*buffer);
			++buffer;
		}

		volume /= sampPerFrame;
		sampleCounter -= sampPerFrame;

		//::fprintf(stderr, "volume=%d\n", volume);

		if ( volume == 0 )
		{
			++framesWithoutSound;
		}
		else if ( volume > 20000 )
		{
			if ( volume > 256000 )
				volume = 220000;

			++framesWithLoudSound;
			volume = -1;
		}
		else
		{
			framesWithoutSound = 0;
		}

		if ( volumeMax < volume )
			volumeMax = volume;

		apts = aptsBase + ((buffer - bufS16) / sampPerSec);
		const int delta((apts - is->video_clock) * is->pcs->getFps());

		// delta = (int64_t)(apts/PTS_FRAME) - (int64_t)(pts/PTS_FRAME);

		if ( -max_internal_repair_size < delta
				&& delta < max_internal_repair_size
				&& std::abs(soundFrameCounter - delta - framenum) > 5 )
		{
			is->pcs->Debug(1, "Audio PTS jumped %d frames at frame %d\n"
				, -soundFrameCounter + delta + framenum, framenum);
			soundFrameCounter = delta + framenum;
		}

		// DUMP_TIMING("a frame", apts, is->video_clock);
		is->pcs->setFrameVolume(((demux_asf) ? ms_audio_delay : 0) + soundFrameCounter++, volume);

		apts += (double)sampPerFrame / (double)sampPerSec;
#if 0
		expected_apts = apts;
#endif
	}

	aptsBase += (double)(buffer - bufS16) / (double)sampPerSec;
	bufS16Ptr = bufS16;

	if ( sampleCounter > 0 )
		for ( int i = 0; i < sampleCounter; ++i )
			*bufS16Ptr++ = *buffer++;
}

void Audio::soundToFramesF32(VideoState *is)
{
	const int sampPerFrame(is->audio->getSamplesPerFrame());

	if ( sampPerFrame == 0 )
		return;

	if ( ! bufF32 )
		allocBufF32();

	const int count(getSampleCount() * getChannelCount());
	const int sampPerSec(getSamplesPerSecond());

	int sampleCounter(bufF32Ptr - bufF32);
	double p(getClock() - ((double)sampleCounter / (double)(sampPerSec)));
	aptsBase = p;

#ifdef PRINT_S_N
	const std::string fmt(is->audio->getSampleFormatString());
	::fprintf(stderr, "count=%d, sampPerSec=%d"
					", sampPerFrame=%d, bytesPerSample=%d, format=%s\n"
					, count, sampPerSec
					, sampPerFrame, getBytesPerSample(), fmt.c_str());
#endif

	const float *b((const float*)getFrame().data[0]);

	if ( count > 0 )
	{
		for ( int i = 0; i < count; ++i )
		{
			*bufF32Ptr++ = ( *b >= -1.0 && *b <= 1.0 ) ? *b : 0.0;
			++b;
		}
	}

	sampleCounter = bufF32Ptr - bufF32;
	const float *buffer(bufF32);

	while ( sampleCounter >= sampPerFrame )
	{
		if ( sampleFile )
			sampleFile->printf("Frame %i\n", soundFrameCounter);

		float volumeFloat(0);

		for ( int i = 0; i < sampPerFrame; ++i )
		{
			if ( sampleFile )
				sampleFile->printf("%i\n", (int)((*buffer) * 32768.0));

			volumeFloat += std::abs(*buffer);
			++buffer;
		}

		volumeFloat /= sampPerFrame;
		volumeFloat *= 32767.0;
		sampleCounter -= sampPerFrame;

		int volume((int)volumeFloat);

		// ::fprintf(stderr, "volume=%d\n", volume);

		if ( volume == 0 )
		{
			++framesWithoutSound;
		}
		else if ( volume > 20000 )
		{
			if ( volume > 256000 )
				volume = 220000;

			++framesWithLoudSound;
			volume = -1;
		}
		else
		{
			framesWithoutSound = 0;
		}

		if ( volumeMax < volume )
			volumeMax = volume;

		apts = aptsBase + ((buffer - bufF32) / sampPerSec);
		const int delta((apts - is->video_clock) * is->pcs->getFps());

		// delta = (int64_t)(apts/PTS_FRAME) - (int64_t)(pts/PTS_FRAME);

		if ( -max_internal_repair_size < delta
				&& delta < max_internal_repair_size
				&& std::abs(soundFrameCounter - delta - framenum) > 5 )
		{
			is->pcs->Debug(1, "Audio PTS jumped %d frames at frame %d\n"
				, -soundFrameCounter + delta + framenum, framenum);
			soundFrameCounter = delta + framenum;
		}

		// DUMP_TIMING("a frame", apts, is->video_clock);
		is->pcs->setFrameVolume(((demux_asf) ? ms_audio_delay : 0) + soundFrameCounter++, volume);

		apts += (double)sampPerFrame / (double)sampPerSec;
#if 0
		expected_apts = apts;
#endif
	}

	aptsBase += (double)(buffer - bufF32) / (double)sampPerSec;
	bufF32Ptr = bufF32;

	if ( sampleCounter > 0 )
		for ( int i = 0; i < sampleCounter; ++i )
			*bufF32Ptr++ = *buffer++;
}

#if 0
#define STORAGE_SIZE 1000000
int16_t storage_buf[STORAGE_SIZE];
#endif

void audio_packet_process(VideoState *is, AVPacket *pkt)
{
	//double frame_delay = 1.0;
	AVPacket *pkt_temp = &is->audio_pkt_temp;

	if ( ! reviewing )
		is->pcs->dumpAudio((char *)pkt->data,(char *) (pkt->data + pkt->size));

	pkt_temp->data = pkt->data;
	pkt_temp->size = pkt->size;
	/*
	 * Try to align on packet boundary as some demuxers don't do that, in particular dvr-ms
	 if (is->audio_st->codec->codec_id == CODEC_ID_MP1) {
	 while (pkt_temp->size > 2 && (pkt_temp->data[0] != 0xff || pkt_temp->data[1] != 0xe0) ) {
		 pkt_temp->data++;
		 pkt_temp->size--;
	 }
	 }
	*/

	/* if update, update the audio clock w/pts */
	double pts(0.0);

	if ( pkt->pts != NOPTS )
	{
		pts = av_q2d(is->audio->getStream()->time_base) * ( pkt->pts - (is->video_st->start_time != NOPTS ? is->video_st->start_time : 0));
	}

	pts = pts + pts_offset;

	// Debug(0 ,"apst[%3d] = %12.3f\n", framenum, pts);

	if ( pts != 0.0 )
	{
		if ( is->audio->getClock() != 0.0 )
		{
			if ( (pts - is->audio->getClock()) < -0.999
					|| (pts - is->audio->getClock()) > max_repair_size / is->pcs->getFps() )
			{
				is->pcs->Debug(1 ,"Audio jumped by %6.3f at frame %d\n"
						, (pts - is->audio->getClock()), framenum);
				// DUMP_TIMING("a   set", pts, is->audio_clock);
				is->audio->setClock(pts);
			}
			else
			{
				// DUMP_TIMING("a  free", pts, is->audio_clock);
				// Do nothing
			}
		}
		else
		{
			// DUMP_TIMING("ajmpset", pts, is->audio_clock);
			is->audio->setClock(pts);
		}
	}
	else
	{
		/* if we aren't given a pts, set it to the clock */
		// DUMP_TIMING("a  tick", pts, is->audio_clock);
		pts = is->audio->getClock();
	}

	// ::fprintf(stderr, "sac = %f\n", is->audio_clock);

	while ( pkt_temp->size > 0 )
	{
#if LIBAVCODEC_VERSION_MAJOR >= 55
		av_frame_unref(&is->audio->getFrame());
#else
		avcodec_get_frame_defaults(&is->audio->getFrame());
#endif
		int gotFrame(0);
		int len1(avcodec_decode_audio4(is->audio->getStream()->codec
					, &is->audio->getFrame(), &gotFrame, pkt_temp));

		if ( len1 < 0 )
		{
			/* if error, skip frame */
			pkt_temp->size = 0;
			break;
		}

		pkt_temp->data += len1;
		pkt_temp->size -= len1;

		// ::fprintf(stderr, "Audio %f\n", is->audio->getClock());
		if ( gotFrame )
		{
			is->audio->soundToFrames(is);
			is->audio->incrementClock();
		}
	}
}

void SetField(char t)
{
	cs_field_t = t;
	// printf(" %c", t);
}

void ResetInputFile(Audio *audio)
{
	global_video_state->seek_req = 1;
	global_video_state->seek_pos = 0;

#ifdef PROCESS_CC
	if ( output_srt || output_smi )
		CEW_reinit();
#endif
	framenum = 0;
	// frame_count = 0;
	audio->resetSoundFrameCounter();
	initial_pts = 0;
	initial_pts_set = false;
	initial_apts_set = false;
	initial_apts = 0;
	// audio_samples = 0;
#ifndef _DEBUG
	// DUMP_CLOSE
	// DUMP_OPEN
#endif
}

bool SubmitFrame(VideoState *is)
{
	AVStream *video_st(is->video_st);
	AVFrame *pFrame(is->pFrame);
	double pts(is->video_clock);
	AVCodecContext *pCodecCtx(video_st->codec);
	CommercialSkipper *pcs(is->pcs);
	const MinMax mmWidth(100, 3840+32);
	const MinMax mmHeight(100, 2160);
	const int height(pCodecCtx->height);
	const int width(pFrame->linesize[0]);
	const int videoWidth(pCodecCtx->width);

	if ( mmWidth.isInRangeInclusive(width)
		&& mmHeight.isInRangeInclusive(height) )
	{
		if ( ! pcs->checkVideoSize(height, width, videoWidth) )
		{
			pcs->setVideoSize(height, width, videoWidth);
			is->pcs->Debug(2, "Format changed to [%d : %d]\n"
							, pcs->getVideoWidth()
							, pcs->getHeight());
		}
	}
	else
	{
		is->pcs->Debug(2, "Invalid frame size [%d : %d]\n"
					, width
					, height);
		pcs->setVideoFramePointer(0);
		return true;
	}

	if ( ! pcs->setVideoFramePointer(pFrame->data[0]) )
		return true;

	if ( pFrame->pict_type == AV_PICTURE_TYPE_B )
		SetField('B');
	else if ( pFrame->pict_type == AV_PICTURE_TYPE_I )
		SetField('I');
	else
		SetField('P');

	if ( framenum == 0 && pass == 0 && test_pts == 0.0 )
		test_pts = pts;

	if ( framenum == 0 && pass > 0 && test_pts != pts )
	{
		is->pcs->Debug(1,"Reset File Failed, initial pts = %6.3f"
						", seek pts = %6.3f, pass = %d\n"
						, test_pts, pts, pass+1);
	}

	if ( framenum == 0 )
		++pass;

#ifdef SELFTEST
	if ( is->pcs->getSelfTest() == 2 && framenum > 20 )
	{
		if ( pass > 1 )
			exit(1);

		ResetInputFile(is->audio);
	}
#endif

	bool status(true);

	if ( ! reviewing )
	{
		is->pcs->getFramesPerSecond().print(false);

		status = (pcs->detectCommercials((int)framenum, pts) == 0);

		++framenum;
		pts += (double)(1.0 / is->pcs->getFps());
	}

	return status;
}

} // namespace

void decodeOnePicture(FILE *f, double pts, CommercialSkipper *pcs, Audio *audio)
{
	(void)f;

	double frame_delay(1.0);
	VideoState *is(global_video_state);
	AVPacket *packet;
	int ret;

	file_open(pcs, audio);
	is = global_video_state;

	reviewing = true;
	is->seek_req = 1;
	// is->seek_pos = av_q2d(is->video_st->codec->time_base)* ((int64_t)is->video_st->codec->ticks_per_frame) * (fp -1) / av_q2d(is->video_st->time_base);
	is->seek_pos = pts / av_q2d(is->video_st->time_base);

	if ( is->video_st->start_time != NOPTS )
		is->seek_pos += is->video_st->start_time;

	pts_offset = 0.0;

	// Debug ( 5,  "Seek to %f\n", pts);
	pcs->setVideoFramePointer(0);
	packet = &(is->audio_pkt);

	int64_t comp_pts(0);

	for( ; ; )
	{
		if ( is->quit )
			break;

		// seek stuff goes here
		if ( is->seek_req )
		{
			// double frame_rate = av_q2d(anim->video_st->r_frame_rate);
			// double time_base = av_q2d(anim->video_st->time_base);

			// long long pos = (long long) position * AV_TIME_BASE / frame_rate;

			int stream_index= -1;
			// int64_t seek_target = av_q2d(is->video_st->codec->time_base)* is->video_st->codec->ticks_per_frame * (fp - 10 ) / av_q2d(is->video_st->time_base);;
			int64_t seek_target = (pts - 1.0)/ av_q2d(is->video_st->time_base);

			if ( is->video_st->start_time != NOPTS )
				seek_target += is->video_st->start_time;

			if ( is->videoStream >= 0 )
				stream_index = is->videoStream;
			else if ( is->audioStream >= 0 )
				stream_index = is->audioStream;

			DUMP_TIMING("v  seek", pts, pts, is->video_clock);

               if(stream_index>=0) {
                    //				   av_q2d(is->video_st->time_base)*
                    //                   seek_target= av_rescale_q(seek_target, AV_TIME_BASE_Q, is->pFormatCtx->streams[stream_index]->time_base);
               }
               ret = -1;
//			is->seek_pos = is->seek_pos * AV_TIME_BASE;
               if (!(is->pFormatCtx->iformat->flags & AVFMT_TS_DISCONT)) {
//                    ret = av_seek_frame(is->pFormatCtx, stream_index, seek_target, AVSEEK_FLAG_BYTE);
               }
               //           {
               //             is->seek_flags = AVSEEK_FLAG_BYTE;
               //       }
                //            ret = av_seek_frame(is->pFormatCtx, stream_index, is->seek_pos, is->seek_flags);
//              ret = avformat_seek_file(is->pFormatCtx, stream_index, INT64_MIN, seek_target, INT64_MAX, is->seek_flags);
//              if (ret< 0) {
                    ret = av_seek_frame(is->pFormatCtx, stream_index, seek_target, AVSEEK_FLAG_BACKWARD);
 //              }
               if (ret< 0) {
                    ret = av_seek_frame(is->pFormatCtx, stream_index, seek_target, AVSEEK_FLAG_BYTE);
               }
//			ret = av_seek_frame(is->pFormatCtx, is->videoStream, seek_target, is->seek_flags);
//               avcodec_flush_buffers(is->video_st->codec);
               is->video_clock = 0.0;
//			   avcodec_flush_buffers(is->audio_st->codec);
               is->audio->resetClock();
//			ret = av_seek_frame(is->pFormatCtx, is->videoStream, av_rescale_q(is->seek_pos, AV_TIME_BASE_Q, is->video_st->time_base), AVSEEK_FLAG_BACKWARD);

               //            ret = avformat_seek_file(is->pFormatCtx, stream_index, INT64_MIN, seek_target, INT64_MAX, is->seek_flags);
               if ( ret < 0 )
			   {

                    if ( is->pFormatCtx->iformat->read_seek )
                         ::printf("format specific\n");
                    else if( is->pFormatCtx->iformat->read_timestamp )
                         ::printf("frame_binary\n");
                    else
                         ::printf("generic\n");

                    ::fprintf(stderr, "%s: error while seeking. target: %d"
							", stream_index: %d\n"
							, is->pFormatCtx->filename
							, (int)seek_target
							, stream_index);
               }
			   else
			   {
                    if ( is->audioStream >= 0 )
					{
                         // packet_queue_flush(&is->audioq);
                         // packet_queue_put(&is->audioq, &flush_pkt);
                    }

                    if ( is->videoStream >= 0 )
					{
                         // packet_queue_flush(&is->videoq);
                         // packet_queue_put(&is->videoq, &flush_pkt);
                    }
               }

               is->seek_req = 0;
          }

          if ( av_read_frame(is->pFormatCtx, packet) < 0 )
               break;

          if ( packet->stream_index == is->videoStream )
		  {
               if ( packet->pts != NOPTS )
					  comp_pts = packet->pts;

               // av_rescale_q(comp_pts, is->video_st->time_base, AV_TIME_BASE_Q);
				int64_t pack_pts(comp_pts);
               //av_rescale_q(packet->duration, is->video_st->time_base, AV_TIME_BASE_Q);
				int64_t pack_duration(packet->duration);
               comp_pts += packet->duration;
				pass = 0;
               video_packet_process(is, packet);

               if ( pcs->getVideoFramePointer() )
               {
//				Debug(0, "Search step %d : Field=%c cur=%d, till=%d, clock = %10.2f\n", count++, cs_field_t, (int)best_effort_timestamp, (int)is->seek_pos,is->video_clock);
                    // If we got the time exactly, or we are already past the seek time,
                    // this is the frame we want
 //                   best_effort_timestamp = av_opt_ptr(avcodec_get_frame_class(), is->pFrame, "best_effort_timestamp");

//                Debug(10,"Search step: terget = %6.3f, pos = %6.3f\n", pts, is->video_clock );
				   if ( is->video_clock < pts - 2.0 || is->video_clock > pts + 2.0 )
                    {
                        pcs->Debug(1,"Search step failed, error too big: target = %6.3f"
										", pos = %6.3f\n", pts, is->video_clock );
                         av_free_packet(packet);
                         break;

                    }
				   else if ( is->video_clock >= pts )
				   {
//                    if (best_effort_timestamp >= is->seek_pos) {
//				if (pack_pts >= is->seek_pos) {
                         av_free_packet(packet);
                         break;
                    }
                    // If the next frame will be past our seek_time, this is the frame we want
                    else if ( pack_pts + pack_duration > is->seek_pos )
					{
                         av_free_packet(packet);
                         break;
                         //	av_free_packet(packet);
                         //	break;
                    }
               }


          }
		else if ( packet->stream_index == is->audioStream )
		{
			// audio_packet_process(is, packet);
		}
		else
		{
			// Do nothing
		}

		av_free_packet(packet);
	}

	reviewing = false;
}

namespace {

void raise_exception(void)
{
     *(int *)0 = 0;
}

int filter(void)
{
	::printf("Exception raised, Comskip is terminating\n");
	::exit(99);
}

bool video_packet_process(VideoState *is, AVPacket *packet)
{
	static int summed_repeat = 0;

	if ( ! reviewing )
		is->pcs->dumpVideo((char *)packet->data
				, (char *)(packet->data + packet->size));

	double real_pts(0.0);
	double pts(0.0);
#if 0
	is->video_st->codec->flags |= CODEC_FLAG_GRAY;
#endif

	// Decode video frame
	int gotFrame(0);
	int len1(avcodec_decode_video2(is->video_st->codec
					, is->pFrame, &gotFrame, packet));

	if ( len1 < 0 )
	{
		::fprintf(stderr, "Error while decoding frame %d\n", framenum);
		return false;
	}

	if ( gotFrame )
	{
		if ( is->video_st->codec->ticks_per_frame < 1 )
			is->video_st->codec->ticks_per_frame = 1;

		// frame delay is the time in seconds till the next frame
		double frame_delay = av_q2d(is->video_st->codec->time_base)
						* is->video_st->codec->ticks_per_frame;

#if 1
		best_effort_timestamp = *(int64_t *)av_opt_ptr(avcodec_get_frame_class(), is->pFrame, "best_effort_timestamp");
#else
		best_effort_timestamp = *(int64_t *)av_opt_ptr(avcodec_get_frame_class(), &frame, "best_effort_timestamp");
#endif

		if ( best_effort_timestamp == NOPTS )
			real_pts = 0.0;
        else
        {
            headerpos = best_effort_timestamp;

            if ( ! initial_pts_set )
            {
                initial_pts = best_effort_timestamp
						- ((is->video_st->start_time != NOPTS) ? is->video_st->start_time : 0);
                initial_pts_set = true;
                final_pts = 0;
                pts_offset = 0.0;
            }

            real_pts = av_q2d(is->video_st->time_base)* ( best_effort_timestamp - (is->video_st->start_time != NOPTS ? is->video_st->start_time : 0)) ;
            final_pts = best_effort_timestamp -  (is->video_st->start_time != NOPTS ? is->video_st->start_time : 0);
        }

        double dts = av_q2d(is->video_st->time_base)* ( is->pFrame->pkt_dts - (is->video_st->start_time != NOPTS ? is->video_st->start_time : 0)) ;

#if 0
		is->pcs->Debug(0 ,"pst[%3d] = %12.3f, inter = %d, rep = %d, ticks = %d\n"
				, framenum, pts/frame_delay
				, is->pFrame->interlaced_frame
				, is->pFrame->repeat_pict
				, is->video_st->codec->ticks_per_frame);
#endif

        pts = real_pts + pts_offset;

        if ( pts != 0 )
        {
               /* if we have pts, set video clock to it */
               if (is->video_clock != 0)
			   {
                    if ((pts - is->video_clock)/frame_delay < -0.99
							|| (pts - is->video_clock)/frame_delay > max_repair_size )
					{
                         if ( ! reviewing )
						{
							is->pcs->Debug(1
								,"Video jumped by %6.1f at frame %d, inter = %d, rep = %d, ticks = %d\n"
								, (pts - is->video_clock) / frame_delay
								, framenum
								, is->pFrame->interlaced_frame
								, is->pFrame->repeat_pict
								, is->video_st->codec->ticks_per_frame);
						}
                         // Calculate offset for jump
                        pts_offset = is->video_clock - real_pts/* set to mid of free window */;
                         pts = real_pts + pts_offset;
                         is->video_clock = pts;
                         DUMP_TIMING("v   set",real_pts, pts, is->video_clock);
                    }
					else if ((pts - is->video_clock)/frame_delay > 5 )
					{
					if ( ! reviewing )
						is->pcs->Debug(1 ,"Video jumped by %6.1f frames at frame %d, repairing timeline\n",
							(pts - is->video_clock)/frame_delay, framenum);
//					is->pFrame->repeat_pict += is->video_st->codec->ticks_per_frame * (pts - is->video_clock)/frame_delay;
                         is->video_clock = pts;
                         DUMP_TIMING("vfollow", real_pts, pts, is->video_clock);
                    }
					else
					{
                         DUMP_TIMING("v  free",real_pts,  pts, is->video_clock);
                         // Do nothing
                    }
               }
			   else
			   {
                    is->video_clock = pts - (5.0 / 2.0) * frame_delay;
                    DUMP_TIMING("v  init", real_pts, pts, is->video_clock);
               }
          }
		else
		{
               /* if we aren't given a pts, set it to the clock */
               DUMP_TIMING("v clock", real_pts, pts, is->video_clock);
               pts = is->video_clock;
          }

          frame_period = (double)900000*30 / (((double)is->video_st->codec->time_base.den) / is->video_st->codec->time_base.num );
          if ( is->video_st->codec->ticks_per_frame >= 1 )
               frame_period *= is->video_st->codec->ticks_per_frame;
          else
               is->video_st->codec->ticks_per_frame = 1;

          is->pcs->setFps(frame_period);

          if ( ! SubmitFrame(is) )
          {
               is->seek_req = 1;
               is->seek_pos = 0;
               return true;
          }
          /* update the video clock */
          is->video_clock += frame_delay;

          summed_repeat += is->pFrame->repeat_pict;

          while ( summed_repeat >= is->video_st->codec->ticks_per_frame )
		  {
               DUMP_TIMING("vrepeat", dts, pts, is->video_clock);

               if ( ! SubmitFrame(is) )
			   {
                    is->seek_req = 1;
                    is->seek_pos = 0;
                    return true;
               }

               /* update the video clock */
               is->video_clock += frame_delay;
               summed_repeat -= is->video_st->codec->ticks_per_frame;
          }
     }

   return true;
}

bool stream_component_open(VideoState *is
		, int stream_index
		, CommercialSkipper *pcs
		, Audio *audio)
{
	if ( stream_index < 0 || stream_index >= (int)is->pFormatCtx->nb_streams )
		return false;

	AVFormatContext *pFormatCtx(is->pFormatCtx);
	/* AVDictionary *opts; */

	if ( std::string(pFormatCtx->iformat->name) == "mpegts" )
		demux_pid = 1;

	// Get a pointer to the codec context for the video stream
	AVCodecContext *codecCtx(pFormatCtx->streams[stream_index]->codec);

	/* prepare audio output */
	if ( codecCtx->codec_type == AVMEDIA_TYPE_AUDIO )
	{
#if LIBAVCODEC_VERSION_MAJOR >= 55
		codecCtx->request_channel_layout = ( codecCtx->channels == 1 )
				? AV_CH_LAYOUT_MONO
				: AV_CH_LAYOUT_STEREO;
#else
		codecCtx->request_channels = ( codecCtx->channels > 0 )
				? FFMIN(2, codecCtx->channels)
				: 2;
#endif
	}

	if ( codecCtx->codec_type == AVMEDIA_TYPE_VIDEO )
	{
		codecCtx->flags |= CODEC_FLAG_GRAY;

		if ( codecCtx->codec_id == CODEC_ID_H264 )
			is_h264 = true;
		else
		{
			codecCtx->lowres = pcs->getLowRes();
			/* if ( lowres ) */
			codecCtx->flags |= CODEC_FLAG_EMU_EDGE;
		}

		// codecCtx->flags2 |= CODEC_FLAG2_FAST;

		if ( codecCtx->codec_id != CODEC_ID_MPEG1VIDEO )
			codecCtx->thread_count = pcs->getThreadCount();
	}

	AVCodec *codec(avcodec_find_decoder(codecCtx->codec_id));

	if ( ! codec || ( avcodec_open2(codecCtx, codec, 0) < 0 ) )
	{
		::fprintf(stderr, "Unsupported codec!\n");
		return false;
	}

	switch ( codecCtx->codec_type )
	{
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitleStream = stream_index;
		is->subtitle_st = pFormatCtx->streams[stream_index];

		if ( demux_pid )
			selected_subtitle_pid = is->subtitle_st->id;

		break;

	case AVMEDIA_TYPE_AUDIO:
		is->audioStream = stream_index;
		audio->setStream(pFormatCtx->streams[stream_index]);
		// is->audio_buf_size = 0;
		// is->audio_buf_index = 0;

		/* averaging filter for audio sync */
		// is->audio_diff_avg_coef = exp(log(0.01 / AUDIO_DIFF_AVG_NB));
		// is->audio_diff_avg_count = 0;
		/* Correct audio only if larger error than this */
		// is->audio_diff_threshold = 2.0 * SDL_AUDIO_BUFFER_SIZE / codecCtx->sample_rate;
		if ( demux_pid )
			selected_audio_pid = audio->getStream()->id;

		::memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));

		break;

	case AVMEDIA_TYPE_VIDEO:
		is->videoStream = stream_index;
		is->video_st = pFormatCtx->streams[stream_index];

		// is->frame_timer = (double)av_gettime() / 1000000.0;
		// is->frame_last_delay = 40e-3;
		// is->video_current_pts_time = av_gettime();

#if LIBAVCODEC_VERSION_MAJOR >= 55
		if ( ! is->pFrame )
			is->pFrame = av_frame_alloc();
#else
		if ( ! is->pFrame )
			is->pFrame = avcodec_alloc_frame();
#endif

		codecCtx->flags |= CODEC_FLAG_GRAY;

		if ( codecCtx->codec_id == CODEC_ID_H264 )
			is_h264 = true;
		else if ( codecCtx->codec_id != CODEC_ID_MPEG1VIDEO )
			codecCtx->lowres = pcs->getLowRes();

		// codecCtx->flags2 |= CODEC_FLAG2_FAST;

		if ( codecCtx->codec_id != CODEC_ID_MPEG1VIDEO )
			codecCtx->thread_count = pcs->getThreadCount();

		if ( codecCtx->codec_id == CODEC_ID_MPEG1VIDEO )
			is->video_st->codec->ticks_per_frame = 1;

		if ( demux_pid )
			selected_video_pid = is->video_st->id;

#if 0
		// MPEG
		if(  (codecCtx->skip_frame >= AVDISCARD_NONREF && s2->pict_type==FF_B_TYPE)
			||(codecCtx->skip_frame >= AVDISCARD_NONKEY && s2->pict_type!=FF_I_TYPE)
			|| codecCtx->skip_frame >= AVDISCARD_ALL)


		if(  (s->avctx->skip_idct >= AVDISCARD_NONREF && s->pict_type == FF_B_TYPE)
			||(codecCtx->skip_idct >= AVDISCARD_NONKEY && s->pict_type != FF_I_TYPE)
			|| s->avctx->skip_idct >= AVDISCARD_ALL)

		// h.264
		if(   s->codecCtx->skip_loop_filter >= AVDISCARD_ALL
			||(s->codecCtx->skip_loop_filter >= AVDISCARD_NONKEY && h->slice_type_nos != FF_I_TYPE)
			||(s->codecCtx->skip_loop_filter >= AVDISCARD_BIDIR  && h->slice_type_nos == FF_B_TYPE)
			||(s->codecCtx->skip_loop_filter >= AVDISCARD_NONREF && h->nal_ref_idc == 0))

		// Both
		if(  (codecCtx->skip_frame >= AVDISCARD_NONREF && s2->pict_type==FF_B_TYPE)
			||(codecCtx->skip_frame >= AVDISCARD_NONKEY && s2->pict_type!=FF_I_TYPE)
			|| codecCtx->skip_frame >= AVDISCARD_ALL)
			break;
#endif

		if ( skip_B_frames )
			codecCtx->skip_frame = AVDISCARD_NONREF;

		// codecCtx->skip_loop_filter = AVDISCARD_NONKEY;
		// codecCtx->skip_idct = AVDISCARD_NONKEY;

		break;

	default:
		break;
	}

	return true;
}

void file_open(CommercialSkipper *pcs, Audio *audio)
{
	AVFormatContext *pFormatCtx;
	int audio_index(-1);
	int video_index(-1);

	if ( ! global_video_state )
	{
		is = (VideoState*)av_mallocz(sizeof(VideoState));

		::memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));

		::strcpy(is->filename, pcs->getMpegFilename().c_str());
		global_video_state = is;

		is->videoStream = -1;
		is->audioStream = -1;
		is->subtitleStream = -1;
		is->pFormatCtx = 0;
	}
	else
		is = global_video_state;

	// will interrupt blocking functions if we quit!

	// avformat_network_init();

	// Open video file

	if ( ! is->pFormatCtx )
	{
		pFormatCtx = avformat_alloc_context();

		if ( avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) )
		{
			::fprintf(stderr, "%s: avformat_open_input failed\n", is->filename);
			exit(-1);
		}

		is->pFormatCtx = pFormatCtx;

#if LIBAVFORMAT_VERSION_MAJOR >= 55
		av_opt_set_int(pFormatCtx, "analyzeduration", 20000000, 0);
#else
		pFormatCtx->max_analyze_duration = 20000000;
#endif

		// pFormatCtx->thread_count= 2;

		// Retrieve stream information
		if ( avformat_find_stream_info(is->pFormatCtx, 0) < 0 )
		{
			::fprintf(stderr, "%s: avformat_find_stream_info failed\n", is->filename);
			exit(-1);
		}

		// Dump information about file onto standard error
		av_dump_format(is->pFormatCtx, 0, is->filename, 0);

		// Find the first video stream
	}

	if ( is->videoStream == -1 )
	{
		video_index = av_find_best_stream(is->pFormatCtx
							, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

		if ( video_index >= 0
				// && is->pFormatCtx->streams[video_index]->codec->width > 100
				// && is->pFormatCtx->streams[video_index]->codec->height > 100
			)
		{
			stream_component_open(is, video_index, pcs, audio);
		}

		if ( is->videoStream < 0 )
		{
			if ( is->pcs )
				is->pcs->Debug(0, "Could not open video codec for %s\n", is->filename);
			else
				::fprintf(stderr, "Could not open video codec for %s\n", is->filename);

			exit(-1);
		}

		if ( is->video_st->duration == NOPTS
				||  is->video_st->duration < 0 )
			is->duration =  ((float)pFormatCtx->duration) / AV_TIME_BASE;
		else
			is->duration =  av_q2d(is->video_st->time_base)* is->video_st->duration;

		/* Calc FPS */
		is->fps = ( is->video_st->r_frame_rate.den && is->video_st->r_frame_rate.num )
				? av_q2d(is->video_st->r_frame_rate)
				: 1 / av_q2d(is->video_st->codec->time_base);
	}

	if ( is->audioStream == -1 )
	{
		audio_index = av_find_best_stream(is->pFormatCtx
							, AVMEDIA_TYPE_AUDIO, -1, video_index, NULL, 0);

		if ( audio_index >= 0 )
		{
			stream_component_open(is, audio_index, pcs, audio);

			if ( is->audioStream < 0 )
			{
				if ( is->pcs )
					is->pcs->Debug(1, "Could not open audio codec for %s\n", is->filename);
				else
					::fprintf(stderr, "Could not open audio codec for %s\n", is->filename);
			}
		}
	}

#if 0
	if ( is->subtitleStream == -1 )
	{
		int subtitle_index = av_find_best_stream(is->pFormatCtx
							, AVMEDIA_TYPE_SUBTITLE, -1, video_index, NULL, 0);

		if( subtitle_index >= 0 )
		{
			is->subtitle_st = pFormatCtx->streams[subtitle_index];

			if ( demux_pid )
				selected_subtitle_pid = is->subtitle_st->id;
		}
	}
#endif
}

void file_close(void)
{
	is = global_video_state;

	if ( is->videoStream != -1 )
	{
		avcodec_close(is->pFormatCtx->streams[is->videoStream]->codec);
		is->videoStream = -1;
	}

	if ( is->audioStream != -1 )
	{
		avcodec_close(is->pFormatCtx->streams[is->audioStream]->codec);
		is->audioStream = -1;
	}

	if ( is->subtitleStream != -1 )
	{
		// avcodec_close(is->pFormatCtx->streams[is->subtitleStream]->codec);
		is->subtitleStream = -1;
	}

	if ( is->pFrame )
	{
#if LIBAVCODEC_VERSION_MAJOR >= 55
		av_frame_free(&is->pFrame);
#else
		avcodec_free_frame(is->pFrame);
#endif
		is->pFrame = 0;
	}

#if LIBAVFORMAT_VERSION_MAJOR >= 54
	::avformat_close_input(&is->pFormatCtx);
#else
	::av_close_input_file(is->pFormatCtx);
#endif
	is->pFormatCtx = 0;
}

} // namespace

#ifdef CO_6193
namespace CS {

void FramesPerSecond::print(bool final)
{
	if ( pcs->getVerbose() || csStepping )
		return;

	struct timeval tvEnd;
	double fps;
	double tfps;
	int frames;
	int elapsed;

	::gettimeofday (&tvEnd, NULL);

	if ( ! frameCounter )
	{
		tvStart = tvBeg = tvEnd;
		signal (SIGINT, signal_handler);
	}

	elapsed = (tvEnd.tv_sec - tvBeg.tv_sec) * 100 + (tvEnd.tv_usec - tvBeg.tv_usec) / 10000;
	totalElapsed = (tvEnd.tv_sec - tvStart.tv_sec) * 100 + (tvEnd.tv_usec - tvStart.tv_usec) / 10000;

	if ( final )
	{
		if ( totalElapsed )
			tfps = frameCounter * 100.0 / totalElapsed;
		else
			tfps = 0;

		::fprintf(stderr
					, "\n%d frames decoded in %.2f seconds (%.2f fps)\n"
					, frameCounter, totalElapsed / 100.0, tfps);
		::fflush(stderr);
		return;
	}

	++frameCounter;

	if ( elapsed < 100 ) /* only display every 1.00 seconds */
		return;

	tvBeg = tvEnd;
	frames = frameCounter - lastCount;

	CurrentTime ct((double)framenum / pcs->getFps());

	fps = frames * 100.0 / elapsed;
	tfps = frameCounter * 100.0 / totalElapsed;

	::fprintf (stderr
			, "%s - %d frames in %.2f sec(%.2f fps), "
				"%.2f sec(%.2f fps), %d%%\r"
			, ct.getString().c_str()
			, frameCounter
			, totalElapsed / 100.0
			, tfps, elapsed / 100.0
			, fps, (int) (100.0 * ((double)(framenum) / pcs->getFps()) / global_video_state->duration));
	::fflush(stderr);
	lastCount = frameCounter;
}

} // namespace CS
#endif

#ifdef CO_8273
int main (int argc, char **argv)
{
	int result(0);

	try
	{

	// Register all formats and codecs
	avcodec_register_all();
	av_register_all();

	// AVFormatContext *pFormatCtx;
	AVPacket pkt1;
	AVPacket *packet = &pkt1;

#if 0
	int video_index = -1;
	int audio_index = -1;
	int i;
#ifdef SELFTEST
	int tries = 0;
#endif
#endif

	int ret;
	// fpos_t fileendpos;

	char *ptr;
	CommercialSkipper commercialSkipper;
	Audio audio(&commercialSkipper);

#ifndef _DEBUG
//	__tr y
	{
		// raise_ exception();
#endif

		// flagFiles.debugwindow.use = true;

		if ( strstr(argv[0], "comskipGUI" ) )
			commercialSkipper.setDebugWindowUse(true);
		else
		{
#ifdef _WIN32
			// added windows specific
			// SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
			// SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
		}

		// get path to executable
		ptr = argv[0];
		size_t len(::strlen(ptr));

		if ( *ptr == '\"' )
		{
			++ptr; //strip off quotation marks
			len = (size_t)(strchr(ptr,'\"') - ptr);
		}

		::strncpy(HomeDir, ptr, len);

		len = (size_t)std::max(0, (int)(::strrchr(HomeDir, DIRSEP) - HomeDir));

		if ( len == 0 )
		{
			HomeDir[0] = '.';
			HomeDir[1] = '\0';
		}
		else
		{
			HomeDir[len] = '\0';
		}

		::fprintf (stderr, "Comskip %s.%s, made using avcodec\n"
				, PACKAGE_VERSION, SUBVERSION);

#ifdef _WIN32
#ifdef HAVE_IO_H
		// _setmode (_fileno (stdin), O_BINARY);
		// _setmode (_fileno (stdout), O_BINARY);
#endif
#endif

#ifdef _WIN32
		// added windows specific
		// if ( ! live_tv ) SetThreadPriority(GetCurrentThread(), /* THREAD_MODE_BACKGROUND_BEGIN */ 0x00010000); // This will fail in XP but who cares
#endif

		commercialSkipper.loadSettings(argc, argv);

		file_open(&commercialSkipper, &audio);

		csRestart = 0;
		framenum = 0;

		DUMP_OPEN

		is = global_video_state;
		is->pcs = &commercialSkipper;
		is->audio = &audio;

		av_log_set_level(AV_LOG_WARNING);
		av_log_set_flags(AV_LOG_SKIP_REPEATED);

		packet = &(is->audio_pkt);

		// main decode loop

		for ( ; ; )
		{
			if ( is->quit )
				break;

			// seek stuff goes here
			if ( is->seek_req )
			{
#ifdef SELFTEST
				int stream_index= -1;
				int64_t seek_target = is->seek_pos;

				if ( commercialSkipper.getSelfTest() == 1 )
				{
					if ( ! (is->seek_flags & AVSEEK_FLAG_BYTE )
							&& is->video_st->start_time != NOPTS )
						seek_target += is->video_st->start_time;

#if 0
					if ( ! (is->pFormatCtx->iformat->flags & AVFMT_TS_DISCONT) )
						is->seek_flags = AVSEEK_FLAG_BYTE;
					else
						is->seek_flags = 0;
#endif


					pts_offset = 0.0;
					is->video_clock = 0.0;
					is->audio->resetClock();

					/*
					 * FIXME the +-2 is due to rounding being not done
					 * in the correct direction in generation
					 * of the seek_pos/seek_rel variables

					ret = avformat_seek_file(is->pFormatCtx, -1, INT64_MIN
							, seek_target
							, INT64_MAX, is->seek_flags);
					*/

					ret = av_seek_frame(is->pFormatCtx, is->videoStream
									, seek_target, is->seek_flags);

					if ( ret < 0 )
					{
						FlagFile ff;
						ff.open(CS_TMPPATH "seektest.log", "a+");
						ff.printf("seek pts  failed: , size=%8.1f \"%s\"\n"
								, is->duration
								, is->filename);
					}

					if ( ret < 0 )
						ret = av_seek_frame(is->pFormatCtx
								, is->videoStream
								, seek_target
								, AVSEEK_FLAG_BYTE);

					if ( ret < 0 )
					{
						FlagFile ff;
						ff.open(CS_TMPPATH "seektest.log", "a+");
						ff.printf("seek byte failed: , size=%8.1f \"%s\"\n"
								, is->duration
								, is->filename);
					}

					is->seek_req = 0;
//               if(stream_index>=0) {is->video_st->start_time
//                    seek_target= av_rescale_q(seek_target, AV_TIME_BASE_Q, is->pFormatCtx->streams[stream_index]->time_base);
//               }
//                    is->seek_flags = AVSEEK_FLAG_BACKWARD;
//              if (strcmp(is->pFormatCtx->iformat->name,"wtv")==0)is->video_st->start_time
//                is->seek_flags = AVSEEK_FLAG_BACKWARD;
//					seek_target = 0;
//                 if(strcmp(is->pFormatCtx->iformat->name,"wtv")==0 || av_seek_frame(is->pFormatCtx, stream_index, seek_target, AVSEEK_FLAG_BYTE) < 0) {
//                    if(av_seek_frame(is->pFormatCtx, stream_index, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
//                         if(av_seek_frame(is->pFormatCtx, stream_index, seek_target, AVSEEK_FLAG_BYTE) < 0) {

					if ( ret < 0 )
					{
						if ( is->pFormatCtx->iformat->read_seek )
							::printf("format specific\n");
						else if( is->pFormatCtx->iformat->read_timestamp )
							::printf("frame_binary\n");
						else
							::printf("generic\n");

						::fprintf(stderr, "%s: error while seeking. target: %d"
								", stream_index: %d\n"
								, is->pFormatCtx->filename
								, (int) seek_target
								, stream_index);
					}

//                    avcodec_flush_buffers(is->video_st->codec);
//                    avcodec_flush_buffers(is->audio_st->codec);
//                else {
//                   if(is->audioStream >= 0) {
//                         packet_queue_flush(&is->audioq);
//                         packet_queue_put(&is->audioq, &flush_pkt);
                    //                  }
                    //                if(is->videoStream >= 0) {
//                         packet_queue_flush(&is->videoq);
//                         packet_queue_put(&is->videoq, &flush_pkt);
                    //              }
                    //         }
                    //avcodec_default_free_buffers(is->video_st->codec);
                    } else
#endif
				{
					is->seek_req = 0;
					framenum = 0;
					pts_offset = 0.0;
					is->video_clock = 0.0;
					is->audio->resetClock();
					file_close();
					file_open(&commercialSkipper, &audio);
					is->audio->resetSoundFrameCounter();
					initial_pts = 0;
					initial_pts_set = false;
					initial_apts_set = false;
					initial_apts = 0;
					// audio_samples = 0;
				}
	 //                   best_effort_timestamp = NOPTS;
 //                   is->pFrame->pkt_pts = NOPTS;
//                    is->pFrame->pkt_dts = NOPTS;

			}

			if ( av_read_frame(is->pFormatCtx, packet) < 0 )
			{
				/*
				if( url_ferror(is->pFormatCtx->pb) == 0 )
				{
					continue;
				}
				else
				 */
				{
					break;
				}
			}

#ifdef SELFTEST
			if ( commercialSkipper.getSelfTest() == 1 && framenum > 30 && packet->pts != NOPTS )
			{
				FlagFile ff;
				ff.open(CS_TMPPATH "seektest.log", "a+");
				ff.printf("target=%8.1f, result=%8.1f, error=%6.3f,"
						" size=%8.1f, \"%s\"\n"
						, (double)av_q2d(is->video_st->time_base)* is->seek_pos
						, (double) av_q2d(is->video_st->time_base)* (packet->pts - (is->video_st->start_time != NOPTS ? is->video_st->start_time:0))
						, (double) av_q2d(is->video_st->time_base)* ((double)(packet->pts - (is->video_st->start_time != NOPTS ? is->video_st->start_time:0) - is->seek_pos ))
						, is->duration
						, is->filename);
/*
                if (tries ==  0 && fabs((double) av_q2d(is->video_st->time_base)* ((double)(packet->pts - is->video_st->start_time - is->seek_pos ))) > 2.0) {
				   is->seek_req=1;
				   is->seek_pos = 20.0 / av_q2d(is->video_st->time_base);
				   is->seek_flags = AVSEEK_FLAG_BYTE;
				   tries++;
               } else
 */
				exit(1);
			}
#endif

			if ( packet->stream_index == is->videoStream )
				video_packet_process(is, packet);
			else if ( packet->stream_index == is->audioStream )
				audio_packet_process(is, packet);
#if 0
			else
			{
				ccDataLen = (int)packet->size;

				for ( i = 0; i < ccDataLen; ++i )
					ccData[i] = packet->data[i];

				dumpData(ccData, (int)ccDataLen);

				if ( output_srt )
					process_block(ccData, (int)ccDataLen);

				if ( doProcessCc )
					ProcessCCData();
			}
#endif

#if 1
			av_free_packet(packet);
#endif
#ifdef SELFTEST
				if ( commercialSkipper.getSelfTest() == 1
						&& is->seek_req == 0
						&& framenum == 30 )
				{
					is->seek_req = 1;
					is->seek_pos = 20.0 / av_q2d(is->video_st->time_base);
					is->seek_flags = AVSEEK_FLAG_BACKWARD;
					++framenum;
				}
#endif
			}

			commercialSkipper.Debug( 10,"\nParsed %d video frames"
					" and %d audio frames of %4.2f fps\n"
					, framenum
					, is->audio->getSoundFrameCounter()
					, commercialSkipper.getFps());
			commercialSkipper.Debug( 10,"\nMaximum Volume found is %d\n"
					, is->audio->getVolumeMax());

#ifdef _WIN32
			if ( framenum > 0 )
				byterate = fileendpos / framenum;
#endif

			commercialSkipper.getFramesPerSecond().print(true);

          if ( framenum > 0 )
		  {
#ifdef _WIN32
               byterate = fileendpos / framenum;
#endif
               if ( commercialSkipper.buildMasterCommList() )
                    ::printf("Commercials were found.\n");
			   else
                    ::printf("Commercials were not found.\n");

               if ( commercialSkipper.getDebugWindowUse() )
			   {
                    doProcessCc = false;
                    printf("Close window when done\n");

                    DUMP_CLOSE
                    commercialSkipper.setTimingUse(false);

#if defined(_WIN32) && ! defined(GUI)
                    while ( 1 )
					{
                         ReviewResult();
                         vo_refresh();
                         Sleep((DWORD)100);
                    }
#endif
					// printf(" Press Enter to close debug window\n");
					// gets(HomeDir);
               }
          }
		  else
		{
			result = 1; // failure
		}

#ifndef _DEBUG
	}
//	__exc ept(filter()) /* Stage 3 */
//	{
//      ::printf("Exception raised, terminating\n");/* Stage 5 of terminating exception */
//		exit(result);
//	}
#endif

	file_close();

	}
	catch ( std::runtime_error &e )
	{
		::fprintf(stderr, "%s\n", e.what());
		::exit(6);
	}

	return result;
}
#endif

