#include <cassert>
#include <fcntl.h>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

using namespace std;

#include <map>
#include <vector>
#include <algorithm>

#include <boost/fiber/unbuffered_channel.hpp>

using Color			= std::tuple<uint8_t, uint8_t, uint8_t>;
using channel_t		= boost::fibers::unbuffered_channel<tuple<AVFrame*, unsigned long long int>>;
auto EncoderChannel = new channel_t();

/* return a floating point value specifying what to scale the sample
 * value by to reduce it from full volume to dB decibels */
float dBFS(float dB) {
	/* 10 ^ (dB / 20),
	   based on reversing the formula for converting samples to decibels:
	   dB = 20.0 * log10(sample);
	   where "sample" is -1.0 <= x <= 1.0 */
	return pow(10.0F, dB / 20.0F);
}

/* attenuate a sample value by this many dBFS */
/* so if you want to reduce it by 20dBFS you pass -20 as dB */
float attenuate_dBFS(float sample, float dB) { return sample * dBFS(dB); }

/* opposite: convert sample to decibels */
float dBFS_measure(float sample) { return 20.0F * log10(sample); }

// lowpass filter
// you can make it a highpass filter by applying a lowpass then subtracting from source.
class LowpassFilter
{
public:
	LowpassFilter() = default;
	void setFilter(const float rate /*sample rate of audio*/, const float hz /*cutoff*/) {
#ifndef M_PI
#error your math.h does not include M_PI constant
#endif
		timeInterval = 1.0F / rate;
		tau			 = 1.F / (hz * 2.F * M_PI);
		cutoff		 = hz;
		alpha		 = timeInterval / (tau + timeInterval);
	}
	void  resetFilter(const float val) { prev = val; }
	float lowpass(const float sample) {
		const float stage1 = sample * alpha;
		const float stage2 = prev - (prev * alpha); /* NTS: Instead of prev * (1.0 - alpha) */
		return (prev = (stage1 + stage2));			/* prev = stage1+stage2 then return prev */
	}
	float highpass(const float sample) {
		const float stage1 = sample * alpha;
		const float stage2 = prev - (prev * alpha); /* NTS: Instead of prev * (1.0 - alpha) */
		return sample - (prev = (stage1 + stage2)); /* prev = stage1+stage2 then return (sample - prev) */
	}

public:
	float timeInterval{0};
	float cutoff{0};
	float alpha{0}; /* timeInterval / (tau + timeInterval) */
	float prev{0};
	float tau{0};
};

class HiLoPair
{
public:
	LowpassFilter hi, lo;  // highpass, lowpass
public:
	void setFilter(const float rate /*sample rate of audio*/, const float low_hz, const float high_hz) {
		lo.setFilter(rate, low_hz);
		hi.setFilter(rate, high_hz);
	}
	float filter(const float sample) { return hi.highpass(lo.lowpass(sample)); /* first lowpass, then highpass */ }
};

class HiLoPass : public vector<HiLoPair>
{  // all passes, one sample of one channel
public:
	HiLoPass() = default;

public:
	void setFilter(const float rate /*sample rate of audio*/, const float low_hz, const float high_hz) {
		for (size_t i = 0; i < size(); i++) { (*this)[i].setFilter(rate, low_hz, high_hz); }
	}
	float filter(float sample) {
		for (size_t i = 0; i < size(); i++) { sample = (*this)[i].lo.lowpass(sample); }
		for (size_t i = 0; i < size(); i++) { sample = (*this)[i].hi.highpass(sample); }
		return sample;
	}
	void init(const unsigned int passes) {
		clear();
		resize(passes);
		assert(size() >= passes);
	}
};

class HiLoSample : public vector<HiLoPass>
{  // all passes, all channels of one sample period
public:
	HiLoSample() = default;

public:
	void init(const unsigned int channels, const unsigned int passes) {
		clear();
		resize(channels);
		assert(size() >= channels);
		for (size_t i = 0; i < size(); i++) { (*this)[i].init(passes); }
	}
	void setFilter(const float rate /*sample rate of audio*/, const float low_hz, const float high_hz) {
		for (size_t i = 0; i < size(); i++) { (*this)[i].setFilter(rate, low_hz, high_hz); }
	}
};

class HiLoComboPass
{
public:
	HiLoComboPass() = default;
	~HiLoComboPass() { clear(); }
	void setChannels(const size_t _channels) {
		if (channels != _channels) {
			clear();
			channels = _channels;
		}
	}
	void setCutoff(const float _low_cutoff, const float _high_cutoff) {
		if (low_cutoff != _low_cutoff || high_cutoff != _high_cutoff) {
			clear();
			low_cutoff  = _low_cutoff;
			high_cutoff = _high_cutoff;
		}
	}
	void setRate(const float _rate) {
		if (rate != _rate) {
			clear();
			rate = _rate;
		}
	}
	void setPasses(const size_t _passes) {
		if (passes != _passes) {
			clear();
			passes = _passes;
		}
	}
	void clear() { audiostate.clear(); }
	void init() {
		clear();
		if (channels == 0 || passes == 0 || rate == 0 || low_cutoff == 0 || high_cutoff == 0) { return; }
		audiostate.init(channels, passes);
		audiostate.setFilter(rate, low_cutoff, high_cutoff);
	}

public:
	float	  rate{0.F};
	size_t	 passes{0};
	size_t	 channels{0};
	float	  low_cutoff{0.F};
	float	  high_cutoff{0.F};
	HiLoSample audiostate;
};

AVRational output_field_rate				 = {60000, 1001};  // NTSC 60Hz default
AVRational output_aspect_ratio				 = {4, 3};
int		   output_width						 = 720;
int		   output_height					 = 480;
bool	   output_ntsc						 = true;   // NTSC color subcarrier emulation
bool	   output_pal						 = false;  // PAL color subcarrier emulation
int		   output_audio_channels			 = 2;	  // VHS stereo (set to 1 for mono)
int		   output_audio_rate				 = 44100;  // VHS Hi-Fi goes up to 20KHz
int		   video_scanline_phase_shift		 = 180;
int		   video_scanline_phase_shift_offset = 0;

#define RGBTRIPLET(r, g, b)                                                                                            \
	(((uint32_t)(r) << (uint32_t)16) + ((uint32_t)(g) << (uint32_t)8) + ((uint32_t)(b) << (uint32_t)0))

AVFormatContext*	  output_avfmt						  = nullptr;
AVStream*			  output_avstream_audio				  = nullptr;  // do not free
AVCodecContext*		  output_avstream_audio_codec_context = nullptr;  // do not free
AVStream*			  output_avstream_video				  = nullptr;  // do not free
AVCodecContext*		  output_avstream_video_codec_context = nullptr;  // do not free
std::vector<AVFrame*> output_avstream_video_frame;					  // ARGB
AVFrame*			  output_avstream_video_encode_frame = nullptr;   // 4:2:2 or 4:2:0
size_t				  output_avstream_video_frame_delay  = 1;
size_t				  output_avstream_video_frame_index  = 0;
struct SwsContext*	output_avstream_video_resampler	= nullptr;

class InputFile
{
public:
	InputFile() {
		input_avfmt						   = nullptr;
		audio_dst_data					   = nullptr;
		input_avstream_audio			   = nullptr;
		input_avstream_audio_frame		   = nullptr;
		input_avstream_audio_resampler	 = nullptr;
		input_avstream_audio_codec_context = nullptr;
		input_avstream_video			   = nullptr;
		input_avstream_video_frame		   = nullptr;
		input_avstream_video_frame_rgb	 = nullptr;
		input_avstream_video_resampler	 = nullptr;
		input_avstream_video_codec_context = nullptr;
		next_pts = next_dts = -1LL;
		avpkt_valid			= false;
		eof_stream			= false;
		eof					= false;
	}
	~InputFile() { close_input(); }

public:
	void reset_on_dup() { path.clear(); }
	bool open_input() {
		if (input_avfmt == nullptr) {
			if (avformat_open_input(&input_avfmt, path.c_str(), nullptr, nullptr) < 0) {
				fprintf(stderr, "Failed to open input file\n");
				close_input();
				return false;
			}

			if (avformat_find_stream_info(input_avfmt, nullptr) < 0) {
				fprintf(stderr, "WARNING: Did not find stream info on input\n");
			}

			/* scan streams for one video, one audio */
			{
				size_t			i;
				AVStream*		is;
				int				ac = 0;
				int				vc = 0;
				AVCodecContext* isctx;

				fprintf(stderr, "Input format: %u streams found\n", input_avfmt->nb_streams);
				for (i = 0; i < static_cast<size_t>(input_avfmt->nb_streams); i++) {
					is = input_avfmt->streams[i];
					if (is == nullptr) { continue; }

					isctx = is->codec;
					if (isctx == nullptr) { continue; }

					if (isctx->codec_type == AVMEDIA_TYPE_AUDIO) {
						if (input_avstream_audio == nullptr && ac == 0) {
							if (avcodec_open2(isctx, avcodec_find_decoder(isctx->codec_id), nullptr) >= 0) {
								input_avstream_audio			   = is;
								input_avstream_audio_codec_context = isctx;
								fprintf(stderr, "Found audio stream idx=%zu %u-channel %uHz\n", i,
									input_avstream_audio_codec_context->channels,
									input_avstream_audio_codec_context->sample_rate);
							} else {
								fprintf(stderr, "Found audio stream but not able to decode\n");
							}
						}

						ac++;
					} else if (isctx->codec_type == AVMEDIA_TYPE_VIDEO) {
						if (input_avstream_video == nullptr && vc == 0) {
							if (avcodec_open2(isctx, avcodec_find_decoder(isctx->codec_id), nullptr) >= 0) {
								input_avstream_video			   = is;
								input_avstream_video_codec_context = isctx;
								fprintf(stderr, "Found video stream idx=%zu\n", i);
							} else {
								fprintf(stderr, "Found video stream but not able to decode\n");
							}
						}

						vc++;
					}
				}

				if (input_avstream_video == nullptr && input_avstream_audio == nullptr) {
					fprintf(stderr, "Neither video nor audio found\n");
					close_input();
					return 1;
				}
			}
		}

		/* prepare audio decoding */
		if (input_avstream_audio != nullptr) {
			input_avstream_audio_frame = av_frame_alloc();
			if (input_avstream_audio_frame == nullptr) {
				fprintf(stderr, "Failed to alloc audio frame\n");
				close_input();
				return 1;
			}
		}

		/* prepare video decoding */
		if (input_avstream_video != nullptr) {
			input_avstream_video_frame = av_frame_alloc();
			if (input_avstream_video_frame == nullptr) {
				fprintf(stderr, "Failed to alloc video frame\n");
				close_input();
				return 1;
			}

			/* prepare video encoding */
			input_avstream_video_frame_rgb = av_frame_alloc();
			if (input_avstream_video_frame_rgb == nullptr) {
				fprintf(stderr, "Failed to alloc video frame\n");
				close_input();
				return 1;
			}

			/* make output dimensions and aspect ratio match input */
			output_height = input_avstream_video_codec_context->height;
			output_width  = input_avstream_video_codec_context->width;
			output_aspect_ratio =
				av_guess_sample_aspect_ratio(input_avfmt, input_avstream_video, input_avstream_video_frame);

			input_avstream_video_frame_rgb->format = AV_PIX_FMT_BGRA;
			input_avstream_video_frame_rgb->height = output_height;
			input_avstream_video_frame_rgb->width  = output_width;
			if (av_frame_get_buffer(input_avstream_video_frame_rgb, 64) < 0) {
				fprintf(stderr, "Failed to alloc render frame\n");
				close_input();
				return 1;
			}
			memset(input_avstream_video_frame_rgb->data[0], 0,
				input_avstream_video_frame_rgb->linesize[0] * input_avstream_video_frame_rgb->height);
		}

		input_avstream_video_resampler_format   = AV_PIX_FMT_NONE;
		input_avstream_video_resampler_height   = -1;
		input_avstream_video_resampler_width	= -1;
		last_written_sample						= 0;
		audio_dst_data_out_audio_sample			= 0;
		audio_sample							= 0;
		audio_dst_data							= nullptr;
		audio_dst_data_alloc_samples			= 0;
		audio_dst_data_linesize					= 0;
		audio_dst_data_samples					= 0;
		audio_dst_data_out_samples				= 0;
		input_avstream_audio_resampler_channels = -1;
		input_avstream_audio_resampler_rate		= -1;
		eof_stream								= false;
		got_audio								= false;
		got_video								= false;
		adj_time								= 0;
		t = pt = -1;
		eof	= false;
		avpkt_init();
		next_pts = next_dts = -1LL;
		return (input_avfmt != nullptr);
	}
	bool next_packet() {
		if (eof) { return false; }
		if (input_avfmt == nullptr) { return false; }

		do {
			if (eof_stream) { break; }
			avpkt_release();
			avpkt_init();
			if (av_read_frame(input_avfmt, &avpkt) < 0) {
				eof_stream = true;
				return false;
			}
			if (avpkt.stream_index >= input_avfmt->nb_streams) { continue; }

			// ugh... this can happen if the source is an AVI file
			if (avpkt.pts == AV_NOPTS_VALUE) { avpkt.pts = avpkt.dts; }

			/* track time and keep things monotonic for our code */
			if (avpkt.pts != AV_NOPTS_VALUE) {
				t = avpkt.pts * av_q2d(input_avfmt->streams[avpkt.stream_index]->time_base);

				if (pt < 0) {
					adj_time = -t;
				} else if ((t + 1.5F) < pt) {  // time code jumps backwards (1.5 is safe for DVD timecode resets)
					adj_time += pt - t;
					fprintf(stderr, "Time code jump backwards %.6f->%.6f. adj_time=%.6f\n", pt, t, adj_time);
				} else if (t > (pt + 5)) {  // time code jumps forwards
					adj_time += pt - t;
					fprintf(stderr, "Time code jump forwards %.6f->%.6f. adj_time=%.6f\n", pt, t, adj_time);
				}

				pt = t;
			}

			if (pt < 0) { continue; }

			if (avpkt.pts != AV_NOPTS_VALUE) {
				avpkt.pts += (adj_time * input_avfmt->streams[avpkt.stream_index]->time_base.den) /
							 input_avfmt->streams[avpkt.stream_index]->time_base.num;
			}

			if (avpkt.dts != AV_NOPTS_VALUE) {
				avpkt.dts += (adj_time * input_avfmt->streams[avpkt.stream_index]->time_base.den) /
							 input_avfmt->streams[avpkt.stream_index]->time_base.num;
			}

			got_audio = false;
			got_video = false;
			if (input_avstream_audio != nullptr && avpkt.stream_index == input_avstream_audio->index) {
				if (got_audio) { fprintf(stderr, "Audio content lost\n"); }
				av_packet_rescale_ts(&avpkt, input_avstream_audio->time_base, output_avstream_audio->time_base);
				handle_audio(/*&*/ avpkt);
				got_audio = true;
				break;
			}
			if (input_avstream_video != nullptr && avpkt.stream_index == input_avstream_video->index) {
				if (got_video) { fprintf(stderr, "Video content lost\n"); }
				AVRational m = (AVRational){output_field_rate.den, output_field_rate.num};
				av_packet_rescale_ts(&avpkt, input_avstream_video->time_base, m);  // convert to FIELD number
				handle_frame(/*&*/ avpkt);										   // will set got_video
				break;
			}

			avpkt_release();
		} while (1);

		if (eof_stream) {
			avpkt_release();
			avpkt.size = 0;
			avpkt.data = nullptr;
			handle_frame(/*&*/ avpkt);  // will set got_video
			if (!got_video) {
				eof = true;
			} else {
				fprintf(stderr, "Got latent frame\n");
			}
		}

		return true;
	}
	void handle_audio(AVPacket& pkt) {
		int got_frame = 0;

		if (avcodec_decode_audio4(input_avstream_audio_codec_context, input_avstream_audio_frame, &got_frame, &pkt) >=
			0) {
			if (got_frame != 0 && input_avstream_audio_frame->nb_samples != 0) {
				if (input_avstream_audio_frame->pts == AV_NOPTS_VALUE) { input_avstream_audio_frame->pts = pkt.pts; }

				if (input_avstream_audio_resampler != nullptr) {
					if (input_avstream_audio_resampler_rate != input_avstream_audio_codec_context->sample_rate ||
						input_avstream_audio_resampler_channels != input_avstream_audio_codec_context->channels) {
						fprintf(stderr, "Audio format changed\n");
						swr_free(&input_avstream_audio_resampler);
					}
				}

				if (input_avstream_audio_resampler == nullptr) {
					input_avstream_audio_resampler = swr_alloc();
					av_opt_set_int(input_avstream_audio_resampler, "in_channel_count",
						input_avstream_audio_codec_context->channels, 0);  // FIXME: FFMPEG should document this!!
					av_opt_set_int(input_avstream_audio_resampler, "out_channel_count",
						output_avstream_audio_codec_context->channels, 0);  // FIXME: FFMPEG should document this!!
					av_opt_set_int(input_avstream_audio_resampler, "in_channel_layout",
						input_avstream_audio_codec_context->channel_layout, 0);
					av_opt_set_int(input_avstream_audio_resampler, "out_channel_layout",
						output_avstream_audio_codec_context->channel_layout, 0);
					av_opt_set_int(input_avstream_audio_resampler, "in_sample_rate",
						input_avstream_audio_codec_context->sample_rate, 0);
					av_opt_set_int(input_avstream_audio_resampler, "out_sample_rate",
						output_avstream_audio_codec_context->sample_rate, 0);
					av_opt_set_sample_fmt(input_avstream_audio_resampler, "in_sample_fmt",
						input_avstream_audio_codec_context->sample_fmt, 0);
					av_opt_set_sample_fmt(input_avstream_audio_resampler, "out_sample_fmt",
						output_avstream_audio_codec_context->sample_fmt, 0);
					if (swr_init(input_avstream_audio_resampler) < 0) {
						fprintf(stderr, "Failed to init audio resampler\n");
						swr_free(&input_avstream_audio_resampler);
						return;
					}
					input_avstream_audio_resampler_rate		= input_avstream_audio_codec_context->sample_rate;
					input_avstream_audio_resampler_channels = input_avstream_audio_codec_context->channels;

					if (audio_dst_data != nullptr) {
						av_freep(&audio_dst_data[0]);  // NTS: Why??
						av_freep(&audio_dst_data);
					}

					audio_dst_data_alloc_samples = 0;
					fprintf(stderr, "Audio resampler init %uHz -> %uHz\n",
						input_avstream_audio_codec_context->sample_rate,
						output_avstream_audio_codec_context->sample_rate);
				}

				audio_dst_data_samples = av_rescale_rnd(
					swr_get_delay(input_avstream_audio_resampler, input_avstream_audio_frame->sample_rate) +
						input_avstream_audio_frame->nb_samples,
					output_avstream_audio_codec_context->sample_rate, input_avstream_audio_frame->sample_rate,
					AV_ROUND_UP);

				if (audio_dst_data == nullptr || audio_dst_data_samples > audio_dst_data_alloc_samples) {
					if (audio_dst_data != nullptr) {
						av_freep(&audio_dst_data[0]);  // NTS: Why??
						av_freep(&audio_dst_data);
					}

					audio_dst_data_alloc_samples = 0;
					fprintf(stderr, "Allocating audio buffer %u samples\n",
						static_cast<unsigned int>(audio_dst_data_samples));
					if (av_samples_alloc_array_and_samples(&audio_dst_data, &audio_dst_data_linesize,
							output_avstream_audio_codec_context->channels, audio_dst_data_samples,
							output_avstream_audio_codec_context->sample_fmt, 0) >= 0) {
						audio_dst_data_alloc_samples = audio_dst_data_samples;
					} else {
						fprintf(stderr, "Failure to allocate audio buffer\n");
						audio_dst_data_alloc_samples = 0;
					}
				}

				if (audio_dst_data != nullptr) {
					if ((audio_dst_data_out_samples = swr_convert(input_avstream_audio_resampler, audio_dst_data,
							 audio_dst_data_samples, (const uint8_t**)input_avstream_audio_frame->data,
							 input_avstream_audio_frame->nb_samples)) > 0) {
					} else if (audio_dst_data_out_samples < 0) {
						fprintf(stderr, "Failed to resample audio\n");
					}

					audio_dst_data_out_audio_sample = audio_sample;
					audio_sample += audio_dst_data_out_samples;
				}
			}
		}
	}
	void frame_copy_scale() {
		if (input_avstream_video_frame_rgb == nullptr) {
			fprintf(stderr, "New input frame\n");
			input_avstream_video_frame_rgb = av_frame_alloc();
			if (input_avstream_video_frame_rgb == nullptr) {
				fprintf(stderr, "Failed to alloc video frame\n");
				return;
			}

			input_avstream_video_frame_rgb->format = AV_PIX_FMT_BGRA;
			input_avstream_video_frame_rgb->height = output_height;
			input_avstream_video_frame_rgb->width  = output_width;
			if (av_frame_get_buffer(input_avstream_video_frame_rgb, 64) < 0) {
				fprintf(stderr, "Failed to alloc render frame\n");
				return;
			}
			memset(input_avstream_video_frame_rgb->data[0], 0,
				input_avstream_video_frame_rgb->linesize[0] * input_avstream_video_frame_rgb->height);
		}

		if (input_avstream_video_resampler !=
			nullptr) {  // pixel format change or width/height change = free resampler and reinit
			if (input_avstream_video_resampler_format != input_avstream_video_frame->format ||
				input_avstream_video_resampler_width != input_avstream_video_frame->width ||
				input_avstream_video_resampler_height != input_avstream_video_frame->height) {
				sws_freeContext(input_avstream_video_resampler);
				input_avstream_video_resampler = nullptr;
			}
		}

		if (input_avstream_video_resampler == nullptr) {
			input_avstream_video_resampler = sws_getContext(
				// source
				input_avstream_video_frame->width, input_avstream_video_frame->height,
				static_cast<AVPixelFormat>(input_avstream_video_frame->format),
				// dest
				input_avstream_video_frame_rgb->width, input_avstream_video_frame_rgb->height,
				static_cast<AVPixelFormat>(input_avstream_video_frame_rgb->format),
				// opt
				SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

			if (input_avstream_video_resampler != nullptr) {
				fprintf(stderr, "sws_getContext new context\n");
				input_avstream_video_resampler_format = static_cast<AVPixelFormat>(input_avstream_video_frame->format);
				input_avstream_video_resampler_width  = input_avstream_video_frame->width;
				input_avstream_video_resampler_height = input_avstream_video_frame->height;
			} else {
				fprintf(stderr, "sws_getContext fail\n");
			}
		}

		if (input_avstream_video_resampler != nullptr) {
			input_avstream_video_frame_rgb->pts				 = input_avstream_video_frame->pts;
			input_avstream_video_frame_rgb->pkt_pts			 = input_avstream_video_frame->pkt_pts;
			input_avstream_video_frame_rgb->pkt_dts			 = input_avstream_video_frame->pkt_dts;
			input_avstream_video_frame_rgb->top_field_first  = input_avstream_video_frame->top_field_first;
			input_avstream_video_frame_rgb->interlaced_frame = input_avstream_video_frame->interlaced_frame;

			if (sws_scale(input_avstream_video_resampler,
					// source
					input_avstream_video_frame->data, input_avstream_video_frame->linesize, 0,
					input_avstream_video_frame->height,
					// dest
					input_avstream_video_frame_rgb->data, input_avstream_video_frame_rgb->linesize) <= 0) {
				fprintf(stderr, "WARNING: sws_scale failed\n");
			}
		}
	}
	void handle_frame(AVPacket& pkt) {
		int got_frame = 0;

		if (avcodec_decode_video2(input_avstream_video_codec_context, input_avstream_video_frame, &got_frame, &pkt) >=
			0) {
			if (got_frame != 0 && input_avstream_video_frame->width > 0 && input_avstream_video_frame->height > 0) {
				got_video = true;
			}
		} else {
			fprintf(stderr, "No video decoded\n");
		}
	}
	void avpkt_init() {
		if (!avpkt_valid) {
			avpkt_valid = true;
			av_init_packet(&avpkt);
		}
	}
	void avpkt_release() {
		if (avpkt_valid) {
			avpkt_valid = false;
			av_packet_unref(&avpkt);
		}
		got_audio = false;
		got_video = false;
	}
	void close_input() {
		eof = true;
		avpkt_release();
		if (input_avstream_audio_codec_context != nullptr) {
			avcodec_close(input_avstream_audio_codec_context);
			input_avstream_audio_codec_context = nullptr;
			input_avstream_audio			   = nullptr;
		}
		if (input_avstream_video_codec_context != nullptr) {
			avcodec_close(input_avstream_video_codec_context);
			input_avstream_video_codec_context = nullptr;
			input_avstream_video			   = nullptr;
		}

		if (input_avstream_audio_frame != nullptr) { av_frame_free(&input_avstream_audio_frame); }
		if (input_avstream_video_frame != nullptr) { av_frame_free(&input_avstream_video_frame); }
		if (input_avstream_video_frame_rgb != nullptr) { av_frame_free(&input_avstream_video_frame_rgb); }

		if (input_avstream_audio_resampler != nullptr) { swr_free(&input_avstream_audio_resampler); }
		if (input_avstream_video_resampler != nullptr) {
			sws_freeContext(input_avstream_video_resampler);
			input_avstream_video_resampler = nullptr;
		}

		if (audio_dst_data != nullptr) {
			av_freep(&audio_dst_data[0]);  // NTS: Why??
			av_freep(&audio_dst_data);
		}

		input_avstream_audio_resampler_channels = -1;
		input_avstream_audio_resampler_rate		= -1;
		avformat_close_input(&input_avfmt);
	}

public:
	std::string path;
	uint32_t	color{};
	bool		eof;
	bool		eof_stream;
	bool		got_audio{};
	bool		got_video{};

public:
	unsigned long long last_written_sample{};
	unsigned long long audio_sample{};
	uint8_t**		   audio_dst_data;
	int				   audio_dst_data_alloc_samples{};
	int				   audio_dst_data_linesize{};
	int				   audio_dst_data_samples{};
	int				   audio_dst_data_out_samples{};
	unsigned long long audio_dst_data_out_audio_sample{};
	int				   input_avstream_audio_resampler_rate{};
	int				   input_avstream_audio_resampler_channels{};
	AVFormatContext*   input_avfmt;
	AVStream*		   input_avstream_audio;				// do not free
	AVCodecContext*	input_avstream_audio_codec_context;  // do not free
	AVFrame*		   input_avstream_audio_frame;
	AVStream*		   input_avstream_video;				// do not free
	AVCodecContext*	input_avstream_video_codec_context;  // do not free
	AVFrame*		   input_avstream_video_frame;
	AVFrame*		   input_avstream_video_frame_rgb;
	struct SwrContext* input_avstream_audio_resampler;
	struct SwsContext* input_avstream_video_resampler;
	AVPixelFormat	  input_avstream_video_resampler_format;
	int				   input_avstream_video_resampler_height{};
	int				   input_avstream_video_resampler_width{};
	signed long long   next_pts;
	signed long long   next_dts;
	AVPacket		   avpkt{};
	bool			   avpkt_valid;
	float			   adj_time{};
	float			   t{}, pt{};
};

std::vector<InputFile> input_files;
std::string			   output_file;

InputFile& current_input_file() {
	if (input_files.empty()) {
		std::string what = "input files empty";
		throw std::out_of_range(/*&*/ what);
	}

	return *(input_files.rbegin()); /* last one */
}

InputFile& new_input_file() {
	if (!input_files.empty()) {
		/* copy the last one, except for some fields */
		{
			InputFile& last = current_input_file();
			input_files.push_back(last);
		}
		{
			InputFile& last = current_input_file();
			last.reset_on_dup();
		}
	} else {
		/* make a new one with defaults */
		input_files.emplace_back();
	}

	return current_input_file();
}

volatile int DIE = 0;

HiLoComboPass audio_hilopass;

// preemphsis emuluation
LowpassFilter audio_linear_preemphasis_pre[2];
LowpassFilter audio_linear_preemphasis_post[2];

float composite_preemphasis =
	0.F;  // analog artifacts related to anything that affects the raw composite signal i.e. CATV modulation
float composite_preemphasis_cut = 1000000.F;

float vhs_out_sharpen = 1.5F;

bool  vhs_head_switching = false;
float vhs_head_switching_point =
	1.0F - ((4.5F + 0.01F /*slight error, like most VHS tapes*/) / 262.5F);  // 4 scanlines NTSC up from vsync
float vhs_head_switching_phase =
	((1.0F - 0.01F /*slight error, like most VHS tapes*/) / 262.5F);  // 4 scanlines NTSC up from vsync
float vhs_head_switching_phase_noise =
	(((1.0F / 500.F) /*slight error, like most VHS tapes*/) / 262.5F);  // 1/500th of a scanline

bool composite_in_chroma_lowpass	   = true;  // apply chroma lowpass before composite encode
bool composite_out_chroma_lowpass	  = true;
bool composite_out_chroma_lowpass_lite = true;

int   video_yc_recombine		= 0;  // additional Y/C combine/sep phases (testing)
int   video_color_fields		= 4;  // NTSC color framing
int   video_chroma_noise		= 0;
int   video_chroma_phase_noise  = 0;
int   video_chroma_loss			= 0;
int   video_noise				= 2;
int   subcarrier_amplitude		= 50;
int   subcarrier_amplitude_back = 50;
float output_audio_hiss_db		= -72.F;
float output_audio_linear_buzz =
	-42.F;  // how loud the "buzz" is audible in dBFS (S/N). Ever notice on old VHS tapes (prior to Hi-Fi) you can
			// almost hear the video signal sync pulses in the audio?
float output_audio_highpass = 20.F;		// highpass to filter out below 20Hz
float output_audio_lowpass  = 20000.F;  // lowpass to filter out above 20KHz
float vhs_linear_high_boost = 0.25F;
// NTS:
//   VHS Hi-Fi: 20Hz - 20KHz                  (70dBFS S/N)
//   VHS SP:    100Hz - 10KHz                 (42dBFS S/N)
//   VHS LP:    100Hz - 7KHz (right??)        (42dBFS S/N)
//   VHS EP:    100Hz - 4KHz                  (42dBFS S/N)
bool output_vhs_hifi		  = true;
bool output_vhs_linear_stereo = false;  // not common
bool output_vhs_linear_audio  = false;  // if true (non Hi-Fi) then we emulate hiss and noise of linear VHS tracks
										// including the video sync pulses audible in the audio.
bool emulating_vhs					 = false;
bool emulating_preemphasis			 = true;   // emulate preemphasis
bool emulating_deemphasis			 = true;   // emulate deemphasis
bool nocolor_subcarrier				 = false;  // if set, emulate subcarrier but do not decode back to color (debug)
bool nocolor_subcarrier_after_yc_sep = false;  // if set, separate luma-chroma but do not decode back to color (debug)
bool vhs_chroma_vert_blend = true;   // if set, and VHS, blend vertically the chroma scanlines (as the VHS format does)
bool vhs_svideo_out		   = false;  // if not set, and VHS, video is recombined as if composite out on VCR
bool enable_composite_emulation = true;  // if not set, video goes straight back out to the encoder.
bool enable_audio_emulation		= true;

int output_audio_hiss_level = 0;  // out of 10000

enum
{
	VHS_SP = 0,
	VHS_LP,
	VHS_EP
};

int output_vhs_tape_speed = VHS_SP;

void sigma(int /*x*/) {
	if (++DIE >= 20) { abort(); }
}

void preset_PAL() {
	output_field_rate.num = 50;
	output_field_rate.den = 1;
	output_height		  = 576;
	output_width		  = 720;
	output_pal			  = true;
	output_ntsc			  = false;
}

void preset_NTSC() {
	output_field_rate.num = 60000;
	output_field_rate.den = 1001;
	output_height		  = 480;
	output_width		  = 720;
	output_pal			  = false;
	output_ntsc			  = true;
}

static void help(const char* arg0) {
	fprintf(stderr, "%s [options]\n", arg0);
	fprintf(stderr, " -i <input file>               you can specify more than one input file, in order of layering\n");
	fprintf(stderr, " -o <output file>\n");
	fprintf(stderr, " -d <n>                        Video delay buffer (n frames)\n");
	fprintf(stderr, " -tvstd <pal|ntsc>\n");
	fprintf(stderr, " -vhs                      Emulation of VHS artifacts\n");
	fprintf(stderr, " -vhs-hifi <0|1>           (default on)\n");
	fprintf(stderr, " -vhs-speed <ep|lp|sp>     (default sp)\n");
	fprintf(stderr, " -preemphasis <0|1>        Enable preemphasis emulation\n");
	fprintf(stderr, " -deemphasis <0|1>         Enable deepmhasis emulation\n");
	fprintf(stderr, " -nocolor-subcarrier       Emulate color subcarrier but do not decode back (debug)\n");
	fprintf(
		stderr, " -nocolor-subcarrier-after-yc-sep Emulate Y/C subcarrier separation but do not decode back (debug)\n");
	fprintf(stderr, " -subcarrier-amp <0...100> Subcarrier amplitude (0 to 100 percent of luma)\n");
	fprintf(stderr, " -noise <0..100>           Noise amplitude\n");
	fprintf(stderr, " -chroma-noise <0..100>    Chroma noise amplitude\n");
	fprintf(stderr, " -audio-hiss <-120..0>     Audio hiss in decibels (0=100%%)\n");
	fprintf(stderr, " -vhs-linear-video-crosstalk <x> Emulate video crosstalk in audio. Loudness in dBFS (0=100%%)\n");
	fprintf(stderr, " -chroma-phase-noise <x>   Chroma phase noise (0...100)\n");
	fprintf(stderr, " -vhs-chroma-vblend <0|1>  Vertically blend chroma scanlines (as VHS format does)\n");
	fprintf(stderr, " -vhs-svideo <0|1>         Render VHS as if S-Video (luma and chroma separate out of VHS)\n");
	fprintf(stderr, " -yc-recomb <n>            Recombine Y/C n-times\n");
	fprintf(stderr, " -a <n>                    Pick the n'th audio stream\n");
	fprintf(stderr, " -an                       Don't render any audio stream\n");
	fprintf(stderr, " -v <n>                    Pick the n'th video stream\n");
	fprintf(stderr, " -vn                       Don't render any video stream\n");
	fprintf(stderr, " -comp-pre <s>             Composite preemphasis scale\n");
	fprintf(stderr, " -comp-cut <f>             Composite preemphasis freq\n");
	fprintf(stderr, " -comp-catv                Composite preemphasis preset, as if CATV #1\n");
	fprintf(stderr, " -comp-catv2               Composite preemphasis preset, as if CATV #2\n");
	fprintf(stderr, " -comp-catv3               Composite preemphasis preset, as if CATV #3\n");
	fprintf(stderr, " -comp-catv4               Composite preemphasis preset, as if CATV #4\n");
	fprintf(stderr, " -vi                       Render video at frame rate, interlaced\n");
	fprintf(stderr, " -vp                       Render video at field rate, progressive (with bob filter)\n");
	fprintf(stderr, " -chroma-dropout <x>       Chroma scanline dropouts (0...10000)\n");
	fprintf(stderr, " -vhs-linear-high-boost <x> Boost high frequencies in VHS audio (linear tracks)\n");
	fprintf(stderr, " -vhs-head-switching <0|1> Enable/disable VHS head switching emulation\n");
	fprintf(stderr, " -vhs-head-switching-point <x> Head switching point (0....1)\n");
	fprintf(stderr, " -vhs-head-switching-phase <x> Head switching displacement (-1....1)\n");
	fprintf(stderr, " -vhs-head-switching-noise-level <x> Head switching noise (variation)\n");
	fprintf(stderr, " -nocomp                   Don't apply emulation, just transcode\n");
	fprintf(stderr, " -ss <t>                   Start transcoding from t seconds\n");
	fprintf(stderr, " -se <t>                   Stop transcoding at t seconds\n");
	fprintf(stderr, " -t <t>                    Transcode only t seconds\n");
	fprintf(stderr, " -in-composite-lowpass <n> Enable/disable chroma lowpass on composite in\n");
	fprintf(stderr, " -out-composite-lowpass <n> Enable/disable chroma lowpass on composite out\n");
	fprintf(stderr, " -out-composite-lowpass-lite <n> Enable/disable chroma lowpass on composite out (lite)\n");
	fprintf(stderr, " -bkey-feedback <n>        Black key feedback (black level <= N)\n");
	fprintf(stderr, " -comp-phase <n>           NTSC subcarrier phase per scanline (0, 90, 180, or 270)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, " Output file will be up/down converted to 720x480 (NTSC 29.97fps) or 720x576 (PAL 25fps).\n");
	fprintf(stderr, " Output will be rendered as interlaced video.\n");
}

static unsigned long long audio_proc_count = 0;
static LowpassFilter	  audio_post_vhs_boost[2];

static inline int clips16(const int x) {
	if (x < -32768) { return -32768; }
	if (x > 32767) { return 32767; }

	return x;
}

void composite_audio_process(
	int16_t* audio, unsigned int samples) {  // number of channels = output_audio_channels, sample rate =
											 // output_audio_rate. audio is interleaved.
	assert(audio_hilopass.audiostate.size() >= output_audio_channels);
	float linear_buzz = dBFS(output_audio_linear_buzz);
	float hsync_hz	= output_ntsc ? /*NTSC*/ 15734.F : /*PAL*/ 15625.F;
	int   vsync_lines = output_ntsc ? /*NTSC*/ 525.F : /*PAL*/ 625.F;
	int   vpulse_end  = output_ntsc ? /*NTSC*/ 10.F : /*PAL*/ 12.F;
	float hpulse_end =
		output_ntsc ? /*NTSC*/ (hsync_hz * (4.7F /*us*/ / 1000000.F)) : /*PAL*/ (hsync_hz * (4.0F /*us*/ / 1000000.F));

	for (unsigned int s = 0; s < samples; s++, audio += output_audio_channels) {
		for (unsigned int c = 0; c < output_audio_channels; c++) {
			float s;

			s = static_cast<float>(audio[c]) / 32768.F;

			/* lowpass filter */
			s = audio_hilopass.audiostate[c].filter(s);

			/* preemphasis */
			if (emulating_preemphasis) {
				for (unsigned int i = 0; i < output_audio_channels; i++) {
					s = s + audio_linear_preemphasis_pre[i].highpass(s);
				}
			}

			/* that faint "buzzing" noise on linear tracks because of audio/video crosstalk */
			if (!output_vhs_hifi && linear_buzz > 0.000000001F) {
				const unsigned int oversample = 16;
				for (unsigned int oi = 0; oi < oversample; oi++) {
					float t = (((static_cast<float>(audio_proc_count) * oversample) + oi) * hsync_hz) /
							  output_audio_rate / oversample;
					float hpos  = fmod(t, 1.0F);
					int   vline = static_cast<int>(
						  fmod(floor(t + 0.0001F /*fudge*/ - hpos), static_cast<float>(vsync_lines) / 2.F));
					bool pulse = false;

					if (hpos < hpulse_end) {
						pulse = true;  // HSYNC
					}
					if (vline < vpulse_end) {
						pulse = true;  // VSYNC
					}

					if (pulse) { s -= linear_buzz / oversample / 2; }
				}
			}

			/* analog limiting (when the signal is too loud) */
			if (s > 1.0F) {
				s = 1.0F;
			} else if (s < -1.0F) {
				s = -1.0F;
			}

			/* hiss */
			if (output_audio_hiss_level != 0) {
				s += (static_cast<float>(
						 (static_cast<int>(static_cast<unsigned int>(rand()) % ((output_audio_hiss_level * 2) + 1))) -
						 output_audio_hiss_level)) /
					 20000;
			}

			/* some VCRs (at least mine) will boost higher frequencies if playing linear tracks */
			if (!output_vhs_hifi && vhs_linear_high_boost > 0) {
				s += audio_post_vhs_boost[c].highpass(s) * vhs_linear_high_boost;
			}

			/* deemphasis */
			if (emulating_deemphasis) {
				for (unsigned int i = 0; i < output_audio_channels; i++) {
					s = audio_linear_preemphasis_post[i].lowpass(s);
				}
			}

			audio[c] = clips16(s * 32768);
		}

		audio_proc_count++;
	}
}

static int parse_argv(int argc, char** argv) {
	const char* a;
	int			i;

	for (i = 1; i < argc;) {
		a = argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

			if ((strcmp(a, "h") == 0) || (strcmp(a, "help") == 0)) {
				help(argv[0]);
				return 1;
			}
			if (strcmp(a, "comp-phase-offset") == 0) {
				video_scanline_phase_shift_offset = atoi(argv[i++]);
			} else if (strcmp(a, "comp-phase") == 0) {
				video_scanline_phase_shift = atoi(argv[i++]);
				if (!(video_scanline_phase_shift == 0 || video_scanline_phase_shift == 90 ||
						video_scanline_phase_shift == 180 || video_scanline_phase_shift == 270)) {
					fprintf(stderr, "Invalid phase\n");
					return 1;
				}
			} else if (strcmp(a, "width") == 0) {
				a = argv[i++];
				if (a == nullptr) { return 1; }
				output_width = static_cast<int>(strtoul(a, nullptr, 0));
				if (output_width < 32) { return 1; }
			} else if (strcmp(a, "d") == 0) {
				a = argv[i++];
				if (a == nullptr) { return 1; }
				output_avstream_video_frame_delay = static_cast<unsigned int>(strtoul(a, nullptr, 0));
				if (output_avstream_video_frame_delay == 0 || output_avstream_video_frame_delay > 256) {
					fprintf(stderr, "Invalid delay\n");
					return 1;
				}
			} else if (strcmp(a, "i") == 0) {
				a = argv[i++];
				if (a == nullptr) { return 1; }
				new_input_file().path = a;
			} else if (strcmp(a, "o") == 0) {
				a = argv[i++];
				if (a == nullptr) { return 1; }
				output_file = a;
			} else if (strcmp(a, "tvstd") == 0) {
				a = argv[i++];

				if (strcmp(a, "pal") == 0) {
					preset_PAL();
				} else if (strcmp(a, "ntsc") == 0) {
					preset_NTSC();
				} else {
					fprintf(stderr, "Unknown tv std '%s'\n", a);
					return 1;
				}
			} else if (strcmp(a, "in-composite-lowpass") == 0) {
				composite_in_chroma_lowpass = atoi(argv[i++]) > 0;
			} else if (strcmp(a, "out-composite-lowpass") == 0) {
				composite_out_chroma_lowpass = atoi(argv[i++]) > 0;
			} else if (strcmp(a, "out-composite-lowpass-lite") == 0) {
				composite_out_chroma_lowpass_lite = atoi(argv[i++]) > 0;
			} else if (strcmp(a, "nocomp") == 0) {
				enable_composite_emulation = false;
				enable_audio_emulation	 = false;
			} else if (strcmp(a, "vhs-head-switching-point") == 0) {
				vhs_head_switching_point = atof(argv[i++]);
			} else if (strcmp(a, "vhs-head-switching-phase") == 0) {
				vhs_head_switching_phase = atof(argv[i++]);
			} else if (strcmp(a, "vhs-head-switching-noise-level") == 0) {
				vhs_head_switching_phase_noise = atof(argv[i++]);
			} else if (strcmp(a, "vhs-head-switching") == 0) {
				int x			   = atoi(argv[i++]);
				vhs_head_switching = x > 0;
			} else if (strcmp(a, "vhs-linear-high-boost") == 0) {
				vhs_linear_high_boost = atof(argv[i++]);
			} else if (strcmp(a, "comp-pre") == 0) {
				composite_preemphasis = atof(argv[i++]);
			} else if (strcmp(a, "comp-cut") == 0) {
				composite_preemphasis_cut = atof(argv[i++]);
			} else if (strcmp(a, "comp-catv") == 0) {
				composite_preemphasis	 = 7;
				composite_preemphasis_cut = 315000000 / 88;
				video_chroma_phase_noise  = 2;
			} else if (strcmp(a, "comp-catv2") == 0) {
				composite_preemphasis	 = 15;
				composite_preemphasis_cut = 315000000 / 88;
				video_chroma_phase_noise  = 4;
			} else if (strcmp(a, "comp-catv3") == 0) {
				composite_preemphasis	 = 25;
				composite_preemphasis_cut = (315000000 * 2) / 88;
				video_chroma_phase_noise  = 6;
			} else if (strcmp(a, "comp-catv4") == 0) {
				composite_preemphasis	 = 40;
				composite_preemphasis_cut = (315000000 * 4) / 88;
				video_chroma_phase_noise  = 6;
			} else if (strcmp(a, "vhs-linear-video-crosstalk") == 0) {
				output_audio_linear_buzz = atof(argv[i++]);
			} else if (strcmp(a, "chroma-phase-noise") == 0) {
				int x					 = atoi(argv[i++]);
				video_chroma_phase_noise = x;
			} else if (strcmp(a, "yc-recomb") == 0) {
				video_yc_recombine = atof(argv[i++]);
			} else if (strcmp(a, "audio-hiss") == 0) {
				output_audio_hiss_db = atof(argv[i++]);
			} else if (strcmp(a, "vhs-svideo") == 0) {
				int x		   = atoi(argv[i++]);
				vhs_svideo_out = (x > 0);
			} else if (strcmp(a, "vhs-chroma-vblend") == 0) {
				int x				  = atoi(argv[i++]);
				vhs_chroma_vert_blend = (x > 0);
			} else if (strcmp(a, "chroma-noise") == 0) {
				int x			   = atoi(argv[i++]);
				video_chroma_noise = x;
			} else if (strcmp(a, "noise") == 0) {
				int x		= atoi(argv[i++]);
				video_noise = x;
			} else if (strcmp(a, "subcarrier-amp") == 0) {
				int x					  = atoi(argv[i++]);
				subcarrier_amplitude	  = x;
				subcarrier_amplitude_back = x;
			} else if (strcmp(a, "nocolor-subcarrier") == 0) {
				nocolor_subcarrier = true;
			} else if (strcmp(a, "nocolor-subcarrier-after-yc-sep") == 0) {
				nocolor_subcarrier_after_yc_sep = true;
			} else if (strcmp(a, "chroma-dropout") == 0) {
				int x			  = atoi(argv[i++]);
				video_chroma_loss = x;
			} else if (strcmp(a, "vhs") == 0) {
				emulating_vhs			 = true;
				vhs_head_switching		 = true;
				emulating_preemphasis	= false;  // no preemphasis by default
				emulating_deemphasis	 = false;  // no preemphasis by default
				output_audio_hiss_db	 = -70;
				video_chroma_phase_noise = 4;
				video_chroma_noise		 = 16;
				video_chroma_loss		 = 4;
				video_noise				 = 4;  // VHS is a bit noisy
			} else if (strcmp(a, "preemphasis") == 0) {
				int x				  = atoi(argv[i++]);
				emulating_preemphasis = (x > 0);
			} else if (strcmp(a, "deemphasis") == 0) {
				int x				 = atoi(argv[i++]);
				emulating_deemphasis = (x > 0);
			} else if (strcmp(a, "vhs-speed") == 0) {
				a = argv[i++];

				emulating_vhs = true;  // implies -vhs
				if (strcmp(a, "ep") == 0) {
					output_vhs_tape_speed	= VHS_EP;
					video_chroma_phase_noise = 6;
					video_chroma_noise		 = 22;
					video_chroma_loss		 = 8;
					video_noise				 = 6;
				} else if (strcmp(a, "lp") == 0) {
					output_vhs_tape_speed	= VHS_LP;
					video_chroma_phase_noise = 5;
					video_chroma_noise		 = 19;
					video_chroma_loss		 = 6;
					video_noise				 = 5;
				} else if (strcmp(a, "sp") == 0) {
					output_vhs_tape_speed	= VHS_SP;
					video_chroma_phase_noise = 4;
					video_chroma_noise		 = 16;
					video_chroma_loss		 = 4;
					video_noise				 = 4;
				} else {
					fprintf(stderr, "Unknown vhs tape speed '%s'\n", a);
					return 1;
				}
			} else if (strcmp(a, "vhs-hifi") == 0) {
				int x					= atoi(argv[i++]);
				output_vhs_hifi			= (x > 0);
				output_vhs_linear_audio = !output_vhs_hifi;
				emulating_vhs			= true;  // implies -vhs
				if (output_vhs_hifi) {
					emulating_preemphasis = true;
					emulating_deemphasis  = true;
					output_audio_hiss_db  = -70;
				} else {
					output_audio_hiss_db = -42;
				}
			} else if (strcmp(a, "tvstd") == 0) {
				a = argv[i++];

				if (strcmp(a, "pal") == 0) {
					preset_PAL();
				} else if (strcmp(a, "ntsc") == 0) {
					preset_NTSC();
				} else {
					fprintf(stderr, "Unknown tv std '%s'\n", a);
					return 1;
				}
			} else {
				fprintf(stderr, "Unknown switch '%s'\n", a);
				return 1;
			}
		} else {
			fprintf(stderr, "Unhandled arg '%s'\n", a);
			return 1;
		}
	}

	if (emulating_vhs) {
		if (output_vhs_hifi) {
			output_audio_highpass = 20;		// highpass to filter out below 20Hz
			output_audio_lowpass  = 20000;  // lowpass to filter out above 20KHz
			output_audio_channels = 2;
		} else if (output_vhs_linear_audio) {
			switch (output_vhs_tape_speed) {
				case VHS_SP:
					output_audio_highpass = 100;	// highpass to filter out below 100Hz
					output_audio_lowpass  = 10000;  // lowpass to filter out above 10KHz
					break;
				case VHS_LP:
					output_audio_highpass = 100;   // highpass to filter out below 100Hz
					output_audio_lowpass  = 7000;  // lowpass to filter out above 7KHz
					break;
				case VHS_EP:
					output_audio_highpass = 100;   // highpass to filter out below 100Hz
					output_audio_lowpass  = 4000;  // lowpass to filter out above 4KHz
					break;
			}

			if (!output_vhs_linear_stereo) {
				output_audio_channels = 1;
			} else {
				output_audio_channels = 2;
			}
		}
	} else {
		// not emulating VHS
		output_audio_highpass = 20;		// highpass to filter out below 20Hz
		output_audio_lowpass  = 20000;  // lowpass to filter out above 20KHz
		output_audio_channels = 2;
	}

	if (composite_preemphasis != 0) {
		subcarrier_amplitude_back += (50 * composite_preemphasis * (315000000 / 88)) / (2 * composite_preemphasis_cut);
	}

	output_audio_hiss_level = dBFS(output_audio_hiss_db) * 5000;

	fprintf(stderr, "VHS head switching point: %.6f\n", vhs_head_switching_phase);
	fprintf(stderr, "VHS head switching noise: %.6f\n", vhs_head_switching_phase_noise);

	if (output_file.empty()) {
		fprintf(stderr, "No output file specified\n");
		return 1;
	}
	if (input_files.empty()) {
		fprintf(stderr, "No input files specified\n");
		return 1;
	}

	return 0;
}

void process_audio(InputFile& fin) {
	if (fin.audio_dst_data == nullptr || fin.audio_dst_data_out_samples == 0) { return; }

	if (enable_audio_emulation) {
		composite_audio_process(reinterpret_cast<int16_t*>(fin.audio_dst_data[0]), fin.audio_dst_data_out_samples);
	}
}

void write_out_audio(InputFile& fin) {
	if (fin.audio_dst_data == nullptr || fin.audio_dst_data_out_samples == 0) { return; }

	/* pad-fill */
	while (fin.last_written_sample < fin.audio_dst_data_out_audio_sample) {
		unsigned long long out_samples = fin.audio_dst_data_out_audio_sample - fin.last_written_sample;

		if (out_samples > output_audio_rate) { out_samples = output_audio_rate; }

		AVPacket dstpkt;
		av_init_packet(&dstpkt);
		if (av_new_packet(&dstpkt, out_samples * 2 * output_audio_channels) >= 0) {  // NTS: Will reset fields too!
			assert(dstpkt.data != nullptr);
			assert(dstpkt.size >= (out_samples * 2 * output_audio_channels));
			memset(dstpkt.data, 0, out_samples * 2 * output_audio_channels);
		}
		dstpkt.pts			= fin.last_written_sample;
		dstpkt.dts			= fin.last_written_sample;
		dstpkt.stream_index = output_avstream_audio->index;
		av_packet_rescale_ts(&dstpkt, output_avstream_audio_codec_context->time_base, output_avstream_audio->time_base);
		if (av_interleaved_write_frame(output_avfmt, &dstpkt) < 0) { fprintf(stderr, "Failed to write frame\n"); }
		av_packet_unref(&dstpkt);

		fprintf(stderr, "Pad fill %llu samples\n", out_samples);
		fin.last_written_sample += out_samples;
	}

	// write it out. TODO: At some point, support conversion to whatever the codec needs and then convert to it.
	// that way we can render directly to MP4 our VHS emulation.
	AVPacket dstpkt;
	av_init_packet(&dstpkt);
	if (av_new_packet(&dstpkt, fin.audio_dst_data_out_samples * 2 * output_audio_channels) >=
		0) {  // NTS: Will reset fields too!
		assert(dstpkt.data != nullptr);
		assert(dstpkt.size >= (fin.audio_dst_data_out_samples * 2 * output_audio_channels));
		memcpy(dstpkt.data, fin.audio_dst_data[0], fin.audio_dst_data_out_samples * 2 * output_audio_channels);
	}
	dstpkt.pts			= fin.audio_dst_data_out_audio_sample;
	dstpkt.dts			= fin.audio_dst_data_out_audio_sample;
	dstpkt.stream_index = output_avstream_audio->index;
	av_packet_rescale_ts(&dstpkt, output_avstream_audio_codec_context->time_base, output_avstream_audio->time_base);
	if (av_interleaved_write_frame(output_avfmt, &dstpkt) < 0) { fprintf(stderr, "Failed to write frame\n"); }
	av_packet_unref(&dstpkt);

	fin.audio_sample = fin.last_written_sample = fin.audio_dst_data_out_audio_sample + fin.audio_dst_data_out_samples;
}

void output_frame(AVFrame* frame, unsigned long long field_number) {
	int		 gotit = 0;
	AVPacket pkt;

	av_init_packet(&pkt);
	if (av_new_packet(&pkt, 50000000 / 8) < 0) {
		fprintf(stderr, "Failed to alloc vid packet\n");
		return;
	}

	frame->key_frame = (field_number % (15ULL * 2ULL)) == 0 ? 1 : 0;

	{
		frame->interlaced_frame = 0;
		frame->pts				= field_number;
		pkt.pts					= field_number;
		pkt.dts					= field_number;
	}

	fprintf(stderr,
		"\x0D"
		"Output field %llu ",
		field_number);
	fflush(stderr);
	if (avcodec_encode_video2(output_avstream_video_codec_context, &pkt, frame, &gotit) == 0) {
		if (gotit != 0) {
			pkt.stream_index = output_avstream_video->index;
			av_packet_rescale_ts(
				&pkt, output_avstream_video_codec_context->time_base, output_avstream_video->time_base);

			if (av_interleaved_write_frame(output_avfmt, &pkt) < 0) {
				fprintf(stderr, "AV write frame failed video\n");
			}
		}
	}

	av_packet_unref(&pkt);
}

void RGB_to_YIQ(int& Y, int& I, int& Q, int r, int g, int b) {
	double dY;

	dY = (0.30 * r) + (0.59 * g) + (0.11 * b);

	Y = static_cast<int>(256 * dY);
	I = static_cast<int>(256 * ((-0.27 * (b - dY)) + (0.74 * (r - dY))));
	Q = static_cast<int>(256 * ((0.41 * (b - dY)) + (0.48 * (r - dY))));
}

void YIQ_to_RGB(int& r, int& g, int& b, int Y, int I, int Q) {
	// FIXME
	r = static_cast<int>(((1.000 * Y) + (0.956 * I) + (0.621 * Q)) / 256);
	g = static_cast<int>(((1.000 * Y) + (-0.272 * I) + (-0.647 * Q)) / 256);
	b = static_cast<int>(((1.000 * Y) + (-1.106 * I) + (1.703 * Q)) / 256);
	if (r < 0) {
		r = 0;
	} else if (r > 255) {
		r = 255;
	}
	if (g < 0) {
		g = 0;
	} else if (g > 255) {
		g = 255;
	}
	if (b < 0) {
		b = 0;
	} else if (b > 255) {
		b = 255;
	}
}

/* lighter-weight filtering, probably what your old CRT does to reduce color fringes a bit */
void composite_lowpass_tv(
	AVFrame* dstframe, int* /*fY*/, int* fI, int* fQ, unsigned int field, unsigned long long /*fieldno*/) {
	unsigned int x;
	unsigned int y;

	{
		for (unsigned int p = 1; p <= 2; p++) {
			for (y = field; y < dstframe->height; y += 2) {
				int*		  P = ((p == 1) ? fI : fQ) + (dstframe->width * y);
				LowpassFilter lp[3];
				float		  cutoff;
				int			  delay;
				float		  s;

				cutoff = 2600000.F;
				delay  = 1;

				for (auto& f : lp) {
					f.setFilter((315000000.00F * 4.F) / 88.F, cutoff);  // 315/88 Mhz rate * 4
					f.resetFilter(0.F);
				}

				for (x = 0; x < dstframe->width; x++) {
					s = P[x];
					for (auto& f : lp) { s = f.lowpass(s); }
					if (x >= delay) { P[x - delay] = s; }
				}
			}
		}
	}
}

void composite_lowpass(
	AVFrame* dstframe, int* /*fY*/, int* fI, int* fQ, unsigned int field, unsigned long long /*fieldno*/) {
	unsigned int x;
	unsigned int y;

	{ /* lowpass the chroma more. composite video does not allocate as much bandwidth to color as luma. */
		for (unsigned int p = 1; p <= 2; p++) {
			for (y = field; y < dstframe->height; y += 2) {
				int*		  P = ((p == 1) ? fI : fQ) + (dstframe->width * y);
				LowpassFilter lp[3];
				float		  cutoff;
				int			  delay;
				float		  s;

				// NTSC YIQ bandwidth: I=1.3MHz Q=0.6MHz
				cutoff = (p == 1) ? 1300000.F : 600000.F;
				delay  = (p == 1) ? 2 : 4;

				for (auto& f : lp) {
					f.setFilter((315000000.00F * 4.F) / 88.F, cutoff);  // 315/88 Mhz rate * 4
					f.resetFilter(0.F);
				}

				for (x = 0; x < dstframe->width; x++) {
					s = P[x];
					for (auto& f : lp) { s = f.lowpass(s); }
					if (x >= delay) { P[x - delay] = s; }
				}
			}
		}
	}
}

void chroma_into_luma(AVFrame* dstframe, int* fY, int* fI, int* fQ, unsigned int field, unsigned long long fieldno,
	int subcarrier_amplitude) {
	/* render chroma into luma, fake subcarrier */
	unsigned int x;
	unsigned int y;

	for (y = field; y < dstframe->height; y += 2) {
		static const int8_t Umult[4] = {1, 0, -1, 0};
		static const int8_t Vmult[4] = {0, 1, 0, -1};
		int*				Y		 = fY + (y * dstframe->width);
		int*				I		 = fI + (y * dstframe->width);
		int*				Q		 = fQ + (y * dstframe->width);
		unsigned int		xc		 = dstframe->width;
		unsigned int		xi;

		if (video_scanline_phase_shift == 90) {
			xi = (fieldno + video_scanline_phase_shift_offset + (y >> 1)) & 3;
		} else if (video_scanline_phase_shift == 180) {
			xi = (((fieldno + y) & 2) + video_scanline_phase_shift_offset) & 3;
		} else if (video_scanline_phase_shift == 270) {
			xi = (fieldno + video_scanline_phase_shift_offset - (y >> 1)) & 3;
		} else {
			xi = video_scanline_phase_shift_offset & 3;
		}

		/* remember: this code assumes 4:2:2 */
		/* NTS: the subcarrier is two sine waves superimposed on top of each other, 90 degrees apart */
		for (x = 0; x < xc; x++) {
			unsigned int sxi = xi + x;
			int			 chroma;

			chroma = I[x] * subcarrier_amplitude * Umult[sxi & 3];
			chroma += Q[x] * subcarrier_amplitude * Vmult[sxi & 3];
			Y[x] += (chroma / 50);
			I[x] = 0;
			Q[x] = 0;
		}
	}
}

void chroma_from_luma(AVFrame* dstframe, int* fY, int* fI, int* fQ, unsigned int field, unsigned long long fieldno,
	int subcarrier_amplitude) {
	/* decode color from luma */
	int			 chroma[dstframe->width];  // WARNING: This is more GCC-specific C++ than normal
	unsigned int x;
	unsigned int y;

	for (y = field; y < dstframe->height; y += 2) {
		int* Y		  = fY + (y * dstframe->width);
		int* I		  = fI + (y * dstframe->width);
		int* Q		  = fQ + (y * dstframe->width);
		int  delay[4] = {0, 0, 0, 0};
		int  sum	  = 0;
		int  c;

		// precharge by 2 pixels to center box blur
		delay[2] = Y[0];
		sum += delay[2];
		delay[3] = Y[1];
		sum += delay[3];
		for (x = 0; x < dstframe->width; x++) {
			if ((x + 2) < dstframe->width) {
				c = Y[x + 2];
			} else {
				c = 0;
			}

			sum -= delay[0];
			for (unsigned int j = 0; j < (4 - 1); j++) { delay[j] = delay[j + 1]; }
			delay[3] = c;
			sum += delay[3];
			Y[x]	  = sum / 4;
			chroma[x] = c - Y[x];
		}

		{
			unsigned int xi = 0;

			if (video_scanline_phase_shift == 90) {
				xi = (fieldno + video_scanline_phase_shift_offset + (y >> 1)) & 3;
			} else if (video_scanline_phase_shift == 180) {
				xi = (((fieldno + y) & 2) + video_scanline_phase_shift_offset) & 3;
			} else if (video_scanline_phase_shift == 270) {
				xi = (fieldno + video_scanline_phase_shift_offset - (y >> 1)) & 3;
			} else {
				xi = video_scanline_phase_shift_offset & 3;
			}

			for (x = ((4 - xi) & 3); (x + 3) < dstframe->width;
				 x += 4) {  // flip the part of the sine wave that would correspond to negative U and V values
				chroma[x + 2] = -chroma[x + 2];
				chroma[x + 3] = -chroma[x + 3];
			}

			for (x = 0; x < dstframe->width; x++) { chroma[x] = (chroma[x] * 50) / subcarrier_amplitude; }

			/* decode the color right back out from the subcarrier we generated */
			for (x = 0; (x + xi + 1) < dstframe->width; x += 2) {
				I[x] = -chroma[x + xi + 0];
				Q[x] = -chroma[x + xi + 1];
			}
			for (; x < dstframe->width; x += 2) {
				I[x] = 0;
				Q[x] = 0;
			}
			for (x = 0; (x + 2) < dstframe->width; x += 2) {
				I[x + 1] = (I[x] + I[x + 2]) >> 1;
				Q[x + 1] = (Q[x] + Q[x + 2]) >> 1;
			}
			for (; x < dstframe->width; x++) {
				I[x] = 0;
				Q[x] = 0;
			}
		}
	}
}

// This code assumes ARGB and the frame match resolution/
void composite_layer(
	AVFrame* dstframe, AVFrame* srcframe, InputFile& /*inputfile*/, unsigned int field, unsigned long long fieldno) {
	unsigned char opposite;
	unsigned int  shr;
	auto		  dstframe_pixels = dstframe->width * dstframe->height;
	int *		  fY, *fI, *fQ;
	int			  r;
	int			  g;
	int			  b;

	if (dstframe == nullptr || srcframe == nullptr) { return; }
	if (dstframe->data[0] == nullptr || srcframe->data[0] == nullptr) { return; }
	if (dstframe->linesize[0] < (dstframe->width * 4)) {
		return;  // ARGB
	}
	if (srcframe->linesize[0] < (srcframe->width * 4)) {
		return;  // ARGB
	}
	if (dstframe->width != srcframe->width) { return; }
	if (dstframe->height != srcframe->height) { return; }

	if (srcframe->interlaced_frame != 0) {
		opposite = (srcframe->top_field_first != 0 ? 1 : 0);
	} else {
		opposite = 0;
	}

	fY = new int[dstframe_pixels]{0};
	fI = new int[dstframe_pixels]{0};
	fQ = new int[dstframe_pixels]{0};

	for (auto row = field; row < dstframe->height; row += 2) {
		for (auto col = 0; col < dstframe->width; col++) {
			/*Getting the pixel location is a bit complicated, because each line from srcframe->data[0] has padding
			 * added to it. We have to use `linesize` to get how many bytes each line actually takes up and index into
			 * the pixel buffer with that.*/
			auto pixel = reinterpret_cast<uint8_t*>(
							 srcframe->data[0] +	   // start of pixel buffer
							 (srcframe->linesize[0] *  // size of row of pixels, including padding
								 std::min(row + opposite, static_cast<unsigned int>(dstframe->height) - 1U))) +
						 (col * 4);  // There are 4 1-byte colors per pixel

			// Colors in srcframe are actually BGRA
			b = pixel[0];
			g = pixel[1];
			r = pixel[2];

			auto idx = (row * dstframe->width) + col;
			RGB_to_YIQ(fY[idx], fI[idx], fQ[idx], r, g, b);
		}
	}

	if (composite_in_chroma_lowpass) { composite_lowpass(dstframe, fY, fI, fQ, field, fieldno); }

	chroma_into_luma(dstframe, fY, fI, fQ, field, fieldno, subcarrier_amplitude);

	/* video composite preemphasis */
	if (composite_preemphasis != 0 && composite_preemphasis_cut > 0) {
		for (auto y = field; y < dstframe->height; y += 2) {
			int*		  Y = fY + (y * dstframe->width);
			LowpassFilter pre;
			float		  s;

			pre.setFilter(
				(315000000.00F * 4.F) / 88.F, composite_preemphasis_cut);  // 315/88 Mhz rate * 4  vs 1.0MHz cutoff
			pre.resetFilter(16.F);
			for (auto x = 0; x < dstframe->width; x++) {
				s = Y[x];
				s += pre.highpass(s) * composite_preemphasis;
				Y[x] = static_cast<int>(s);
			}
		}
	}

	/* add video noise */
	if (video_noise != 0) {
		int noise	 = 0;
		int noise_mod = (video_noise * 2) + 1; /* ,noise_mod = (video_noise * 255) / 100; */

		for (auto y = field; y < dstframe->height; y += 2) {
			int* Y = fY + (y * dstframe->width);

			for (auto x = 0; x < dstframe->width; x++) {
				Y[x] += noise;
				noise += (static_cast<int>(static_cast<unsigned int>(rand()) % noise_mod)) - video_noise;
				noise /= 2;
			}
		}
	}

	// VHS head switching noise
	if (vhs_head_switching) {
		unsigned int twidth = dstframe->width + (dstframe->width / 10);
		unsigned int tx;
		unsigned int x;
		unsigned int p;
		unsigned int x2;
		unsigned int shy   = 0;
		float		 noise = 0.F;
		int			 shif;
		int			 ishif;
		int			 y;
		float		 t;

		if (vhs_head_switching_phase_noise != 0) {
			unsigned int x = static_cast<unsigned int>(rand()) * static_cast<unsigned int>(rand()) *
							 static_cast<unsigned int>(rand()) * static_cast<unsigned int>(rand());
			x %= 2000000000U;
			noise = (static_cast<float>(x) / 1000000000U) - 1.0F;
			noise *= vhs_head_switching_phase_noise;
		}

		if (output_ntsc) {
			t = twidth * 262.5F;
		} else {
			t = twidth * 312.5F;
		}

		p = static_cast<unsigned int>(fmod(vhs_head_switching_point + noise, 1.0F) * t);
		y = ((p / twidth) * 2) + field;

		p = static_cast<unsigned int>(fmod(vhs_head_switching_phase + noise, 1.0F) * t);
		x = p % twidth;

		if (output_ntsc) {
			y -= (262 - 240) * 2;
		} else {
			y -= (312 - 288) * 2;
		}

		tx = x;
		if (x >= (twidth / 2)) {
			ishif = x - twidth;
		} else {
			ishif = x;
		}

		shif = 0;
		while (y < dstframe->height) {
			if (y >= 0) {
				int* Y = fY + (y * dstframe->width);

				if (shif != 0) {
					int tmp[twidth];

					/* WARNING: This is not 100% accurate. On real VHS you'd see the line shifted over and the next
					 * line's contents after hsync. */

					/* luma. the chroma subcarrier is there, so this is all we have to do. */
					x2 = (tx + twidth + static_cast<unsigned int>(shif)) % twidth;
					memset(tmp, 0, sizeof(tmp));
					memcpy(tmp, Y, dstframe->width * sizeof(int));
					for (x = tx; x < dstframe->width; x++) {
						Y[x] = tmp[x2];
						if ((++x2) == twidth) { x2 = 0; }
					}
				}
			}

			if (shy == 0) {
				shif = ishif;
			} else {
				shif = (shif * 7) / 8;
			}

			tx = 0;
			y += 2;
			shy++;
		}
	}

	if (!nocolor_subcarrier) { chroma_from_luma(dstframe, fY, fI, fQ, field, fieldno, subcarrier_amplitude_back); }

	/* add video noise */
	if (video_chroma_noise != 0) {
		int noiseU	= 0;
		int noiseV	= 0;
		int noise_mod = (video_chroma_noise * 255) / 100;

		for (auto y = field; y < dstframe->height; y += 2) {
			int* U = fI + (y * dstframe->width);
			int* V = fQ + (y * dstframe->width);

			for (auto x = 0; x < dstframe->width; x++) {
				U[x] += noiseU;
				V[x] += noiseV;
				noiseU += (static_cast<int>(static_cast<unsigned int>(rand()) % ((video_chroma_noise * 2) + 1))) -
						  video_chroma_noise;
				noiseU /= 2;
				noiseV += (static_cast<int>(static_cast<unsigned int>(rand()) % ((video_chroma_noise * 2) + 1))) -
						  video_chroma_noise;
				noiseV /= 2;
			}
		}
	}
	if (video_chroma_phase_noise != 0) {
		int   noise		= 0;
		int   noise_mod = (video_chroma_noise * 255) / 100;
		float pi;
		float u;
		float v;
		float u_;
		float v_;
		float sinpi;
		float cospi;

		for (auto y = field; y < dstframe->height; y += 2) {
			int* U = fI + (y * dstframe->width);
			int* V = fQ + (y * dstframe->width);

			noise += (static_cast<int>(static_cast<unsigned int>(rand()) % ((video_chroma_phase_noise * 2) + 1))) -
					 video_chroma_phase_noise;
			noise /= 2;
			pi = (static_cast<float>(noise) * M_PI) / 100.F;

			sinpi = sin(pi);
			cospi = cos(pi);

			for (auto x = 0; x < dstframe->width; x++) {
				u = U[x];  // think of 'u' as x-coord
				v = V[x];  // and 'v' as y-coord

				// then this 2D rotation then makes more sense
				u_ = (u * cospi) - (v * sinpi);
				v_ = (u * sinpi) + (v * cospi);

				// put it back
				U[x] = u_;
				V[x] = v_;
			}
		}
	}

	// NTS: At this point, the video best resembles what you'd get from a typical DVD player's composite video output.
	//      Slightly blurry, some color artifacts, and edges will have that "buzz" effect, but still a good picture.

	if (emulating_vhs) {
		float luma_cut;
		float chroma_cut;
		int   chroma_delay;

		switch (output_vhs_tape_speed) {
			case VHS_SP:
				luma_cut	 = 2400000.F;  // 3.0MHz x 80%
				chroma_cut   = 320000.F;   // 400KHz x 80%
				chroma_delay = 9;
				break;
			case VHS_LP:
				luma_cut	 = 1900000.F;  // ..
				chroma_cut   = 300000.F;   // 375KHz x 80%
				chroma_delay = 12;
				break;
			case VHS_EP:
				luma_cut	 = 1400000.F;  // ..
				chroma_cut   = 280000.F;   // 350KHz x 80%
				chroma_delay = 14;
				break;
			default: abort();
		};

		// luma lowpass
		for (auto y = field; y < dstframe->height; y += 2) {
			int*		  Y = fY + (y * dstframe->width);
			LowpassFilter lp[3];
			LowpassFilter pre;
			float		  s;

			for (auto& f : lp) {
				f.setFilter((315000000.00F * 4.F) / 88.F, luma_cut);  // 315/88 Mhz rate * 4  vs 3.0MHz cutoff
				f.resetFilter(16);
			}
			pre.setFilter((315000000.00F * 4.F) / 88.F, luma_cut);  // 315/88 Mhz rate * 4  vs 1.0MHz cutoff
			pre.resetFilter(16.F);
			for (auto x = 0; x < dstframe->width; x++) {
				s = Y[x];
				for (auto& f : lp) { s = f.lowpass(s); }
				s += pre.highpass(s) * 1.6F;
				Y[x] = s;
			}
		}

		// chroma lowpass
		for (auto y = field; y < dstframe->height; y += 2) {
			int*		  U = fI + (y * dstframe->width);
			int*		  V = fQ + (y * dstframe->width);
			LowpassFilter lpU[3];
			LowpassFilter lpV[3];
			float		  s;

			for (unsigned int f = 0; f < 3; f++) {
				lpU[f].setFilter((315000000.00F * 4.F) / 88.F,
					chroma_cut);  // 315/88 Mhz rate * 4 (divide by 2 for 4:2:2) vs 400KHz cutoff
				lpU[f].resetFilter(0);
				lpV[f].setFilter((315000000.00F * 4.F) / 88.F,
					chroma_cut);  // 315/88 Mhz rate * 4 (divide by 2 for 4:2:2) vs 400KHz cutoff
				lpV[f].resetFilter(0.F);
			}
			for (auto x = 0; x < dstframe->width; x++) {
				s = U[x];
				for (auto& f : lpU) { s = f.lowpass(s); }
				if (x >= chroma_delay) { U[x - chroma_delay] = s; }

				s = V[x];
				for (auto& f : lpV) { s = f.lowpass(s); }
				if (x >= chroma_delay) { V[x - chroma_delay] = s; }
			}
		}

		// VHS decks also vertically smear the chroma subcarrier using a delay line
		// to add the previous line's color subcarrier to the current line's color subcarrier.
		// note that phase changes in NTSC are compensated for by the VHS deck to make the
		// phase line up per scanline (else summing the previous line's carrier would
		// cancel it out).
		if (vhs_chroma_vert_blend && output_ntsc) {
			int delayU[dstframe->width];
			int delayV[dstframe->width];

			memset(delayU, 0, dstframe->width * sizeof(int));
			memset(delayV, 0, dstframe->width * sizeof(int));
			for (auto y = (field + 2); y < dstframe->height; y += 2) {
				int* U = fI + (y * dstframe->width);
				int* V = fQ + (y * dstframe->width);
				int  cU;
				int  cV;

				for (auto x = 0; x < dstframe->width; x++) {
					cU		  = U[x];
					cV		  = V[x];
					U[x]	  = (delayU[x] + cU + 1) >> 1;
					V[x]	  = (delayV[x] + cV + 1) >> 1;
					delayU[x] = cU;
					delayV[x] = cV;
				}
			}
		}

		// VHS decks tend to sharpen the picture on playback
		if (true /*TODO make option*/) {
			// luma
			for (auto y = field; y < dstframe->height; y += 2) {
				int*		  Y = fY + (y * dstframe->width);
				LowpassFilter lp[3];
				float		  s;
				float		  ts;

				for (auto& f : lp) {
					f.setFilter((315000000.00F * 4.F) / 88.F, luma_cut * 4.F);  // 315/88 Mhz rate * 4  vs 3.0MHz cutoff
					f.resetFilter(0.F);
				}
				for (auto x = 0; x < dstframe->width; x++) {
					s = ts = Y[x];
					for (auto& f : lp) { ts = f.lowpass(ts); }
					Y[x] = s + ((s - ts) * vhs_out_sharpen * 2);
				}
			}
		}

		if (!vhs_svideo_out) {
			chroma_into_luma(dstframe, fY, fI, fQ, field, fieldno, subcarrier_amplitude);
			chroma_from_luma(dstframe, fY, fI, fQ, field, fieldno, subcarrier_amplitude);
		}
	}

	if (video_chroma_loss != 0) {
		for (auto y = field; y < dstframe->height; y += 2) {
			int* U = fI + (y * dstframe->width);
			int* V = fQ + (y * dstframe->width);

			if (((static_cast<unsigned int>(rand())) % 100000) < video_chroma_loss) {
				memset(U, 0, dstframe->width * sizeof(int));
				memset(V, 0, dstframe->width * sizeof(int));
			}
		}
	}

	if (composite_out_chroma_lowpass) {
		if (composite_out_chroma_lowpass_lite) {
			composite_lowpass_tv(dstframe, fY, fI, fQ, field, fieldno);
		} else {
			composite_lowpass(dstframe, fY, fI, fQ, field, fieldno);
		}
	}

	for (auto y = field; y < dstframe->height; y += 2) {
		auto dscan = reinterpret_cast<uint32_t*>(dstframe->data[0] + (dstframe->linesize[0] * y));
		for (auto x = 0; x < dstframe->width; x++, dscan++) {
			auto idx = (y * dstframe->width) + x;
			int  r, g, b;
			YIQ_to_RGB(r, g, b, fY[idx], fI[idx], fQ[idx]);
			*dscan = (r << 16) + (g << 8) + b;
		}
	}

	delete[] fY;
	delete[] fI;
	delete[] fQ;
}

int main(int argc, char** argv) {
	std::thread EncoderThread([]() {
		for (auto& params : *EncoderChannel) {
			auto& [p1, p2] = params;
			output_frame(p1, p2);
		}
	});

	preset_NTSC();
	if (parse_argv(argc, argv) != 0) { return 1; }

	av_register_all();
	avformat_network_init();
	avcodec_register_all();

	/* open all input files */
	for (auto& input_file : input_files) {
		if (!input_file.open_input()) {
			fprintf(stderr, "Failed to open %s\n", input_file.path.c_str());
			return 1;
		}
	}

	/* open output file */
	assert(output_avfmt == nullptr);
	if (avformat_alloc_output_context2(&output_avfmt, nullptr, nullptr, output_file.c_str()) < 0) {
		fprintf(stderr, "Failed to open output file\n");
		return 1;
	}

	{
		output_avstream_audio = avformat_new_stream(output_avfmt, nullptr);
		if (output_avstream_audio == nullptr) {
			fprintf(stderr, "Unable to create output audio stream\n");
			return 1;
		}

		output_avstream_audio_codec_context = output_avstream_audio->codec;
		if (output_avstream_audio_codec_context == nullptr) {
			fprintf(stderr, "Output stream audio no codec context?\n");
			return 1;
		}

		if (output_audio_channels == 2) {
			output_avstream_audio_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
		} else {
			output_avstream_audio_codec_context->channel_layout = AV_CH_LAYOUT_MONO;
		}

		output_avstream_audio_codec_context->sample_rate = output_audio_rate;
		output_avstream_audio_codec_context->channels	= output_audio_channels;
		output_avstream_audio_codec_context->sample_fmt  = AV_SAMPLE_FMT_S16;
		output_avstream_audio_codec_context->time_base   = (AVRational){1, output_audio_rate};
		output_avstream_audio->time_base				 = output_avstream_audio_codec_context->time_base;

		if ((output_avfmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
			output_avstream_audio_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}

		if (avcodec_open2(output_avstream_audio_codec_context, avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE), nullptr) <
			0) {
			fprintf(stderr, "Output stream cannot open codec\n");
			return 1;
		}
	}

	{
		output_avstream_video = avformat_new_stream(output_avfmt, nullptr);
		if (output_avstream_video == nullptr) {
			fprintf(stderr, "Unable to create output video stream\n");
			return 1;
		}

		output_avstream_video_codec_context = output_avstream_video->codec;
		if (output_avstream_video_codec_context == nullptr) {
			fprintf(stderr, "Output stream video no codec context?\n");
			return 1;
		}

		avcodec_get_context_defaults3(output_avstream_video_codec_context, avcodec_find_encoder(AV_CODEC_ID_H264));
		output_avstream_video_codec_context->width				 = output_width;
		output_avstream_video_codec_context->height				 = output_height;
		output_avstream_video_codec_context->sample_aspect_ratio = output_aspect_ratio;
		output_avstream_video_codec_context->pix_fmt			 = AV_PIX_FMT_YUV444P;
		av_opt_set_int(output_avstream_video_codec_context, "crf", 0, AV_OPT_SEARCH_CHILDREN);
		av_opt_set(output_avstream_video_codec_context, "preset", "ultrafast", AV_OPT_SEARCH_CHILDREN);
		av_opt_set(output_avstream_video_codec_context, "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN);
		output_avstream_video_codec_context->time_base = (AVRational){output_field_rate.den, output_field_rate.num};

		output_avstream_video->time_base = output_avstream_video_codec_context->time_base;
		if ((output_avfmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
			output_avstream_video_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}

		if (avcodec_open2(output_avstream_video_codec_context, avcodec_find_encoder(AV_CODEC_ID_H264), nullptr) < 0) {
			fprintf(stderr, "Output stream cannot open codec\n");
			return 1;
		}
	}

	if ((output_avfmt->oformat->flags & AVFMT_NOFILE) == 0) {
		if (avio_open(&output_avfmt->pb, output_file.c_str(), AVIO_FLAG_WRITE) < 0) {
			fprintf(stderr, "Output file cannot open file\n");
			return 1;
		}
	}

	if (avformat_write_header(output_avfmt, nullptr) < 0) {
		fprintf(stderr, "Failed to write header\n");
		return 1;
	}

	/* soft break on CTRL+C */
	signal(SIGINT, sigma);
	signal(SIGHUP, sigma);
	signal(SIGQUIT, sigma);
	signal(SIGTERM, sigma);

	/* prepare audio filtering */
	audio_hilopass.setChannels(output_audio_channels);
	audio_hilopass.setRate(output_audio_rate);
	audio_hilopass.setCutoff(output_audio_lowpass, output_audio_highpass);  // hey, our filters aren't perfect
	audio_hilopass.setPasses(6);
	audio_hilopass.init();

	/* high boost on playback */
	for (auto& i : audio_post_vhs_boost) { i.setFilter(output_audio_rate, 10000); }

	// TODO(max): VHS Hi-Fi is also documented to use 2:1 companding when recording, which we do not yet emulate

	if (emulating_preemphasis) {
		if (output_vhs_hifi) {
			for (unsigned int i = 0; i < output_audio_channels; i++) {
				audio_linear_preemphasis_pre[i].setFilter(
					output_audio_rate, 16000 /*FIXME: Guess! Also let user set this.*/);
			}
		} else {
			for (unsigned int i = 0; i < output_audio_channels; i++) {
				audio_linear_preemphasis_pre[i].setFilter(
					output_audio_rate, 8000 /*FIXME: Guess! Also let user set this.*/);
			}
		}
	}
	if (emulating_deemphasis) {
		if (output_vhs_hifi) {
			for (unsigned int i = 0; i < output_audio_channels; i++) {
				audio_linear_preemphasis_post[i].setFilter(
					output_audio_rate, 16000 /*FIXME: Guess! Also let user set this.*/);
			}
		} else {
			for (unsigned int i = 0; i < output_audio_channels; i++) {
				audio_linear_preemphasis_post[i].setFilter(
					output_audio_rate, 8000 /*FIXME: Guess! Also let user set this.*/);
			}
		}
	}

	/* prepare video encoding */
	for (size_t i = 0; i <= output_avstream_video_frame_delay; i++) {
		AVFrame* nf;

		nf = av_frame_alloc();
		if (nf == nullptr) {
			fprintf(stderr, "Failed to alloc video frame\n");
			return 1;
		}
		nf->format = AV_PIX_FMT_BGRA;
		nf->height = output_height;
		nf->width  = output_width;
		if (av_frame_get_buffer(nf, 64) < 0) {
			fprintf(stderr, "Failed to alloc render frame\n");
			return 1;
		}

		// zero the frame ONCE.
		// what we want is, if a threshhold is given for the first input file,
		// that it means the user wants us to cause a "hall of mirrors" effect where keying happens.
		memset(nf->data[0], 0, nf->linesize[0] * nf->height);

		output_avstream_video_frame.push_back(nf);
	}

	{
		output_avstream_video_encode_frame = av_frame_alloc();
		if (output_avstream_video_encode_frame == nullptr) {
			fprintf(stderr, "Failed to alloc video frame3\n");
			return 1;
		}
		av_frame_set_colorspace(output_avstream_video_encode_frame, AVCOL_SPC_SMPTE170M);
		av_frame_set_color_range(output_avstream_video_encode_frame, AVCOL_RANGE_MPEG);
		output_avstream_video_encode_frame->format = output_avstream_video_codec_context->pix_fmt;
		output_avstream_video_encode_frame->height = output_height;
		output_avstream_video_encode_frame->width  = output_width;
		if (av_frame_get_buffer(output_avstream_video_encode_frame, 64) < 0) {
			fprintf(stderr, "Failed to alloc render frame2\n");
			return 1;
		}

		memset(output_avstream_video_encode_frame->data[0], 16,
			output_avstream_video_encode_frame->linesize[0] * output_avstream_video_encode_frame->height);
		memset(output_avstream_video_encode_frame->data[1], 128,
			output_avstream_video_encode_frame->linesize[1] * output_avstream_video_encode_frame->height);
		memset(output_avstream_video_encode_frame->data[2], 128,
			output_avstream_video_encode_frame->linesize[2] * output_avstream_video_encode_frame->height);
	}

	if (output_avstream_video_resampler == nullptr) {
		output_avstream_video_resampler = sws_getContext(
			// source
			output_avstream_video_frame[0]->width, output_avstream_video_frame[0]->height,
			static_cast<AVPixelFormat>(output_avstream_video_frame[0]->format),
			// dest
			output_avstream_video_encode_frame->width, output_avstream_video_encode_frame->height,
			static_cast<AVPixelFormat>(output_avstream_video_encode_frame->format),
			// opt
			SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		if (output_avstream_video_resampler == nullptr) {
			fprintf(stderr, "Failed to alloc ARGB -> codec converter\n");
			return 1;
		}
	}

	/* run all inputs and render to output, until done */
	{
		bool			 eof;
		bool			 copyaud;
		signed long long upto	= 0;
		signed long long current = 0;

		do {
			if (DIE != 0) { break; }

			eof		= true;
			copyaud = false;
			for (auto& input_file : input_files) {
				if (!input_file.eof) {
					eof = false;
					if (!(input_file.got_audio) && !(input_file.got_video)) { input_file.next_packet(); }

					if (input_file.got_audio) {
						/* we don't do anything with audio, but we do copy through the first input file's audio */
						if (!copyaud) {
							copyaud = true;
							process_audio(
								input_file);  // NTS: We do chroma/color keying, we don't do anything with audio
							write_out_audio(input_file);
						}
						input_file.got_audio = false;
					}
				}
			}

			upto = -1LL;
			for (auto& input_file : input_files) {
				if (!input_file.eof) {
					if (input_file.input_avstream_video_frame != nullptr) {
						if (input_file.got_video) {
							if (input_file.input_avstream_video_frame->pkt_pts != AV_NOPTS_VALUE) {
								if (upto == (-1LL) || upto > input_file.input_avstream_video_frame->pkt_pts) {
									upto = input_file.input_avstream_video_frame->pkt_pts;
								}
							}

							if (input_file.input_avstream_video_frame->pkt_pts == AV_NOPTS_VALUE ||
								current >= input_file.input_avstream_video_frame->pkt_pts) {
								input_file.frame_copy_scale();
								input_file.got_video = false;
							}
						} else {
							input_file.got_video = false;
							upto				 = current;
						}
					} else {
						input_file.got_video = false;
						upto				 = current;
					}
				} else {
					if (input_file.got_video) {
						input_file.frame_copy_scale();
						input_file.got_video = false;
					}
				}
			}

			while (current < upto) {
				for (auto& input_file : input_files) {
					if (!input_file.eof) {
						if (input_file.input_avstream_video_frame != nullptr) {
							if (input_file.got_video) {
								if (input_file.input_avstream_video_frame->pkt_pts == AV_NOPTS_VALUE ||
									current >= input_file.input_avstream_video_frame->pkt_pts) {
									input_file.frame_copy_scale();
									input_file.got_video = false;
								}
							} else {
								input_file.got_video = false;
							}
						} else {
							input_file.got_video = false;
							upto				 = current;
						}
					} else {
						if (input_file.got_video) {
							input_file.frame_copy_scale();
							input_file.got_video = false;
						}
					}

					// composite the layer, keying against the color. all code assumes ARGB
					composite_layer(output_avstream_video_frame[output_avstream_video_frame_index],
						input_file.input_avstream_video_frame_rgb, input_file, (current & 1) ^ 1, current);
				}

				// field deinterlace
				/*{
					unsigned int field = (current & 1) ^ 1;
					unsigned int sy;
					unsigned int dy;
					unsigned int y;

					if (field != 0u) {
						for (y = field; y < output_avstream_video_frame[output_avstream_video_frame_index]->height;
							 y += 2) {
							auto* d = reinterpret_cast<uint32_t*>(
								output_avstream_video_frame[output_avstream_video_frame_index]->data[0] +
								(output_avstream_video_frame[output_avstream_video_frame_index]->linesize[0] *
									(y - 1)));
							auto* s = reinterpret_cast<uint32_t*>(
								output_avstream_video_frame[output_avstream_video_frame_index]->data[0] +
								(output_avstream_video_frame[output_avstream_video_frame_index]->linesize[0] * y));

							memcpy(d, s,
								sizeof(uint32_t) *
									output_avstream_video_frame[output_avstream_video_frame_index]->width);
						}
					} else {  // field == 0
						for (y = 1; (y + 1) < output_avstream_video_frame[output_avstream_video_frame_index]->height;
							 y += 2) {
							auto* d = reinterpret_cast<uint32_t*>(
								output_avstream_video_frame[output_avstream_video_frame_index]->data[0] +
								(output_avstream_video_frame[output_avstream_video_frame_index]->linesize[0] * y));
							auto* s = reinterpret_cast<uint32_t*>(
								output_avstream_video_frame[output_avstream_video_frame_index]->data[0] +
								(output_avstream_video_frame[output_avstream_video_frame_index]->linesize[0] *
									(y + 1)));

							memcpy(d, s,
								sizeof(uint32_t) *
									output_avstream_video_frame[output_avstream_video_frame_index]->width);
						}
					}
				}*/

				// convert ARGB to whatever the codec demands, and encode
				output_avstream_video_encode_frame->pts =
					output_avstream_video_frame[output_avstream_video_frame_index]->pts;
				output_avstream_video_encode_frame->pkt_pts =
					output_avstream_video_frame[output_avstream_video_frame_index]->pkt_pts;
				output_avstream_video_encode_frame->pkt_dts =
					output_avstream_video_frame[output_avstream_video_frame_index]->pkt_dts;
				output_avstream_video_encode_frame->top_field_first =
					output_avstream_video_frame[output_avstream_video_frame_index]->top_field_first;
				output_avstream_video_encode_frame->interlaced_frame =
					output_avstream_video_frame[output_avstream_video_frame_index]->interlaced_frame;

				if (sws_scale(output_avstream_video_resampler,
						// source
						output_avstream_video_frame[output_avstream_video_frame_index]->data,
						output_avstream_video_frame[output_avstream_video_frame_index]->linesize, 0,
						output_avstream_video_frame[output_avstream_video_frame_index]->height,
						// dest
						output_avstream_video_encode_frame->data, output_avstream_video_encode_frame->linesize) <= 0) {
					fprintf(stderr, "WARNING: sws_scale failed\n");
				}

				assert(output_avstream_video_frame_index < output_avstream_video_frame.size());
				if ((++output_avstream_video_frame_index) >= output_avstream_video_frame_delay) {
					output_avstream_video_frame_index = 0;
				}

				EncoderChannel->push({output_avstream_video_encode_frame, current});
				current++;
			}
		} while (!eof);
		EncoderChannel->close();
		EncoderThread.join();
	}

	/* flush encoder delay */
	do {
		AVPacket pkt;
		int		 gotit = 0;

		av_init_packet(&pkt);
		if (av_new_packet(&pkt, 50000000 / 8) < 0) { break; }

		if (avcodec_encode_video2(output_avstream_video_codec_context, &pkt, nullptr, &gotit) == 0) {
			if (gotit != 0) {
				pkt.stream_index = output_avstream_video->index;
				av_packet_rescale_ts(
					&pkt, output_avstream_video_codec_context->time_base, output_avstream_video->time_base);

				if (av_interleaved_write_frame(output_avfmt, &pkt) < 0) {
					fprintf(stderr, "AV write frame failed video\n");
				}
			}
		}

		av_packet_unref(&pkt);
		if (gotit == 0) { break; }
	} while (1);

	/* close output */
	if (output_avstream_video_resampler != nullptr) {
		sws_freeContext(output_avstream_video_resampler);
		output_avstream_video_resampler = nullptr;
	}
	if (output_avstream_video_encode_frame != nullptr) { av_frame_free(&output_avstream_video_encode_frame); }
	while (!output_avstream_video_frame.empty()) {
		AVFrame* nf = output_avstream_video_frame.back();
		output_avstream_video_frame.pop_back();
		if (nf != nullptr) { av_frame_free(&nf); }
	}
	audio_hilopass.clear();
	av_write_trailer(output_avfmt);
	if (output_avfmt != nullptr && ((output_avfmt->oformat->flags & AVFMT_NOFILE) == 0)) {
		avio_closep(&output_avfmt->pb);
	}
	avformat_free_context(output_avfmt);

	/* close all */
	for (auto& input_file : input_files) { input_file.close_input(); }

	return 0;
}
