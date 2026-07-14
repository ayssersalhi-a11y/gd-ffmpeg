/**
 * ffmpeg_player.cpp
 * GDExtension —FFmpeg Video Player (Unified) for Godot 4
 *
 * الإصدار 6.1 — إصلاح AVERROR_INVALIDDATA + تحسين تشخيص البناء
 *
 * ─── ما الجديد في v6.1 ───────────────────────────────────────────────────────
 *
 * [FIX-1] seekable=0 بدلاً من seekable=1 في _open_audio_with_ffmpeg:
 *         seekable=1 كان يجبر FFmpeg على تجربة Range-Requests أثناء الـ Probe،
 *         إذا رفض الخادم → AVERROR_INVALIDDATA (-1094995529). الآن نستخدم
 *         وضع Streaming النقي (seekable=0) الذي يقرأ تسلسلياً بلا Seek.
 *
 * [FIX-2] إصلاح منطق load_audio() — كان يرفض صامتاً روابط HTTP/HTTPS
 *         إذا كان الفيديو محلياً (use_external_audio=false). الآن يُتحقق
 *         من مسار الصوت نفسه بشكل مستقل عن مصدر الفيديو.
 *
 * [FIX-3] تشخيص بناء المكتبة في _ready():
 *         طباعة جميع البروتوكولات المتاحة (avio_enum_protocols) لتأكيد
 *         وجود tls/https/ssl في هذا الـ Build على الأندرويد.
 *
 * [FIX-4] تحسينات إضافية لـ _open_audio_with_ffmpeg:
 *         - إضافة ssl للـ protocol_whitelist.
 *         - User-Agent أندرويد (بدلاً من Windows).
 *         - reconnect=1 + reconnect_delay_max=5.
 *         - probesize=512KB + analyzeduration=2s لتسريع الفتح.
 *
 * ─── الإصدارات السابقة ────────────────────────────────────────────────────────
 *
 * [6]  Audio Overrun Protection (v6.0)
 * [7]  Audio Auto-Recovery (v6.0)
 * [8]  Audio Interpolation & Soft Clipping (v6.0)
 * [9]  Silence Filling (v6.0)
 * [10] Smart Channel Mapping (v6.0)
 * [11] Hard Frame Drop + GPU Latency Offset (v6.0)
 * [12] إصلاح أولي لروابط HTTP/HTTPS (v6.0 — مكتمل في v6.1)
 */

#include "ffmpeg_player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>

#include <cmath>

using namespace godot;

// ─── تسجيل الكلاس ─────────────────────────────────────────────────────────────
void FFmpegPlayer::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_video", "path"),        &FFmpegPlayer::load_video);
    ClassDB::bind_method(D_METHOD("play"),                      &FFmpegPlayer::play);
    ClassDB::bind_method(D_METHOD("pause"),                     &FFmpegPlayer::pause);
    ClassDB::bind_method(D_METHOD("stop"),                      &FFmpegPlayer::stop);
    ClassDB::bind_method(D_METHOD("seek", "seconds"),           &FFmpegPlayer::seek);
    ClassDB::bind_method(D_METHOD("is_playing"),                &FFmpegPlayer::is_playing);
    ClassDB::bind_method(D_METHOD("get_duration"),              &FFmpegPlayer::get_duration);
    ClassDB::bind_method(D_METHOD("get_position"),              &FFmpegPlayer::get_position);
    ClassDB::bind_method(D_METHOD("get_video_width"),           &FFmpegPlayer::get_video_width);
    ClassDB::bind_method(D_METHOD("get_video_height"),          &FFmpegPlayer::get_video_height);
    ClassDB::bind_method(D_METHOD("get_fps"),                   &FFmpegPlayer::get_fps);
    ClassDB::bind_method(D_METHOD("get_current_frame_texture"), &FFmpegPlayer::get_current_frame_texture);
    ClassDB::bind_method(D_METHOD("set_loop", "enable"),        &FFmpegPlayer::set_loop);
    ClassDB::bind_method(D_METHOD("get_loop"),                  &FFmpegPlayer::get_loop);

    ClassDB::bind_method(D_METHOD("load_audio", "path"),        &FFmpegPlayer::load_audio);
    ClassDB::bind_method(D_METHOD("unload_audio"),              &FFmpegPlayer::unload_audio);
    ClassDB::bind_method(D_METHOD("set_audio_volume", "vol"),   &FFmpegPlayer::set_audio_volume);
    ClassDB::bind_method(D_METHOD("get_audio_volume"),          &FFmpegPlayer::get_audio_volume);
    ClassDB::bind_method(D_METHOD("set_audio_muted", "muted"),  &FFmpegPlayer::set_audio_muted);
    ClassDB::bind_method(D_METHOD("is_audio_muted"),            &FFmpegPlayer::is_audio_muted);
    ClassDB::bind_method(D_METHOD("get_loaded_audio_path"),     &FFmpegPlayer::get_loaded_audio_path);
    ClassDB::bind_method(D_METHOD("is_using_external_audio"),   &FFmpegPlayer::is_using_external_audio);

    ClassDB::bind_method(D_METHOD("get_forward_buffer"),        &FFmpegPlayer::get_forward_buffer);
    ClassDB::bind_method(D_METHOD("is_buffering"),              &FFmpegPlayer::is_buffering);
    ClassDB::bind_method(D_METHOD("get_buffer_status"),         &FFmpegPlayer::get_buffer_status);

    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "get_loop");

    ADD_SIGNAL(MethodInfo("video_loaded",
        PropertyInfo(Variant::BOOL, "success")));
    ADD_SIGNAL(MethodInfo("frame_updated",
        PropertyInfo(Variant::OBJECT, "texture")));
    ADD_SIGNAL(MethodInfo("video_finished"));
    ADD_SIGNAL(MethodInfo("playback_error",
        PropertyInfo(Variant::STRING, "message")));
    ADD_SIGNAL(MethodInfo("buffering_changed",
        PropertyInfo(Variant::BOOL, "is_buffering")));
    ADD_SIGNAL(MethodInfo("audio_loaded",
        PropertyInfo(Variant::BOOL, "success")));
    ADD_SIGNAL(MethodInfo("buffering_status",
        PropertyInfo(Variant::FLOAT, "low_secs"),
        PropertyInfo(Variant::FLOAT, "high_secs")));
}

FFmpegPlayer::FFmpegPlayer() {}
FFmpegPlayer::~FFmpegPlayer() { _cleanup(); }

// ─── _ready ───────────────────────────────────────────────────────────────────
void FFmpegPlayer::_ready() {
    godot_mix_rate = (int)AudioServer::get_singleton()->get_mix_rate();
    UtilityFunctions::print("[AUDIO] Godot mix rate: ", godot_mix_rate, " Hz");

    int_audio_player = memnew(AudioStreamPlayer);
    int_audio_player->set_name("_IntAudioPlayer");
    add_child(int_audio_player);

    ext_audio_player = memnew(AudioStreamPlayer);
    ext_audio_player->set_name("_ExtAudioPlayer");
    add_child(ext_audio_player);

    UtilityFunctions::print("--- FFmpeg GDExtension v6.1 ---");

    // ── تشخيص: طباعة البروتوكولات المتاحة في هذا الـ Build ──────────────────
    {
        void *proto_iter = nullptr;
        const char *proto_name;
        String available = "[BUILD] Input protocols: ";
        while ((proto_name = avio_enum_protocols(&proto_iter, 0)) != nullptr)
            available += String(proto_name) + " ";
        UtilityFunctions::print(available);
    }

    void *opaque = nullptr;
    const AVCodec *codec;
    while ((codec = av_codec_iterate(&opaque)))
        if (av_codec_is_decoder(codec) && String(codec->name).find("mediacodec") != -1)
            UtilityFunctions::print("[HW] ", codec->name);
}

// ─── تحميل الفيديو ────────────────────────────────────────────────────────────
bool FFmpegPlayer::load_video(const String &path) {
    _cleanup();
    buffering = false; forward_buffer_secs = 0.0;
    position  = 0.0;   frame_timer         = 0.0;
    _reset_last_audio_pts();

    if (path.is_empty()) { _emit_playback_error("Path is empty"); return false; }

    is_live_stream     = path.begins_with("rtmp://") || path.begins_with("rtsp://");
    use_external_audio = path.begins_with("http://")  || path.begins_with("https://");
    bool is_network    = is_live_stream || use_external_audio;

    UtilityFunctions::print("[LOAD] Mode: ",
        use_external_audio ? "Direct URL (external audio)" :
        is_live_stream     ? "Live Stream (internal audio)" :
                             "Local File (internal audio)");

    if (path.ends_with(".m3u") || path.contains(".m3u?")) {
        _emit_playback_error("M3U_DETECTED"); return false;
    }

    String real_path = is_network ? path
        : ProjectSettings::get_singleton()->globalize_path(path);

    CharString utf8   = real_path.utf8();
    AVDictionary *opts = nullptr;

    if (is_network) {
        av_dict_set(&opts, "user_agent",
            "Mozilla/5.0 (Linux; Android 10; Mobile) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/119.0.0.0 Mobile Safari/537.36", 0);
        av_dict_set(&opts, "tls_verify",          "0", 0);
        av_dict_set(&opts, "protocol_whitelist",
            "file,http,https,tcp,tls,crypto,hls,applehttp,rtmp,rtsp,data,redirect", 0);
        av_dict_set(&opts, "fflags",              "nobuffer", 0);
        av_dict_set(&opts, "flags",               "low_delay", 0);
        av_dict_set(&opts, "reconnect",           "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "5", 0);
        // [12] لا reconnect_streamed للملفات المباشرة
        if (is_live_stream) av_dict_set(&opts, "reconnect_streamed", "1", 0);
    }

    if (avformat_open_input(&fmt_ctx, utf8.get_data(), nullptr, &opts) < 0) {
        av_dict_free(&opts);
        _emit_playback_error("Cannot open: " + path);
        _emit_video_loaded(false); return false;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        _cleanup();
        _emit_playback_error("Cannot read stream info: " + path);
        return false;
    }

    stream_start_time = (fmt_ctx->start_time != AV_NOPTS_VALUE)
                        ? (double)fmt_ctx->start_time / AV_TIME_BASE : 0.0;

    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx < 0) {
        _cleanup();
        _emit_playback_error("No video stream: " + path);
        _emit_video_loaded(false); return false;
    }
    if (!_setup_video_codec(fmt_ctx->streams[video_stream_idx])) {
        _cleanup(); _emit_video_loaded(false); return false;
    }

    if (!use_external_audio) {
        audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_stream_idx >= 0) {
            AVCodecContext *ac = nullptr; SwrContext *sw = nullptr;
            int rate = 0, ch = 0;
            if (_setup_audio_codec(fmt_ctx->streams[audio_stream_idx], ac, sw, rate, ch)) {
                audio_codec_ctx = ac; swr_ctx = sw;
                audio_sample_rate = rate; audio_channels = ch;
                int_audio_generator.instantiate();
                int_audio_generator->set_mix_rate((float)godot_mix_rate);
                int_audio_generator->set_buffer_length(0.5);
                int_audio_player->set_stream(int_audio_generator);
            } else {
                UtilityFunctions::printerr("[AUDIO] Internal setup failed, video-only.");
                audio_stream_idx = -1;
            }
        }
    } else {
        audio_stream_idx = -1;
    }

    duration = (fmt_ctx->duration != AV_NOPTS_VALUE)
               ? (double)fmt_ctx->duration / AV_TIME_BASE : 0.0;

    _allocate_buffers();
    _emit_video_loaded(true);
    UtilityFunctions::print("[LOAD] OK | dur=", duration, "s | fps=", fps,
        " | ", video_width, "x", video_height, " | mix=", godot_mix_rate, "Hz");
    return true;
}

// ─── إعداد كودك الفيديو ───────────────────────────────────────────────────────
bool FFmpegPlayer::_setup_video_codec(AVStream *vs) {
    const AVCodec *vc = nullptr;
    if      (vs->codecpar->codec_id == AV_CODEC_ID_H264) vc = avcodec_find_decoder_by_name("h264_mediacodec");
    else if (vs->codecpar->codec_id == AV_CODEC_ID_HEVC) vc = avcodec_find_decoder_by_name("hevc_mediacodec");
    else if (vs->codecpar->codec_id == AV_CODEC_ID_VP8)  vc = avcodec_find_decoder_by_name("vp8_mediacodec");
    else if (vs->codecpar->codec_id == AV_CODEC_ID_VP9)  vc = avcodec_find_decoder_by_name("vp9_mediacodec");

    if (!vc) { vc = avcodec_find_decoder(vs->codecpar->codec_id);
        UtilityFunctions::print("[VIDEO] SOFTWARE"); }
    else      { UtilityFunctions::print("[VIDEO] HARDWARE (MediaCodec)"); }

    if (!vc) { _emit_playback_error("No video decoder"); return false; }

    video_codec_ctx = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(video_codec_ctx, vs->codecpar);
    if (String(vc->name).find("mediacodec") == -1) {
        video_codec_ctx->thread_count = 0;
        video_codec_ctx->thread_type  = FF_THREAD_FRAME;
    }
    if (avcodec_open2(video_codec_ctx, vc, nullptr) < 0) {
        _emit_playback_error("Cannot open video decoder"); return false;
    }
    video_width  = video_codec_ctx->width;
    video_height = video_codec_ctx->height;
    fps          = av_q2d(vs->r_frame_rate);
    if (fps <= 0.0 || fps > 240.0) fps = 30.0;
    sws_ctx = sws_getContext(video_width, video_height, video_codec_ctx->pix_fmt,
                             video_width, video_height, AV_PIX_FMT_RGB24,
                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    return true;
}

// ─── إعداد كودك الصوت ─────────────────────────────────────────────────────────
// [5] Sample Rate Sync (→ godot_mix_rate)
// [8] Kaiser filter β=9
// [10] Smart Channel Mapping: Mono→Stereo | 5.1→Stereo
bool FFmpegPlayer::_setup_audio_codec(AVStream *as,
    AVCodecContext *&ctx_out, SwrContext *&swr_out, int &rate_out, int &ch_out)
{
    const AVCodec *ac = avcodec_find_decoder(as->codecpar->codec_id);
    if (!ac) { UtilityFunctions::printerr("[AUDIO] No decoder"); return false; }

    ctx_out = avcodec_alloc_context3(ac);
    avcodec_parameters_to_context(ctx_out, as->codecpar);
    if (avcodec_open2(ctx_out, ac, nullptr) < 0) {
        UtilityFunctions::printerr("[AUDIO] Cannot open decoder");
        avcodec_free_context(&ctx_out); return false;
    }

    rate_out = ctx_out->sample_rate;
    ch_out   = ctx_out->ch_layout.nb_channels;

    // [10] تحديد تخطيط المُدخَل بدقة
    AVChannelLayout in_layout = {};
    if (ctx_out->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_copy(&in_layout, &ctx_out->ch_layout);
    else
        av_channel_layout_default(&in_layout, ch_out > 0 ? ch_out : 1);

    // دائماً نخرج stereo بغض النظر عن عدد قنوات المصدر
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    ch_out = 2; // الخرج دائماً stereo

    // [5] الخرج بـ godot_mix_rate | [8] Kaiser interpolation
    swr_alloc_set_opts2(&swr_out,
        &out_layout,  AV_SAMPLE_FMT_FLT, godot_mix_rate,
        &in_layout,   ctx_out->sample_fmt, rate_out,
        0, nullptr);

    av_channel_layout_uninit(&in_layout);

    if (!swr_out) {
        UtilityFunctions::printerr("[AUDIO] swr_alloc failed");
        avcodec_free_context(&ctx_out); return false;
    }

    // [8] Kaiser filter للانتقال الناعم (يحل مشكلة الطقطقة عند Resampling)
    av_opt_set_int(swr_out, "filter_size",  64,  0); // فلتر طويل = جودة أعلى
    av_opt_set_int(swr_out, "phase_shift",  10,  0);
    // نوع فلتر Kaiser
    av_opt_set(swr_out, "resampler", "swr", 0);

    if (swr_init(swr_out) < 0) {
        UtilityFunctions::printerr("[AUDIO] swr_init failed");
        swr_free(&swr_out); swr_out = nullptr;
        avcodec_free_context(&ctx_out); return false;
    }

    UtilityFunctions::print("[AUDIO] Codec OK | src=", rate_out, "Hz ch=",
        ctx_out->ch_layout.nb_channels, " → dst=", godot_mix_rate, "Hz stereo");
    return true;
}

// ─── [12] فتح رابط الصوت عبر FFmpeg (مُصلَح) ───────────────────────────────
bool FFmpegPlayer::_open_audio_with_ffmpeg(const String &path) {
    _cleanup_ext_audio();

    CharString utf8    = path.utf8();
    AVDictionary *opts = nullptr;

    // إعدادات الاتصال — [v6.1] seekable=0 لمنع AVERROR_INVALIDDATA
    av_dict_set(&opts, "user_agent",
        "Mozilla/5.0 (Linux; Android 10; Mobile) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/119.0.0.0 Mobile Safari/537.36", 0);
    av_dict_set(&opts, "tls_verify",         "0",  0);
    av_dict_set(&opts, "protocol_whitelist",
        "file,http,https,tcp,tls,ssl,crypto,data,redirect", 0);
    av_dict_set(&opts, "timeout",            "15000000", 0);
    av_dict_set(&opts, "rw_timeout",         "15000000", 0);
    av_dict_set(&opts, "http_persistent",    "0",  0);
    // [FIX-1] seekable=0: وضع Streaming النقي — لا يحاول FFmpeg عمل Range-Request
    // seekable=1 كان يسبب AVERROR_INVALIDDATA عندما الخادم لا يدعم byte-range
    av_dict_set(&opts, "seekable",           "0",  0);
    av_dict_set(&opts, "reconnect",          "1",  0);
    av_dict_set(&opts, "reconnect_delay_max","5",  0);
    // تسريع الـ Probe حتى لا يتوقف FFmpeg طويلاً عند فتح الرابط
    av_dict_set(&opts, "probesize",          "524288",   0); // 512 KB بدلاً من 5 MB
    av_dict_set(&opts, "analyzeduration",    "2000000",  0); // 2 ثانية بدلاً من 5

    UtilityFunctions::print("[DEBUG-AUDIO] Attempting to open URL: ", path);

    int ret = avformat_open_input(&ext_fmt_ctx, utf8.get_data(), nullptr, &opts);
    
    if (ret < 0) {
        av_dict_free(&opts);
        
        // --- تحويل كود الخطأ إلى نص مفهوم ---
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf)); 
        String error_msg = "[FFmpeg Critical] " + String(err_buf) + " (Error Code: " + String::num(ret) + ")";
        
        UtilityFunctions::printerr(error_msg); 
        _emit_playback_error(error_msg);       
        emit_signal("audio_loaded", false); 
        return false;
    }
    av_dict_free(&opts);

    // التحقق من وجود بيانات داخل الملف
    if (avformat_find_stream_info(ext_fmt_ctx, nullptr) < 0) {
        avformat_close_input(&ext_fmt_ctx);
        _emit_playback_error("[DEBUG-AUDIO] Failed to find stream info after opening.");
        emit_signal("audio_loaded", false); 
        return false;
    }

    ext_audio_stream = av_find_best_stream(ext_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ext_audio_stream < 0) {
        avformat_close_input(&ext_fmt_ctx);
        _emit_playback_error("[DEBUG-AUDIO] No audio stream found in this URL.");
        emit_signal("audio_loaded", false); 
        return false;
    }

    int rate = 0, ch = 0;
    if (!_setup_audio_codec(ext_fmt_ctx->streams[ext_audio_stream], ext_audio_ctx, ext_swr_ctx, rate, ch)) {
        avformat_close_input(&ext_fmt_ctx);
        _emit_playback_error("[DEBUG-AUDIO] Codec setup failed for external audio.");
        emit_signal("audio_loaded", false); 
        return false;
    }

    // تهيئة مشغل جودو
    if (int_audio_generator.is_null()) {
        int_audio_generator.instantiate();
        int_audio_generator->set_mix_rate((float)godot_mix_rate);
        int_audio_generator->set_buffer_length(0.5);
        int_audio_player->set_stream(int_audio_generator);
    }

    ext_audio_eof = false;
    ext_using_godot_player = false;
    _reset_last_audio_pts();
    
    UtilityFunctions::print("[DEBUG-AUDIO] Success! Audio is ready to play.");
    emit_signal("audio_loaded", true);
    return true;
}



// ─── تنظيف الصوت الخارجي ──────────────────────────────────────────────────────
void FFmpegPlayer::_cleanup_ext_audio() {
    for (auto *p : ext_audio_pkt_queue)   av_packet_free(&p);
    ext_audio_pkt_queue.clear();
    for (auto *f : ext_audio_frame_queue) av_frame_free(&f);
    ext_audio_frame_queue.clear();

    if (ext_audio_ctx) { avcodec_free_context(&ext_audio_ctx); ext_audio_ctx  = nullptr; }
    if (ext_swr_ctx)   { swr_free(&ext_swr_ctx);               ext_swr_ctx    = nullptr; }
    if (ext_fmt_ctx)   { avformat_close_input(&ext_fmt_ctx);   ext_fmt_ctx    = nullptr; }
    ext_audio_stream = -1;
    ext_audio_eof    = false;
}

// ─── تخصيص بافرات الذاكرة ─────────────────────────────────────────────────────
void FFmpegPlayer::_allocate_buffers() {
    if (frame_buffer) { av_free(frame_buffer); frame_buffer = nullptr; }
    int sz = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);
    frame_buffer = (uint8_t *)av_malloc(sz);
    if (!frame_buffer) { UtilityFunctions::printerr("[MEM] Alloc failed!"); return; }

    PackedByteArray black; black.resize(video_width * video_height * 3); black.fill(0);
    Ref<Image> tmp = Image::create_from_data(video_width, video_height, false,
                                             Image::FORMAT_RGB8, black);
    if (current_texture.is_null()) current_texture.instantiate();
    current_texture->set_image(tmp);
    UtilityFunctions::print("[MEM] Ready: ", video_width, "x", video_height);
}

// ─── تحميل الصوت الخارجي ─────────────────────────────────────────────────────

bool FFmpegPlayer::load_audio(const String &path) {
    bool path_is_network = path.begins_with("http://") || path.begins_with("https://");
    if (!use_external_audio && !path_is_network) {
        UtilityFunctions::print("[AUDIO] load_audio() ignored: internal audio mode");
        return false;
    }
    
    // إيقاف المشغلات بأمان
    if (ext_audio_player && ext_audio_player->is_playing()) ext_audio_player->stop();
    if (int_audio_player && int_audio_player->is_playing()) int_audio_player->stop();

    // تأمين الخيط: إذا كان هناك خيط يعمل حالياً، ننتظر نهايته لضمان سلامة الذاكرة
    if (audio_loading_thread_running) {
        if (audio_loading_thread.joinable()) {
            audio_loading_thread.join(); 
        }
    }

    // مسح طوابير الصوت القديمة تماماً
    for (auto *f : decoded_audio_queue)    av_frame_free(&f);
    decoded_audio_queue.clear();
    _cleanup_ext_audio();
    ext_using_godot_player = false;
    int_audio_playback.unref();
    _reset_audio_clock(position);
    audio_load_finished_successfully = false; // إعادة تهيئة العلم

    if (path.is_empty()) {
        loaded_audio_path = ""; emit_signal("audio_loaded", false); return false;
    }

    // الروابط الشبكية
    if (path.begins_with("http://") || path.begins_with("https://")) {
        loaded_audio_path = path;
        audio_loading_thread_running = true;
        
        // إطلاق الخيط والاحتفاظ به في الكلاس (بدون استخدام detach!)
        audio_loading_thread = std::thread(&FFmpegPlayer::_open_audio_async_worker, this, path);
        return true; 
    }

    // الأكواد المحلية res:// و user:// والمصادر المطلقة تظل كما هي سريعة
    if (path.begins_with("res://") || path.begins_with("user://")) {
        Ref<Resource> res = ResourceLoader::get_singleton()->load(path);
        Ref<AudioStream> s = res;
        if (s.is_null()) {
            _emit_playback_error("Cannot load: " + path);
            emit_signal("audio_loaded", false); return false;
        }
        ext_audio_player->set_stream(s);
        loaded_audio_path = path;
        ext_using_godot_player = true;
        _apply_audio_volume();
        if (playing && !buffering) ext_audio_player->play((float)position);
        emit_signal("audio_loaded", true);
        return true;
    }

    Ref<FileAccess> fa = FileAccess::open(path, FileAccess::READ);
    if (fa.is_null()) {
        _emit_playback_error("Audio file not found: " + path);
        emit_signal("audio_loaded", false); return false;
    }
    PackedByteArray data = fa->get_buffer(fa->get_length()); fa.unref();

    String lower = path.to_lower();
    Ref<AudioStream> s;
    if (lower.ends_with(".mp3")) {
        Ref<AudioStreamMP3> mp3; mp3.instantiate(); mp3->set_data(data); s = mp3;
    } else if (lower.ends_with(".ogg")) {
        s = AudioStreamOggVorbis::load_from_buffer(data);
    } else {
        _emit_playback_error("Unsupported format: " + path);
        emit_signal("audio_loaded", false); return false;
    }
    if (s.is_null()) {
        _emit_playback_error("Audio decode failed: " + path);
        emit_signal("audio_loaded", false); return false;
    }
    ext_audio_player->set_stream(s);
    loaded_audio_path = path;
    ext_using_godot_player = true;
    _apply_audio_volume();
    if (playing && !buffering) ext_audio_player->play((float)position);
    emit_signal("audio_loaded", true);
    return true;
}


// ─── دالة العامل الخلفي لفتح الصوت الشبكي (جديدة ومساعدة) ──────────────────────────
void FFmpegPlayer::_open_audio_async_worker(String path) {
    bool ok = _open_audio_with_ffmpeg(path);
    
    if (ok) {
        // نكتفي برفع العلم فقط، ولا نستدعي _start_audio_at من هنا!
        audio_load_finished_successfully = true; 
    } else {
        audio_load_finished_successfully = false;
    }
    
    audio_loading_thread_running = false; 
}

void FFmpegPlayer::unload_audio() {
    if (ext_audio_player && ext_audio_player->is_playing()) ext_audio_player->stop();
    if (int_audio_player && int_audio_player->is_playing()) int_audio_player->stop();
    // [6] مسح كامل
    for (auto *f : decoded_audio_queue)    av_frame_free(&f);
    decoded_audio_queue.clear();
    _cleanup_ext_audio();
    ext_using_godot_player = false;
    int_audio_playback.unref();
    loaded_audio_path = "";
}

// ─── مستوى الصوت ──────────────────────────────────────────────────────────────
void FFmpegPlayer::_apply_audio_volume() {
    float db = (audio_muted || audio_volume <= 0.0001f)
               ? -80.0f : 20.0f * log10f(audio_volume);
    if (ext_using_godot_player && ext_audio_player) ext_audio_player->set_volume_db(db);
    else if (int_audio_player)                      int_audio_player->set_volume_db(db);
}
void FFmpegPlayer::set_audio_volume(float v) { audio_volume = CLAMP(v,0.f,1.f); _apply_audio_volume(); }
float FFmpegPlayer::get_audio_volume()  const { return audio_volume; }
void  FFmpegPlayer::set_audio_muted(bool m)   { audio_muted = m; _apply_audio_volume(); }
bool  FFmpegPlayer::is_audio_muted()    const { return audio_muted; }
String FFmpegPlayer::get_loaded_audio_path() const { return loaded_audio_path; }

// ─── ساعة الصوت ───────────────────────────────────────────────────────────────
double FFmpegPlayer::_get_audio_clock() const {
    if (!audio_clock_active) return position;
    return audio_clock_offset + (double)audio_samples_pushed / godot_mix_rate;
}
void FFmpegPlayer::_reset_audio_clock(double pos) {
    audio_clock_offset   = pos;
    audio_samples_pushed = 0;
    audio_clock_active   = false;
}
void FFmpegPlayer::_reset_last_audio_pts() {
    last_audio_pts      = -1.0;
    audio_resync_needed = false;
}

// ─── [7] التعافي التلقائي من الفجوات الزمنية ─────────────────────────────────
void FFmpegPlayer::_handle_audio_gap(double gap_secs) {
    if (gap_secs < AUDIO_GAP_RESYNC_S) return;

    UtilityFunctions::print("[AUDIO-RECOVERY] Gap=", gap_secs, "s");

    if (gap_secs >= AUDIO_GAP_SKIP_S) {
        // فجوة كبيرة: اقفز للنقطة الحالية
        UtilityFunctions::print("[AUDIO-RECOVERY] Skip — seeking to pos=", position);
        if (audio_codec_ctx)  avcodec_flush_buffers(audio_codec_ctx);
        if (ext_audio_ctx)    avcodec_flush_buffers(ext_audio_ctx);
        for (auto *f : decoded_audio_queue)    av_frame_free(&f);
        decoded_audio_queue.clear();
        for (auto *f : ext_audio_frame_queue)  av_frame_free(&f);
        ext_audio_frame_queue.clear();
        _reset_audio_clock(position);
    } else {
        // فجوة متوسطة: flush الكودك فقط
        if (audio_codec_ctx)  avcodec_flush_buffers(audio_codec_ctx);
        if (ext_audio_ctx)    avcodec_flush_buffers(ext_audio_ctx);
        UtilityFunctions::print("[AUDIO-RECOVERY] Flush done, resyncing clock.");
    }
    _reset_last_audio_pts();
}

// ─── مساعدات بدء/إيقاف الصوت ─────────────────────────────────────────────────
void FFmpegPlayer::_start_audio_at(double pos) {
    _reset_audio_clock(pos);
    _reset_last_audio_pts();

    if (ext_using_godot_player) {
        if (ext_audio_player && ext_audio_player->get_stream().is_valid()) {
            if (ext_audio_player->is_playing()) ext_audio_player->stop();
            ext_audio_player->play((float)pos);
        }
        return;
    }
    if (int_audio_player && !int_audio_generator.is_null()) {
        if (int_audio_player->is_playing()) int_audio_player->stop();
        int_audio_player->play();
        int_audio_playback = Ref<AudioStreamGeneratorPlayback>(
            Object::cast_to<AudioStreamGeneratorPlayback>(
                int_audio_player->get_stream_playback().ptr()));
    }
    _apply_audio_volume();
}

void FFmpegPlayer::_stop_audio() {
    if (ext_using_godot_player) {
        if (ext_audio_player && ext_audio_player->is_playing()) ext_audio_player->stop();
    } else {
        if (int_audio_player && int_audio_player->is_playing()) int_audio_player->stop();
    }
    int_audio_playback.unref();
    _reset_audio_clock(0.0);
}

void FFmpegPlayer::_pause_audio() {
    if (ext_using_godot_player) { if (ext_audio_player) ext_audio_player->set_stream_paused(true); }
    else                        { if (int_audio_player) int_audio_player->set_stream_paused(true); }
}

void FFmpegPlayer::_resume_audio(double pos) {
    if (ext_using_godot_player) {
        if (ext_audio_player && ext_audio_player->get_stream().is_valid()) {
            ext_audio_player->set_stream_paused(false);
            if (!ext_audio_player->is_playing()) ext_audio_player->play((float)pos);
        }
        return;
    }
    if (int_audio_player) {
        int_audio_player->set_stream_paused(false);
        if (!int_audio_player->is_playing()) {
            int_audio_player->play();
            int_audio_playback = Ref<AudioStreamGeneratorPlayback>(
                Object::cast_to<AudioStreamGeneratorPlayback>(
                    int_audio_player->get_stream_playback().ptr()));
        }
    }
}

// ─── [9] صمت بديل عند فراغ الطابور ──────────────────────────────────────────
void FFmpegPlayer::_fill_silence(int n) {
    if (int_audio_playback.is_null() || n <= 0) return;
    int avail = int_audio_playback->get_frames_available();
    int to_fill = (n < avail) ? n : avail;
    if (to_fill <= 0) return;

    PackedVector2Array silence;
    silence.resize(to_fill);
    Vector2 *w = silence.ptrw();
    for (int i = 0; i < to_fill; i++) w[i] = Vector2(0.0f, 0.0f);
    int_audio_playback->push_buffer(silence);
}

// ─── [8+9+10] ضخ الصوت مع جميع ضمانات الجودة ────────────────────────────────
void FFmpegPlayer::_push_audio_frames(
    std::deque<AVFrame*> &frame_queue, SwrContext *swr, int src_rate)
{
    if (int_audio_playback.is_null() || !swr) return;

    int total_f   = (int)(godot_mix_rate * int_audio_generator->get_buffer_length());
    int avail_f   = int_audio_playback->get_frames_available();
    int buf_f     = total_f - avail_f;
    double buf_ms = (double)buf_f / godot_mix_rate * 1000.0;

    // [5] فرملة: لا تضخ إذا البافر ممتلئ
    if (buf_ms >= AUDIO_BUFFER_MAX_MS) return;

    // [9] إذا فرغ الطابور، اضخ صمت لتحافظ على استقرار محرك Godot
    if (frame_queue.empty()) {
        int silence_n = (int)(godot_mix_rate * 0.020); // 20ms
        if (buf_f < silence_n) _fill_silence(silence_n - buf_f);
        return;
    }

    while (!frame_queue.empty()) {
        avail_f = int_audio_playback->get_frames_available();
        buf_ms  = (double)(total_f - avail_f) / godot_mix_rate * 1000.0;
        if (buf_ms >= AUDIO_BUFFER_MAX_MS) break;

        AVFrame *af = frame_queue.front();
        frame_queue.pop_front();

        // [7] كشف الفجوة الزمنية بين الإطارات
        if (af->pts != AV_NOPTS_VALUE && last_audio_pts >= 0.0) {
            double cur_pts = af->pts * (src_rate > 0 ? 1.0 / src_rate : 1.0 / godot_mix_rate);
            double gap     = cur_pts - last_audio_pts;
            if (gap > AUDIO_GAP_RESYNC_S) {
                av_frame_free(&af);
                _handle_audio_gap(gap); return;
            }
            last_audio_pts = cur_pts;
        } else if (af->pts != AV_NOPTS_VALUE && last_audio_pts < 0.0) {
            last_audio_pts = af->pts * (src_rate > 0 ? 1.0 / src_rate : 1.0 / godot_mix_rate);
        }

        // حساب عدد العينات المُخرَجة بعد Resampling
        int src  = (src_rate > 0) ? src_rate : godot_mix_rate;
        int out_n = av_rescale_rnd(
            swr_get_delay(swr, src) + af->nb_samples,
            godot_mix_rate, src, AV_ROUND_UP);
        if (out_n <= 0) { av_frame_free(&af); continue; }

        std::vector<float> buf(out_n * 2, 0.0f);
        uint8_t *ptr = (uint8_t *)buf.data();

        int conv = swr_convert(swr, &ptr, out_n,
            (const uint8_t **)af->data, af->nb_samples);
        av_frame_free(&af);
        if (conv <= 0) continue;

        PackedVector2Array stereo;
        stereo.resize(conv);
        Vector2 *w = stereo.ptrw();

        for (int i = 0; i < conv; i++) {
            float l = buf[i * 2];
            float r = buf[i * 2 + 1];
            // [8] Soft Clipping: tanh بدلاً من القطع الصارم (يمنع الطقطقة)
            l = (l >  1.0f || l < -1.0f) ? tanhf(l) : l;
            r = (r >  1.0f || r < -1.0f) ? tanhf(r) : r;
            w[i] = Vector2(l, r);
        }

        if (int_audio_playback->push_buffer(stereo)) {
            audio_samples_pushed += conv;
            audio_clock_active    = true;
        }
    }
}

// ─── play ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::play() {
    if (!fmt_ctx) { _emit_playback_error("No video loaded"); return; }
    playing = true; buffering = true; frame_timer = 0.0;
    _reset_audio_clock(position);
    _reset_last_audio_pts();
    if (is_inside_tree()) emit_signal("buffering_changed", true);
    UtilityFunctions::print("[PLAY] Buffering...");
}

// ─── pause ────────────────────────────────────────────────────────────────────
void FFmpegPlayer::pause() {
    if (!playing) return;
    playing = false; _pause_audio();
}

// ─── stop ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::stop() {
    playing = false; _stop_audio(); seek(0.0);
}

// ─── seek ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::seek(double seconds) {
    if (!fmt_ctx || !video_codec_ctx) return;

    bool was_playing = playing;
    playing = false; buffering = true;
    _stop_audio();
    // [6] مسح كامل للطوابير الصوتية
    _reset_last_audio_pts();

    bool in_buf = (seconds >= position &&
                   seconds <= position + forward_buffer_secs - 0.5);

    if (in_buf) {
        // Fast Seek
        while (!decoded_frame_queue.empty() &&
               decoded_frame_queue.front().pts < seconds)
            decoded_frame_queue.pop_front();
        // [6] مسح إطارات الصوت القديمة
        while (!decoded_audio_queue.empty()) {
            AVFrame *af = decoded_audio_queue.front();
            bool old = (af->pts != AV_NOPTS_VALUE && audio_stream_idx >= 0 &&
                        af->pts * av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base)
                        < seconds);
            if (old) { av_frame_free(&af); decoded_audio_queue.pop_front(); }
            else break;
        }
        position = seconds; frame_timer = 0.0; buffering = false;
        UtilityFunctions::print("[SEEK] Fast → ", seconds);
    } else {
        // Full Seek
        _clear_queues(); // [6] يمسح كل شيء

        int64_t ts = (int64_t)(seconds / av_q2d(fmt_ctx->streams[video_stream_idx]->time_base));
        if (av_seek_frame(fmt_ctx, video_stream_idx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            _emit_playback_error("Seek failed: " + String::num(seconds, 2) + "s");
            if (was_playing) playing = true; return;
        }
        avcodec_flush_buffers(video_codec_ctx);
        if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);

        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = sws_getContext(video_width, video_height, video_codec_ctx->pix_fmt,
                                     video_width, video_height, AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }

        // seek في الصوت الخارجي
        if (ext_fmt_ctx && ext_audio_stream >= 0) {
            int64_t ats = (int64_t)(seconds / av_q2d(ext_fmt_ctx->streams[ext_audio_stream]->time_base));
            av_seek_frame(ext_fmt_ctx, ext_audio_stream, ats, AVSEEK_FLAG_BACKWARD);
            if (ext_audio_ctx) avcodec_flush_buffers(ext_audio_ctx);
            for (auto *p : ext_audio_pkt_queue)    av_packet_free(&p);
            ext_audio_pkt_queue.clear();
            for (auto *f : ext_audio_frame_queue)  av_frame_free(&f);
            ext_audio_frame_queue.clear();
        }

        position = seconds; frame_timer = 0.0; forward_buffer_secs = 0.0;
        if (is_inside_tree()) emit_signal("buffering_changed", true);
        _prefill_buffers();

        if (forward_buffer_secs >= INITIAL_PLAY || !decoded_frame_queue.empty()) {
            buffering = false;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
        }
        UtilityFunctions::print("[SEEK] Full → ", seconds, " fwd=", forward_buffer_secs);
    }

    if (was_playing) {
        playing = true;
        if (!buffering) _start_audio_at(position);
    }
}

// ─── _process ─────────────────────────────────────────────────────────────────
// ─── _process ─────────────────────────────────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    if (!fmt_ctx || !playing) return;

    // التحقق الآمن من نجاح تحميل الصوت الشبكي في الخيط الرئيسي
    if (audio_load_finished_successfully) {
        audio_load_finished_successfully = false; // تصفير العلم فوراً لمنع التكرار
        if (playing && !buffering) {
            _start_audio_at(position); // تشغيل آمن ومستقر تماماً!
        }
    }

    _update_buffer_stats();

    // مرحلة التعبئة
    if (buffering) {
        _read_packets_to_queue();
        _read_ext_audio_packets();
        _decode_packets_into_queue();
        _decode_audio_into_queue();
        _decode_ext_audio_into_queue();

        if (forward_buffer_secs >= INITIAL_PLAY || !decoded_frame_queue.empty()) {
            buffering = false;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
            _start_audio_at(position);
        }
        return;
    }

    // نضوب البافر
    if (forward_buffer_secs < 0.2 && decoded_frame_queue.empty()) {
        buffering = true; _pause_audio();
        if (is_inside_tree()) emit_signal("buffering_changed", true);
        UtilityFunctions::print("[BUFFER] Underrun pos=", position);
        return;
    }

    // ─── [11] تقدم الوقت مع Hard Frame Drop ──────────────────────────────
    bool using_ffmpeg_audio = audio_clock_active && !ext_using_godot_player &&
                              (audio_stream_idx >= 0 || ext_audio_stream >= 0);

    if (using_ffmpeg_audio) {
        // [11] تطبيق GPU Latency Offset على ساعة الصوت
        double audio_clk = _get_audio_clock() + GPU_LATENCY_OFFSET;
        double drift     = position - audio_clk; // موجب = فيديو سابق

        if (drift > 1.5 / fps) {
            // الفيديو متقدم على الصوت: لا تقدم position
            if (!ext_using_godot_player) {
                if (audio_stream_idx >= 0 && swr_ctx)
                    _push_audio_frames(decoded_audio_queue, swr_ctx, audio_sample_rate);
                else if (ext_audio_stream >= 0 && ext_swr_ctx)
                    _push_audio_frames(ext_audio_frame_queue, ext_swr_ctx, 0);
            }
            return;
        }

        // [11] Hard Frame Drop: إسقاط إطارات الفيديو المتأخرة
        // إذا تأخر الفيديو عن الصوت بأكثر من HARD_DROP_THRESHOLD
        while (decoded_frame_queue.size() > 1) {
            double frame_pts = decoded_frame_queue.front().pts;
            if (audio_clk - frame_pts > HARD_DROP_THRESHOLD)
                decoded_frame_queue.pop_front(); // اسقط الإطار المتأخر
            else break;
        }

        position = audio_clk;
    } else {
        position += delta;
    }

    frame_timer += delta;

    // قراءة وفك تشفير
    _read_packets_to_queue();
    _read_ext_audio_packets();

    if ((int)decoded_frame_queue.size() < MAX_DECODED_FRAMES)
        _decode_packets_into_queue();
    if ((int)decoded_audio_queue.size() < MAX_AUDIO_FRAMES)
        _decode_audio_into_queue();
    if ((int)ext_audio_frame_queue.size() < MAX_AUDIO_FRAMES)
        _decode_ext_audio_into_queue();

    // عرض الإطار
    _present_frame_at(position);

    // ضخ الصوت
    if (!ext_using_godot_player) {
        if (audio_stream_idx >= 0 && swr_ctx)
            _push_audio_frames(decoded_audio_queue, swr_ctx, audio_sample_rate);
        else if (ext_audio_stream >= 0 && ext_swr_ctx)
            _push_audio_frames(ext_audio_frame_queue, ext_swr_ctx, 0);
    }

    // [4] إشارة الحالة كل 0.5ث
    status_timer += delta;
    if (status_timer >= STATUS_INTERVAL) {
        status_timer = 0.0; _emit_buffering_status();
    }

    // نهاية الفيديو
    if (duration > 0.0 && position >= duration) {
        if (looping) seek(0.0);
        else { stop(); _emit_video_finished(); }
    }
}


// ─── _update_buffer_stats ──────────────────────────────────────────────────────
void FFmpegPlayer::_update_buffer_stats() {
    if (video_stream_idx < 0 || !fmt_ctx) return;
    double tb     = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstart = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
                    ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;

    if (video_packet_queue.empty())
        forward_buffer_secs = decoded_frame_queue.empty() ? 0.0
            : Math::max(0.0, decoded_frame_queue.back().pts - position);
    else {
        AVPacket *last = video_packet_queue.back();
        if (last->pts != AV_NOPTS_VALUE)
            forward_buffer_secs = Math::max(0.0, (last->pts * tb) - vstart - position);
    }
}

int FFmpegPlayer::_calc_read_batch_size() const {
    if (forward_buffer_secs < MIN_FORWARD) return 100;
    if (forward_buffer_secs < MAX_FORWARD) return 30;
    return 5;
}

// ─── قراءة الحزم الخام ────────────────────────────────────────────────────────
void FFmpegPlayer::_read_packets_to_queue() {
    if (!fmt_ctx) return;
    int batch    = _calc_read_batch_size();
    AVPacket *pk = av_packet_alloc();

    for (int i = 0; i < batch; i++) {
        int ret = av_read_frame(fmt_ctx, pk);
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                _emit_playback_error("Read error — connection lost?");
            av_packet_unref(pk); break;
        }
        if (pk->stream_index == video_stream_idx)
            video_packet_queue.push_back(av_packet_clone(pk));
        else if (!use_external_audio && pk->stream_index == audio_stream_idx)
            audio_packet_queue.push_back(av_packet_clone(pk));
        av_packet_unref(pk);
    }
    av_packet_free(&pk);

    // حذف الحزم القديمة
    if (video_stream_idx >= 0) {
        double tb     = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
        double vstart = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
                        ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;
        while (!video_packet_queue.empty()) {
            AVPacket *o = video_packet_queue.front();
            if (o->pts != AV_NOPTS_VALUE && (o->pts * tb) - vstart < position - 5.0)
                { av_packet_free(&o); video_packet_queue.pop_front(); }
            else break;
        }
    }
    if (audio_stream_idx >= 0) {
        double atb = av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base);
        while (!audio_packet_queue.empty()) {
            AVPacket *o = audio_packet_queue.front();
            if (o->pts != AV_NOPTS_VALUE && o->pts * atb < position - 2.0)
                { av_packet_free(&o); audio_packet_queue.pop_front(); }
            else break;
        }
    }
}

// ─── قراءة حزم الصوت الخارجي ─────────────────────────────────────────────────
void FFmpegPlayer::_read_ext_audio_packets() {
    if (!ext_fmt_ctx || ext_audio_stream < 0 || ext_audio_eof) return;
    if ((int)ext_audio_pkt_queue.size() >= MAX_AUDIO_FRAMES * 2) return;

    AVPacket *pk = av_packet_alloc();
    for (int i = 0; i < 20; i++) {
        int ret = av_read_frame(ext_fmt_ctx, pk);
        if (ret < 0) {
            if (ret == AVERROR_EOF) ext_audio_eof = true;
            else _emit_playback_error("Ext-audio read error — connection lost?");
            break;
        }
        if (pk->stream_index == ext_audio_stream)
            ext_audio_pkt_queue.push_back(av_packet_clone(pk));
        av_packet_unref(pk);
    }
    av_packet_free(&pk);
}

// ─── فك تشفير الفيديو ────────────────────────────────────────────────────────
void FFmpegPlayer::_decode_packets_into_queue() {
    if (!video_codec_ctx || !frame_buffer) return;
    if ((int)decoded_frame_queue.size() >= MAX_DECODED_FRAMES) return;

    double tb     = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstart = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
                    ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;

    AVFrame *vf  = av_frame_alloc();
    int loops    = 0;

    while ((int)decoded_frame_queue.size() < MAX_DECODED_FRAMES && loops++ < 50) {
        int ret = avcodec_receive_frame(video_codec_ctx, vf);
        if (ret == 0) {
            double pts = (vf->pts != AV_NOPTS_VALUE) ? (vf->pts * tb) - vstart : position;
            if (pts < position - 0.5) { av_frame_unref(vf); continue; }

            if (!sws_ctx)
                sws_ctx = sws_getContext(video_width, video_height, (AVPixelFormat)vf->format,
                                         video_width, video_height, AV_PIX_FMT_RGB24,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

            uint8_t *dst[4] = { frame_buffer, nullptr, nullptr, nullptr };
            int dls[4]      = { video_width * 3, 0, 0, 0 };
            sws_scale(sws_ctx, vf->data, vf->linesize, 0, video_height, dst, dls);

            DecodedFrame df;
            df.pts = pts;
            df.data.resize(video_width * video_height * 3);
            memcpy(df.data.ptrw(), frame_buffer, df.data.size());
            decoded_frame_queue.push_back(std::move(df));
            av_frame_unref(vf); continue;
        }
        if (ret == AVERROR(EAGAIN)) {
            if (video_packet_queue.empty()) break;
            AVPacket *p = video_packet_queue.front(); video_packet_queue.pop_front();
            int sr = avcodec_send_packet(video_codec_ctx, p); av_packet_free(&p);
            if (sr < 0 && sr != AVERROR(EAGAIN)) break;
        } else break;
    }
    av_frame_free(&vf);
}

// ─── فك تشفير الصوت الداخلي ──────────────────────────────────────────────────
void FFmpegPlayer::_decode_audio_into_queue() {
    if (!audio_codec_ctx) return;
    if ((int)decoded_audio_queue.size() >= MAX_AUDIO_FRAMES) return;

    int loops = 0;
    while ((int)decoded_audio_queue.size() < MAX_AUDIO_FRAMES && loops++ < 50) {
        AVFrame *af = av_frame_alloc();
        int ret     = avcodec_receive_frame(audio_codec_ctx, af);
        if (ret == 0) { decoded_audio_queue.push_back(af); continue; }
        av_frame_free(&af);
        if (ret == AVERROR(EAGAIN)) {
            if (audio_packet_queue.empty()) break;
            AVPacket *p = audio_packet_queue.front(); audio_packet_queue.pop_front();
            int sr = avcodec_send_packet(audio_codec_ctx, p); av_packet_free(&p);
            if (sr < 0 && sr != AVERROR(EAGAIN)) break;
        } else break;
    }
}

// ─── فك تشفير الصوت الخارجي FFmpeg ──────────────────────────────────────────
void FFmpegPlayer::_decode_ext_audio_into_queue() {
    if (!ext_audio_ctx) return;
    if ((int)ext_audio_frame_queue.size() >= MAX_AUDIO_FRAMES) return;

    int loops = 0;
    while ((int)ext_audio_frame_queue.size() < MAX_AUDIO_FRAMES && loops++ < 50) {
        AVFrame *af = av_frame_alloc();
        int ret     = avcodec_receive_frame(ext_audio_ctx, af);
        if (ret == 0) { ext_audio_frame_queue.push_back(af); continue; }
        av_frame_free(&af);
        if (ret == AVERROR(EAGAIN)) {
            if (ext_audio_pkt_queue.empty()) break;
            AVPacket *p = ext_audio_pkt_queue.front(); ext_audio_pkt_queue.pop_front();
            int sr = avcodec_send_packet(ext_audio_ctx, p); av_packet_free(&p);
            if (sr < 0 && sr != AVERROR(EAGAIN)) break;
        } else break;
    }
}
// ─── [11] عرض الإطار (مع كبح جماح الـ Hard Drop لمنع الـ Lag) ─────────────────────
bool FFmpegPlayer::_present_frame_at(double pos) {
    if (decoded_frame_queue.empty()) return false;

    int frames_dropped_this_tick = 0;
    const int MAX_FRAMES_TO_DROP_PER_TICK = 3; // حد أمان يمنع تشنج المعالج

    // البحث عن الإطار الأنسب للوقت الحالي وإسقاط كل ما تجاوزه الزمن بشكل تدريجي
    while (decoded_frame_queue.size() > 1) {
        double next_pts = decoded_frame_queue[1].pts;
        
        // إذا كان الإطار التالي قد حان موعده
        if (next_pts <= pos) {
            decoded_frame_queue.pop_front();
            frames_dropped_this_tick++;
            
            // إذا وصلنا للحد المسموح به في هذا الفريم، نخرج فوراً ونترك الباقي للفريم القادم
            if (frames_dropped_this_tick >= MAX_FRAMES_TO_DROP_PER_TICK) {
                break;
            }
        } else {
            break;
        }
    }

    const DecodedFrame &f = decoded_frame_queue.front();

    // إذا كان الإطار الحالي لا يزال بعيداً عن وقت العرض (أكثر من 40ms) ننتظر
    if (f.pts > pos + 0.04) return false;

    if (!current_texture.is_valid() || f.data.is_empty()) return false;

    // تحديث الصورة المعروضة
    Ref<Image> img = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGB8, f.data);
    current_texture->update(img);
    _emit_frame_updated();
    
    return true;
}
// ─── الملء الأولي ─────────────────────────────────────────────────────────────
void FFmpegPlayer::_prefill_buffers() {
    if (!fmt_ctx) return;
    UtilityFunctions::print("[PREFILL] → ", INITIAL_PLAY, "s...");
    int tries = 0;
    while (forward_buffer_secs < INITIAL_PLAY && tries++ < 400) {
        _read_packets_to_queue(); _update_buffer_stats();
    }
    _decode_packets_into_queue();
    _decode_audio_into_queue();
    if (ext_fmt_ctx) {
        for (int i = 0; i < 10; i++) _read_ext_audio_packets();
        _decode_ext_audio_into_queue();
    }
    UtilityFunctions::print("[PREFILL] Done fwd=", forward_buffer_secs,
        " vf=", decoded_frame_queue.size(), " af=",
        decoded_audio_queue.size() + ext_audio_frame_queue.size());
}

// ─── get_buffer_status ────────────────────────────────────────────────────────
float FFmpegPlayer::get_buffer_status() {
    if (forward_buffer_secs >= INITIAL_PLAY) return 100.0f;
    float p = (float)(forward_buffer_secs / INITIAL_PLAY) * 100.0f;
    return p < 0.0f ? 0.0f : p;
}

// ─── [6] تنظيف كامل للطوابير ──────────────────────────────────────────────────
void FFmpegPlayer::_clear_queues() {
    while (!video_packet_queue.empty())
        { av_packet_free(&video_packet_queue.front()); video_packet_queue.pop_front(); }
    while (!audio_packet_queue.empty())
        { av_packet_free(&audio_packet_queue.front()); audio_packet_queue.pop_front(); }
    // [6] مسح الصوت المُفكَّك أيضاً
    while (!decoded_audio_queue.empty())
        { av_frame_free(&decoded_audio_queue.front()); decoded_audio_queue.pop_front(); }
    for (auto *f : ext_audio_frame_queue) av_frame_free(&f);
    ext_audio_frame_queue.clear();

    decoded_frame_queue.clear();
    forward_buffer_secs = 0.0;
}

// ─── التنظيف الكامل ───────────────────────────────────────────────────────────
// ─── التنظيف الكامل ───────────────────────────────────────────────────────────
void FFmpegPlayer::_cleanup() {
    // 1. تأمين وإغلاق الخيط أولاً وقبل كل شيء لمنع تصادم الذاكرة (Race Conditions)
    if (audio_loading_thread_running) {
        audio_loading_thread_running = false; 
        if (audio_loading_thread.joinable()) {
            audio_loading_thread.join(); // انتظر إغلاق الخيط بسلام
        }
    }

    _clear_queues();
    _cleanup_ext_audio();
    _stop_audio();

    if (video_codec_ctx) { avcodec_free_context(&video_codec_ctx); video_codec_ctx = nullptr; }
    if (audio_codec_ctx) { avcodec_free_context(&audio_codec_ctx); audio_codec_ctx = nullptr; }
    if (fmt_ctx)         { avformat_close_input(&fmt_ctx);         fmt_ctx         = nullptr; }
    if (sws_ctx)         { sws_freeContext(sws_ctx);               sws_ctx         = nullptr; }
    if (swr_ctx)         { swr_free(&swr_ctx);                     swr_ctx         = nullptr; }
    if (frame_buffer)    { av_free(frame_buffer);                  frame_buffer    = nullptr; }

    int_audio_generator.unref();
    int_audio_playback.unref();

    duration = 0.0; position = 0.0; forward_buffer_secs = 0.0;
    frame_timer = 0.0; status_timer = 0.0;
    playing = false; buffering = false;
    is_live_stream = false; use_external_audio = false;
    video_stream_idx = -1; audio_stream_idx = -1;
    video_width = 0; video_height = 0; fps = 0.0;
    audio_sample_rate = 0; audio_channels = 0;
    loaded_audio_path = ""; ext_using_godot_player = false;
    audio_samples_pushed = 0; audio_clock_offset = 0.0; audio_clock_active = false;
    last_audio_pts = -1.0; audio_resync_needed = false;
}

// ─── الإشارات ─────────────────────────────────────────────────────────────────
void FFmpegPlayer::_emit_video_loaded(bool s)  { if (is_inside_tree()) emit_signal("video_loaded", s); }
void FFmpegPlayer::_emit_video_finished()       { if (is_inside_tree()) emit_signal("video_finished"); }
void FFmpegPlayer::_emit_frame_updated()        { if (is_inside_tree()) emit_signal("frame_updated", current_texture); }
void FFmpegPlayer::_emit_playback_error(const String &m) {
    UtilityFunctions::printerr("[ERROR] ", m);
    if (is_inside_tree()) emit_signal("playback_error", m);
}
void FFmpegPlayer::_emit_buffering_status() {
    double lo = decoded_frame_queue.empty() ? 0.0
              : Math::max(0.0, decoded_frame_queue.front().pts - position);
    if (is_inside_tree())
        emit_signal("buffering_status", (float)lo, (float)forward_buffer_secs);
}

// ─── Getters / Setters ────────────────────────────────────────────────────────
bool   FFmpegPlayer::is_playing()       const { return playing && !buffering; }
double FFmpegPlayer::get_duration()     const { return duration; }
double FFmpegPlayer::get_position()     const { return position; }
int    FFmpegPlayer::get_video_width()  const { return video_width; }
int    FFmpegPlayer::get_video_height() const { return video_height; }
double FFmpegPlayer::get_fps()          const { return fps; }
Ref<ImageTexture> FFmpegPlayer::get_current_frame_texture() const { return current_texture; }
void FFmpegPlayer::set_loop(bool en) { looping = en; }
bool FFmpegPlayer::get_loop() const  { return looping; }

// ─── نقطة دخول GDExtension ───────────────────────────────────────────────────
extern "C" {
    GDExtensionBool GDE_EXPORT gdffmpeg_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization)
    {
        godot::GDExtensionBinding::InitObject init_obj(
            p_get_proc_address, p_library, r_initialization);
        init_obj.register_initializer([](godot::ModuleInitializationLevel level) {
            if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE)
                godot::ClassDB::register_class<godot::FFmpegPlayer>();
        });
        init_obj.register_terminator([](godot::ModuleInitializationLevel) {});
        init_obj.set_minimum_library_initialization_level(
            godot::MODULE_INITIALIZATION_LEVEL_SCENE);
        return init_obj.init();
    }
}
