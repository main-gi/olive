#include "exportthread.h"

#include "project/sequence.h"

#include "panels/panels.h"
#include "panels/timeline.h"
#include "panels/viewer.h"
#include "ui/viewerwidget.h"
#include "playback/playback.h"
#include "playback/audio.h"
#include "dialogs/exportdialog.h"

extern "C" {
	#include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/opt.h>
	#include <libswresample/swresample.h>
	#include <libswscale/swscale.h>
}

#include <QDebug>
#include <QApplication>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLPaintDevice>
#include <QPainter>

AVFormatContext* fmt_ctx = NULL;
AVStream* video_stream;
AVCodec* vcodec;
AVCodecContext* vcodec_ctx;
AVFrame* video_frame;
AVFrame* sws_frame;
SwsContext* sws_ctx = NULL;
AVStream* audio_stream;
AVCodec* acodec;
AVFrame* audio_frame;
AVFrame* swr_frame;
AVCodecContext* acodec_ctx;
AVPacket video_pkt;
AVPacket audio_pkt;
SwrContext* swr_ctx = NULL;
int aframe_bytes;
int ret;
char* c_filename;

ExportThread::ExportThread() : continueEncode(true) {
	surface.create();
}

bool ExportThread::encode(AVFormatContext* ofmt_ctx, AVCodecContext* codec_ctx, AVFrame* frame, AVPacket* packet, AVStream* stream) {
	ret = avcodec_send_frame(codec_ctx, frame);
	if (ret < 0) {
		qDebug() << "[ERROR] Failed to send frame to encoder." << ret;
		ed->export_error = "failed to send frame to encoder (" + QString::number(ret) + ")";
		return false;
	}

	while (ret >= 0) {
		ret = avcodec_receive_packet(codec_ctx, packet);
		if (ret == AVERROR(EAGAIN)) {
			return true;
		} else if (ret < 0) {
			if (ret != AVERROR_EOF) {
				qDebug() << "[ERROR] Failed to receive packet from encoder." << ret;
				ed->export_error = "failed to receive packet from encoder (" + QString::number(ret) + ")";
			}
			return false;
		}

		packet->stream_index = stream->index;
		av_interleaved_write_frame(ofmt_ctx, packet);
		av_packet_unref(packet);
	}
	return true;
}

bool ExportThread::setupVideo() {
	// if video is disabled, no setup necessary
	if (!video_enabled) return true;

	// find video encoder
	vcodec = avcodec_find_encoder((enum AVCodecID) video_codec);
	if (!vcodec) {
		qDebug() << "[ERROR] Could not find video encoder";
		ed->export_error = "could not video encoder for " + QString::number(video_codec);
		return false;
	}

	// create video stream
	video_stream = avformat_new_stream(fmt_ctx, vcodec);
	video_stream->id = 0;
	if (!video_stream) {
		qDebug() << "[ERROR] Could not allocate video stream";
		ed->export_error = "could not allocate video stream";
		return false;
	}

	// allocate context
	vcodec_ctx = video_stream->codec;
//	vcodec_ctx = avcodec_alloc_context3(vcodec);
	if (!vcodec_ctx) {
		qDebug() << "[ERROR] Could not allocate video encoding context";
		ed->export_error = "could not allocate video encoding context";
		return false;
	}

	// setup context
	vcodec_ctx->codec_id = static_cast<AVCodecID>(video_codec);
	vcodec_ctx->width = video_width;
	vcodec_ctx->height = video_height;
	vcodec_ctx->sample_aspect_ratio = av_d2q(video_width/video_height, INT_MAX);
	vcodec_ctx->pix_fmt = vcodec->pix_fmts[0]; // maybe be breakable code
	vcodec_ctx->framerate = av_d2q(video_frame_rate, INT_MAX);
	if (video_compression_type == COMPRESSION_TYPE_CBR) vcodec_ctx->bit_rate = video_bitrate * 1000000;
	vcodec_ctx->time_base = av_inv_q(vcodec_ctx->framerate);
	video_stream->time_base = vcodec_ctx->time_base;

	if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
		vcodec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	if (vcodec_ctx->codec_id == AV_CODEC_ID_H264) {
		/*char buffer[50];
		itoa(vcodec_ctx, buffer, 10);*/

		av_opt_set(vcodec_ctx->priv_data, "preset", "slow", AV_OPT_SEARCH_CHILDREN);

		switch (video_compression_type) {
		case COMPRESSION_TYPE_CFR:
			av_opt_set(vcodec_ctx->priv_data, "crf", QString::number(static_cast<int>(video_bitrate)).toLatin1(), AV_OPT_SEARCH_CHILDREN);
			break;
		}
	}

	ret = avcodec_open2(vcodec_ctx, vcodec, NULL);
	if (ret < 0) {
		qDebug() << "[ERROR] Could not open output video encoder." << ret;
		ed->export_error = "could not open output video encoder (" + QString::number(ret) + ")";
		return false;
	}

	// copy video encoder parameters to output stream
	ret = avcodec_parameters_from_context(video_stream->codecpar, vcodec_ctx);
	if (ret < 0) {
		qDebug() << "[ERROR] Could not copy video encoder parameters to output stream." << ret;
		ed->export_error = "could not copy video encoder parameters to output stream (" + QString::number(ret) + ")";
		return false;
	}

	// create AVFrame
	video_frame = av_frame_alloc();
	av_frame_make_writable(video_frame);
	video_frame->format = AV_PIX_FMT_RGBA;
	video_frame->width = sequence->width;
	video_frame->height = sequence->height;
	av_frame_get_buffer(video_frame, 0);

	av_init_packet(&video_pkt);

	sws_ctx = sws_getContext(
				sequence->width,
				sequence->height,
				AV_PIX_FMT_RGBA,
				video_width,
				video_height,
				vcodec_ctx->pix_fmt,
				SWS_FAST_BILINEAR,
				NULL,
				NULL,
				NULL
			);

	sws_frame = av_frame_alloc();
	sws_frame->format = vcodec_ctx->pix_fmt;
	sws_frame->width = video_width;
	sws_frame->height = video_height;
	av_frame_get_buffer(sws_frame, 0);

	return true;
}

bool ExportThread::setupAudio() {
	// if audio is disabled, no setup necessary
	if (!audio_enabled) return true;

	// find encoder
	acodec = avcodec_find_encoder(static_cast<AVCodecID>(audio_codec));
	if (!acodec) {
		qDebug() << "[ERROR] Could not find audio encoder";
		ed->export_error = "could not audio encoder for " + QString::number(audio_codec);
		return false;
	}

	// allocate audio stream
	audio_stream = avformat_new_stream(fmt_ctx, acodec);
	audio_stream->id = 1;
	if (!audio_stream) {
		qDebug() << "[ERROR] Could not allocate audio stream";
		ed->export_error = "could not allocate audio stream";
		return false;
	}

	// allocate context
	acodec_ctx = audio_stream->codec;
//	acodec_ctx = avcodec_alloc_context3(acodec);
	if (!acodec_ctx) {
		qDebug() << "[ERROR] Could not find allocate audio encoding context";
		ed->export_error = "could not allocate audio encoding context";
		return false;
	}

	// setup context
	acodec_ctx->codec_id = static_cast<AVCodecID>(audio_codec);
	acodec_ctx->sample_rate = audio_sampling_rate;
	acodec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;  // change this to support surround/mono sound in the future (this is what the user sets the output audio to)
	acodec_ctx->channels = av_get_channel_layout_nb_channels(acodec_ctx->channel_layout);
	acodec_ctx->sample_fmt = acodec->sample_fmts[0];
	acodec_ctx->bit_rate = audio_bitrate * 1000;

	acodec_ctx->time_base.num = 1;
	acodec_ctx->time_base.den = audio_sampling_rate;
	audio_stream->time_base = acodec_ctx->time_base;

	if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
		acodec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	// open encoder
	ret = avcodec_open2(acodec_ctx, acodec, NULL);
	if (ret < 0) {
		qDebug() << "[ERROR] Could not open output audio encoder." << ret;
		ed->export_error = "could not open output audio encoder (" + QString::number(ret) + ")";
		return false;
	}

	// copy params to output stream
	ret = avcodec_parameters_from_context(audio_stream->codecpar, acodec_ctx);
	if (ret < 0) {
		qDebug() << "[ERROR] Could not copy audio encoder parameters to output stream." << ret;
		ed->export_error = "could not copy audio encoder parameters to output stream (" + QString::number(ret) + ")";
		return false;
	}

	// init audio resampler context
	swr_ctx = swr_alloc_set_opts(
			NULL,
			acodec_ctx->channel_layout,
			acodec_ctx->sample_fmt,
			acodec_ctx->sample_rate,
			sequence->audio_layout,
			AV_SAMPLE_FMT_S16,
			sequence->audio_frequency,
			0,
			NULL
		);
	swr_init(swr_ctx);

	// initialize raw audio frame
	audio_frame = av_frame_alloc();
	audio_frame->sample_rate = sequence->audio_frequency;
	audio_frame->nb_samples = acodec_ctx->frame_size;
	if (audio_frame->nb_samples == 0) audio_frame->nb_samples = 2048; // should possibly be smaller?
	audio_frame->format = AV_SAMPLE_FMT_S16;
	audio_frame->channel_layout = AV_CH_LAYOUT_STEREO; // change this to support surround/mono sound in the future (this is whatever format they're held in the internal buffer)
	audio_frame->channels = av_get_channel_layout_nb_channels(audio_frame->channel_layout);
	av_frame_make_writable(audio_frame);
	ret = av_frame_get_buffer(audio_frame, 0);
	if (ret < 0) {
		qDebug() << "[ERROR] Could not allocate audio buffer." << ret;
		ed->export_error = "could not allocate audio buffer (" + QString::number(ret) + ")";
		return false;
	}
	aframe_bytes = av_samples_get_buffer_size(NULL, audio_frame->channels, audio_frame->nb_samples, static_cast<AVSampleFormat>(audio_frame->format), 0);

	// init converted audio frame
	swr_frame = av_frame_alloc();
	swr_frame->channel_layout = acodec_ctx->channel_layout;
	swr_frame->channels = acodec_ctx->channels;
	swr_frame->sample_rate = acodec_ctx->sample_rate;
	swr_frame->format = acodec_ctx->sample_fmt;
	av_frame_make_writable(swr_frame);

	av_init_packet(&audio_pkt);

	return true;
}

bool ExportThread::setupContainer() {
	avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, c_filename);
	if (!fmt_ctx) {
		qDebug() << "[ERROR] Could not create output context";
		ed->export_error = "could not create output format context";
		return false;
	}

//	av_dump_format(fmt_ctx, 0, c_filename, 1);

	ret = avio_open(&fmt_ctx->pb, c_filename, AVIO_FLAG_WRITE);
	if (ret < 0) {
		qDebug() << "[ERROR] Could not open output file." << ret;
		ed->export_error = "could not open output file (" + QString::number(ret) + ")";
		return false;
	}

	return true;
}

void ExportThread::run() {
	panel_timeline->pause();

	if (!panel_viewer->viewer_widget->context()->makeCurrent(&surface)) {
		qDebug() << "[ERROR] Make current failed";
		ed->export_error = "could not make OpenGL context current";
		return;
	}

	// copy filename
	QByteArray ba = filename.toLatin1();
	c_filename = new char[ba.size()+1];
	strcpy(c_filename, ba.data());

	continueEncode = setupContainer();

	if (continueEncode) continueEncode = setupVideo();

	if (continueEncode) continueEncode = setupAudio();

	if (continueEncode) {
		ret = avformat_write_header(fmt_ctx, NULL);
		if (ret < 0) {
			qDebug() << "[ERROR] Could not write output file header." << ret;
			ed->export_error = "could not write output file header (" + QString::number(ret) + ")";
			continueEncode = false;
		}
	}

	panel_timeline->seek(start_frame);

	QOpenGLFramebufferObject fbo(sequence->width, sequence->height, QOpenGLFramebufferObject::CombinedDepthStencil, GL_TEXTURE_RECTANGLE);
	fbo.bind();

	panel_viewer->viewer_widget->rendering = true;
	panel_viewer->viewer_widget->default_fbo = &fbo;

	long file_audio_samples = 0;

	while (panel_timeline->playhead < end_frame && continueEncode) {
		panel_viewer->viewer_widget->paintGL();

		double timecode_secs = (double) (panel_timeline->playhead-start_frame) / sequence->frame_rate;
		if (video_enabled) {
			// get image from opengl
			glReadPixels(0, 0, video_frame->linesize[0]/4, sequence->height, GL_RGBA, GL_UNSIGNED_BYTE, video_frame->data[0]);

			// change pixel format
			sws_scale(sws_ctx, video_frame->data, video_frame->linesize, 0, video_frame->height, sws_frame->data, sws_frame->linesize);
			sws_frame->pts = round(timecode_secs/av_q2d(video_stream->time_base));

			// send to encoder
			if (!encode(fmt_ctx, vcodec_ctx, sws_frame, &video_pkt, video_stream)) continueEncode = false;
		}
		if (audio_enabled) {
			// do we need to encode more audio samples?
			while (continueEncode && file_audio_samples <= (timecode_secs*audio_sampling_rate)) {
				int adjusted_read = audio_ibuffer_read%audio_ibuffer_size;
				int copylen = qMin(aframe_bytes, audio_ibuffer_size-adjusted_read);
				memcpy(audio_frame->data[0], audio_ibuffer+adjusted_read, copylen);
				memset(audio_ibuffer+adjusted_read, 0, copylen);
				audio_ibuffer_read += copylen;

				if (copylen < aframe_bytes) {
					// copy remainder
					int remainder_len = aframe_bytes-copylen;
					memcpy(audio_frame->data[0]+copylen, audio_ibuffer, remainder_len);
					memset(audio_ibuffer, 0, remainder_len);
					audio_ibuffer_read += remainder_len;
				}

				// convert to export sample format
				swr_convert_frame(swr_ctx, swr_frame, audio_frame);
				swr_frame->pts = file_audio_samples;

				// send to encoder
				if (!encode(fmt_ctx, acodec_ctx, swr_frame, &audio_pkt, audio_stream)) continueEncode = false;

				file_audio_samples += swr_frame->nb_samples;
			}
		}
		emit progress_changed(((double) (panel_timeline->playhead-start_frame) / (double) (end_frame-start_frame)) * 100);
		panel_timeline->playhead++;
	}

	panel_viewer->viewer_widget->default_fbo = NULL;
	panel_viewer->viewer_widget->rendering = false;

	fbo.release();

	if (audio_enabled) {
		// flush swresample
		do {
			swr_convert_frame(swr_ctx, swr_frame, NULL);
			if (swr_frame->nb_samples == 0) break;
			swr_frame->pts = file_audio_samples;
			if (!encode(fmt_ctx, acodec_ctx, swr_frame, &audio_pkt, audio_stream)) continueEncode = false;
			file_audio_samples += swr_frame->nb_samples;
		} while (swr_frame->nb_samples > 0);
	}

	bool continueVideo = true;
	bool continueAudio = true;
	if (continueEncode) {
		// flush remaining packets
		while (continueVideo && continueAudio) {
			if (continueVideo) continueVideo = encode(fmt_ctx, vcodec_ctx, NULL, &video_pkt, video_stream);
			if (continueAudio) continueAudio = encode(fmt_ctx, acodec_ctx, NULL, &audio_pkt, audio_stream);
		}

		ret = av_write_trailer(fmt_ctx);
		if (ret < 0) {
			qDebug() << "[ERROR] Could not write output file trailer." << ret;
			ed->export_error = "could not write output file trailer (" + QString::number(ret) + ")";
			continueEncode = false;
		}

		emit progress_changed(100);
	}

	avio_closep(&fmt_ctx->pb);

	if (video_enabled) {
		avcodec_close(vcodec_ctx);
		av_packet_unref(&video_pkt);
		av_frame_free(&video_frame);
//		avcodec_free_context(&vcodec_ctx);
	}

	if (audio_enabled) {
		avcodec_close(acodec_ctx);
		av_packet_unref(&audio_pkt);
		av_frame_free(&audio_frame);
//		avcodec_free_context(&acodec_ctx);
	}

	avformat_free_context(fmt_ctx);

	if (sws_ctx != NULL) {
		sws_freeContext(sws_ctx);
		av_frame_free(&sws_frame);
	}
	if (swr_ctx != NULL) {
		swr_free(&swr_ctx);
		av_frame_free(&swr_frame);
	}

	delete [] c_filename;

	panel_viewer->viewer_widget->context()->doneCurrent();
	panel_viewer->viewer_widget->context()->moveToThread(qApp->thread());
}
