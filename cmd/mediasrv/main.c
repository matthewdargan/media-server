#include <dirent.h>
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* clang-format off */
#include "libu/u.h"
#include "libu/arena.h"
#include "libu/string.h"
#include "libu/os.h"
#include "libu/u.c"
#include "libu/arena.c"
#include "libu/string.c"
#include "libu/os.c"
/* clang-format on */

typedef struct S64Array S64Array;
struct S64Array {
	s64 *v;
	u64 cnt;
};

typedef struct VideoContext VideoContext;
struct VideoContext {
	String8 input_path;
	String8 output_path;
	AVFormatContext *input_context;
	AVStream *video_stream;
	S64Array keyframes;
};

/*
static b32
extract_keyframes(Arena *a, VideoContext *ctx, String8 path)
{
    ctx->input_path = push_str8_copy(a, path);
    ctx->output_path = push_str8_copy(a, str8_basename(str8_prefix_ext(path)));
    if (!os_dir_exists(ctx->output_path) && !os_mkdir(ctx->output_path)) {
        fprintf(stderr, "failed to create output directory: %s\n", ctx->output_path.str);
        return 0;
    }
    int err = avformat_open_input(&ctx->input_context, (char *)ctx->input_path.str, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "failed to open input file %s: %s\n", ctx->input_path.str, av_err2str(err));
        return 0;
    }
    err = avformat_find_stream_info(ctx->input_context, NULL);
    if (err < 0) {
        fprintf(stderr, "failed to find stream info: %s\n", av_err2str(err));
        avformat_close_input(&ctx->input_context);
        return 0;
    }
    int video_stream = av_find_best_stream(ctx->input_context, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream < 0) {
        fprintf(stderr, "failed to find video stream: %s\n", av_err2str(video_stream));
        avformat_close_input(&ctx->input_context);
        return 0;
    }
    ctx->video_stream = ctx->input_context->streams[video_stream];
    ctx->keyframes.v = push_array(a, s64, KB(1));
    ctx->keyframes.cnt = 0;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "failed to allocate packet for key frame extraction\n");
        avformat_close_input(&ctx->input_context);
        return 0;
    }
    while (av_read_frame(ctx->input_context, pkt) >= 0) {
        if (pkt->stream_index == video_stream && (pkt->flags & AV_PKT_FLAG_KEY)) {
            ctx->keyframes.v[ctx->keyframes.cnt] = pkt->pts;
            ctx->keyframes.cnt++;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    err = av_seek_frame(ctx->input_context, video_stream, 0, AVSEEK_FLAG_BACKWARD);
    if (err < 0) {
        fprintf(stderr, "failed to seek to start of input file: %s\n", av_err2str(err));
        av_packet_free(&pkt);
        avformat_close_input(&ctx->input_context);
        return 0;
    }
    return 1;
}

static b32
transmux_to_fmp4(String8 src, String8 dst)
{
    AVFormatContext *input_ctx = NULL;
    AVFormatContext *output_ctx = NULL;
    AVIOContext *output_io_ctx = NULL;
    AVPacket pkt;
    b32 success = 0;
    int err = avformat_open_input(&input_ctx, (char *)src.str, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "failed to open input file %s: %s\n", src.str, av_err2str(err));
        goto cleanup;
    }
    err = avformat_find_stream_info(input_ctx, NULL);
    if (err < 0) {
        fprintf(stderr, "failed to find stream info: %s\n", av_err2str(err));
        goto cleanup;
    }
    avformat_alloc_output_context2(&output_ctx, NULL, NULL, (char *)dst.str);
    if (!output_ctx) {
        fprintf(stderr, "failed to allocate output context\n");
        goto cleanup;
    }
    err = avio_open(&output_io_ctx, (char *)dst.str, AVIO_FLAG_WRITE);
    if (err < 0) {
        fprintf(stderr, "failed to open output file %s: %s\n", dst.str, av_err2str(err));
        goto cleanup;
    }
    output_ctx->pb = output_io_ctx;
    for (u32 i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *in_stream = input_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;
        if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO && in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }
        AVStream *out_stream = avformat_new_stream(output_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "failed to create output stream\n");
            goto cleanup;
        }
        err = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (err < 0) {
            fprintf(stderr, "failed to copy codec parameters: %s\n", av_err2str(err));
            goto cleanup;
        }
        out_stream->codecpar->codec_tag = 0;
    }
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov", 0);
    err = avformat_write_header(output_ctx, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        fprintf(stderr, "failed to write header: %s\n", av_err2str(err));
        goto cleanup;
    }
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    while (av_read_frame(input_ctx, &pkt) >= 0) {
        AVStream *in_stream = input_ctx->streams[pkt.stream_index];
        AVStream *out_stream = NULL;
        for (u32 i = 0; i < output_ctx->nb_streams; i++) {
            if (output_ctx->streams[i]->codecpar->codec_type == in_stream->codecpar->codec_type &&
                output_ctx->streams[i]->codecpar->codec_id == in_stream->codecpar->codec_id) {
                out_stream = output_ctx->streams[i];
                break;
            }
        }
        if (out_stream) {
            pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base,
                                       AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
            pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base,
                                       AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
            pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
            pkt.pos = -1;
            err = av_interleaved_write_frame(output_ctx, &pkt);
            if (err < 0) {
                fprintf(stderr, "failed to write packet: %s\n", av_err2str(err));
                av_packet_unref(&pkt);
                goto cleanup;
            }
        }
        av_packet_unref(&pkt);
    }
    err = av_write_trailer(output_ctx);
    if (err < 0) {
        fprintf(stderr, "failed to write trailer: %s\n", av_err2str(err));
        goto cleanup;
    }
    success = 1;
cleanup:
    if (input_ctx) {
        avformat_close_input(&input_ctx);
    }
    if (output_ctx && output_ctx->pb) {
        avio_closep(&output_ctx->pb);
    }
    if (output_ctx) {
        avformat_free_context(output_ctx);
    }
    return success;
}
*/

static b32
transmux_and_segment_for_dash(Arena *a, String8 input_path, String8 output_dir, String8 base_name,
                              f64 segment_duration_sec)
{
	AVFormatContext *input_ctx = NULL;
	AVFormatContext *output_ctx = NULL;
	AVPacket *pkt = av_packet_alloc();
	b32 success = 0;
	String8 segment_template =
	    push_str8f(a, "%.*s/%.*s_segment_%%04d.mp4", output_dir.len, output_dir.str, base_name.len, base_name.str);
	String8 mpd_path = push_str8f(a, "%.*s/%.*s.mpd", output_dir.len, output_dir.str, base_name.len, base_name.str);
	int err = avformat_open_input(&input_ctx, (char *)input_path.str, NULL, NULL);
	if (err < 0) {
		fprintf(stderr, "failed to open input file %s: %s\n", input_path.str, av_err2str(err));
		goto cleanup;
	}
	err = avformat_find_stream_info(input_ctx, NULL);
	if (err < 0) {
		fprintf(stderr, "failed to find stream info: %s\n", av_err2str(err));
		goto cleanup;
	}
	avformat_alloc_output_context2(&output_ctx, NULL, "segment", (char *)segment_template.str);
	if (!output_ctx) {
		fprintf(stderr, "failed to allocate output context for segment muxer\n");
		goto cleanup;
	}
	for (u64 i = 0; i < input_ctx->nb_streams; i++) {
		AVStream *in_stream = input_ctx->streams[i];
		AVCodecParameters *in_codecpar = in_stream->codecpar;
		if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO && in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
		    in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
			continue;
		}
		AVStream *out_stream = avformat_new_stream(output_ctx, NULL);
		if (!out_stream) {
			fprintf(stderr, "failed to create output stream\n");
			goto cleanup;
		}
		err = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
		if (err < 0) {
			fprintf(stderr, "failed to copy codec parameters: %s\n", av_err2str(err));
			goto cleanup;
		}
		/* Set codec tag to 0 (important for compatibility) */
		out_stream->codecpar->codec_tag = 0;
	}
	/* Set options for the segment muxer and fMP4 segments */
	AVDictionary *opts = NULL;
	/* format=mpd: Tell the segment muxer to output an MPD file */
	av_dict_set(&opts, "format", "mpd", 0);
	/* segment_format=mp4: Use MP4 format for the segments */
	av_dict_set(&opts, "segment_format", "mp4", 0);
	/*
	 * movflags for fMP4 segments
	 * frag_keyframe: Start a new fragment at each keyframe
	 * empty_moov: Write a fragmented MP4 that can be played while being written
	 * write_empty_moov: Write an empty moov box even if not fragmented (good practice for fMP4 init segment)
	 * default_base_moof: Use default-base-is-moof flag in traf boxes (common for DASH)
	 */
	av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+write_empty_moov+default_base_moof", 0);
	/* segment_time: Set segment duration in seconds */
	/* av_dict_set_double(&opts, "segment_time", segment_duration_sec, 0); */
	String8 segment_time = push_str8f(a, "%f", segment_duration_sec);
	av_dict_set(&opts, "segment_time", (char *)segment_time.str, 0);
	/* segment_list: Specify the name of the MPD file */
	av_dict_set(&opts, "segment_list", (char *)mpd_path.str, 0);
	/* segment_list_flags=live: Write MPD in a way suitable for dynamic updates (good for VOD too) */
	av_dict_set(&opts, "segment_list_flags", "live", 0);
	/* segment_individualheader: Write a header (moov box) in each segment (essential for fMP4 segments) */
	av_dict_set(&opts, "segment_individualheader", "1", 0);
	/* min_frag_duration: Minimum fragment duration in microseconds (optional, can help with small fragments) */
	/* av_dict_set_int(&opts, "min_frag_duration", 1000000, 0); */
	err = avformat_write_header(output_ctx, &opts);
	av_dict_free(&opts);
	if (err < 0) {
		fprintf(stderr, "failed to write header (MPD/Init Segment): %s\n", av_err2str(err));
		goto cleanup;
	}
	pkt->data = NULL;
	pkt->size = 0;
	while (av_read_frame(input_ctx, pkt) >= 0) {
		AVStream *in_stream = input_ctx->streams[pkt->stream_index];
		AVStream *out_stream = NULL;
		for (u32 i = 0; i < output_ctx->nb_streams; i++) {
			if (output_ctx->streams[i]->codecpar->codec_type == in_stream->codecpar->codec_type &&
			    output_ctx->streams[i]->codecpar->codec_id == in_stream->codecpar->codec_id) {
				out_stream = output_ctx->streams[i];
				break;
			}
		}
		if (out_stream) {
			/*
			 * Rescale timestamps from input timebase to output timebase
			 * The segment muxer handles the actual writing to files based on segment_time
			 */
			pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base,
			                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
			pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base,
			                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
			pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
			pkt->pos = -1; /* Let FFmpeg handle position */
			               /* Set the correct stream index for the output context */
			pkt->stream_index = out_stream->index;
			err = av_interleaved_write_frame(output_ctx, pkt);
			if (err < 0) {
				fprintf(stderr, "failed to write packet: %s\n", av_err2str(err));
				/* Continue processing other packets or break? Let's break on write error. */
				av_packet_unref(pkt);
				goto cleanup;
			}
		}
		av_packet_unref(pkt);
	}
	err = av_write_trailer(output_ctx);
	if (err < 0) {
		fprintf(stderr, "failed to write trailer: %s\n", av_err2str(err));
		goto cleanup;
	}
	success = 1;
cleanup:
	if (input_ctx) {
		avformat_close_input(&input_ctx);
	}
	/* Note: avio_closep is NOT called here, the segment muxer manages the files */
	if (output_ctx) {
		avformat_free_context(output_ctx);
	}
	return success;
}

int
main(void)
{
	sys_info.nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	sys_info.page_size = sysconf(_SC_PAGESIZE);
	sys_info.large_page_size = MB(2);
	arena = arena_alloc((ArenaParams){
	    .flags = arena_default_flags, .res_size = arena_default_res_size, .cmt_size = arena_default_cmt_size});
	Temp scratch = temp_begin(arena);
	av_log_set_level(AV_LOG_INFO);
	String8 input_path = str8_lit("testdata/Big_Buck_Bunny_1080_10s_30MB_h265.mp4");
	String8 output_path = str8_lit("output/Big_Buck_Bunny_1080_10s_30MB_h265_transmuxed.mp4");
	String8 output_dir = str8_lit("output");
	f64 segment_duration = 2.0;
	if (!os_dir_exists(output_dir) && !os_mkdir(output_dir)) {
		fprintf(stderr, "failed to create output directory: %s\n", output_dir.str);
		return 1;
	}
	printf("Transmuxing %s to %s...\n", input_path.str, output_path.str);
	if (transmux_and_segment_for_dash(scratch.a, input_path, output_dir, str8_lit("test"), segment_duration)) {
		printf("DASH segmentation successful.\n");
	} else {
		fprintf(stderr, "DASH segmentation failed.\n");
		return 1;
	}
	temp_end(scratch);
	return 0;
}
