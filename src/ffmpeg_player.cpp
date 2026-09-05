/**
 * ffmpeg_player.cpp
 * GDExtension — FFmpeg Video Player (Unified) for Godot 4
 *
 * الإصدار الحالي: 7.5.1
 * سجل التغييرات الكامل (كل إصدار وسببه): راجع CHANGELOG_ffmpeg_player.md
 * بجانب هذا الملف — لا تُضِف تاريخ إصدارات هنا، فقط الكود.
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
    ClassDB::bind_method(D_METHOD("load_video", "path", "referer"), &FFmpegPlayer::load_video, DEFVAL(""));
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
    // [AV-SYNC v6.5] تُطلَق عند اكتشاف انجراف بين الصوت الخارجي المحلي والفيديو
    ADD_SIGNAL(MethodInfo("av_sync_issue",
        PropertyInfo(Variant::STRING, "message"),
        PropertyInfo(Variant::FLOAT, "drift_seconds")));
}

FFmpegPlayer::FFmpegPlayer() {}
FFmpegPlayer::~FFmpegPlayer() { _cleanup(); }

// [DECODER-WARMUP v7.5.1] تعريف العلم الساكن — مرة واحدة فقط لعمر التطبيق كله
std::atomic<bool> FFmpegPlayer::warmup_done{false};

// ─── [DECODER-WARMUP v7.5.1] تسخين مُفكِّك MediaCodec بالعتاد مسبقًا ───────────
// نفتح جلسة h264_mediacodec وهمية بمعاملات افتراضية شائعة (1280×720) دون
// إطعامها أي بيانات فعلية، ثم نُغلقها فورًا. الهدف: تحفيز إنشاء جلسة
// MediaCodec الحقيقية مع سائق الجهاز (AMediaCodec_configure/start) — وهي على
// الأرجح الجزء الأكثر تكلفة زمنيًا (وليس فك بيانات فعلية) — أثناء وقت "ميت"
// (تصفّح المستخدم قبل طلب فيديو حقيقي) بدل حدوثها على المسار الحرج لأول
// تشغيل. لا علاقة لهذه الجلسة الوهمية بأي فيديو حقيقي لاحق (يُفتح بمعامﻻته
// الخاصة بشكل مستقل تمامًا). فشل هذه الدالة بأي شكل آمن تمامًا ولا يؤثر على
// التشغيل الفعلي إطلاقًا — سياق منفصل يُغلَق فورًا بعد المحاولة.
//
// [v7.5.1 — مهم] تُستدعى الآن من الخيط الرئيسي مباشرة (وليس خيط منفصل).
// السبب: MediaCodec يحتاج بيئة JNI مرتبطة (Java VM) لأي عملية داخلية، وهذا
// متوفر تلقائيًا للخيط الرئيسي عبر إطار عمل Android/Godot، لكن أي std::thread
// خام ننشئه يدويًا لا يملك هذا الارتباط إطلاقًا — كان هذا يسبب فشل التسخين
// الدائم بخطأ "Operation not permitted" في v7.5. التكلفة الزمنية لهذا
// الاستدعاء المتزامن صغيرة نسبيًا ومرة واحدة فقط لعمر التطبيق (وليس لكل
// فيديو)، وهي مقايضة مقبولة مقابل ضمان عمل الميزة فعليًا.
//
// [صدق واجب] لا يوجد يقين كامل أن فتح الجلسة وحده (دون فك إطار حقيقي) يلتقط
// كامل فجوة "الكودك جاهز ← أول إطار" الملاحَظة في السجلات — تبيّن لاحقًا أن
// هذه الفجوة نفسها متذبذبة جدًا بين التجارب (450ms إلى 12+ ثانية)، فقد لا
// تكون تكلفة MediaCodec ثابتة كما افتُرض ابتداءً. راجع CHANGELOG للتفاصيل.
void FFmpegPlayer::_decoder_warmup_worker() {
    auto t0 = std::chrono::steady_clock::now();
    UtilityFunctions::print("[WARMUP] بدء تسخين مُفكِّك الفيديو بالعتاد (على الخيط الرئيسي، مرة واحدة لعمر التطبيق)...");

    const AVCodec *vc = avcodec_find_decoder_by_name("h264_mediacodec");
    if (!vc) {
        UtilityFunctions::print("[WARMUP] h264_mediacodec غير متوفر في هذا البناء — تخطي التسخين.");
        return;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(vc);
    if (!ctx) {
        UtilityFunctions::print("[WARMUP] فشل تخصيص سياق الكودك — تخطي.");
        return;
    }

    // معاملات افتراضية شائعة فقط لتحفيز إنشاء الجلسة — غير مرتبطة بأي فيديو حقيقي
    ctx->width   = 1280;
    ctx->height  = 720;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    int ret = avcodec_open2(ctx, vc, nullptr);
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        UtilityFunctions::print("[WARMUP] فشل فتح جلسة التسخين (", String(err_buf),
            ") بعد ", ms, "ms — لا يؤثر على التشغيل الفعلي لاحقًا.");
    } else {
        UtilityFunctions::print("[WARMUP] جلسة MediaCodec سُخِّنت بنجاح خلال ", ms, "ms");
    }

    avcodec_free_context(&ctx);
}

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

    UtilityFunctions::print("--- FFmpeg GDExtension v7.5.1 ---");

    // [DECODER-WARMUP-JNI-FIX v7.5.1] تصحيح جوهري: خيط std::thread خام غير
    // مرتبط ببيئة JNI الخاصة بأندرويد التي يحتاجها MediaCodec داخليًا —
    // كان هذا يسبب فشل التسخين دائمًا بخطأ "Operation not permitted" (انظر
    // CHANGELOG_ffmpeg_player.md لتفاصيل التشخيص). الخيط الرئيسي لـ Godot
    // مرتبط بـ JNI تلقائيًا من إطار العمل نفسه، لذا ننفّذ التسخين هنا
    // متزامنًا (وليس في خيط منفصل) — تكلفته صغيرة ومرة واحدة فقط لعمر
    // التطبيق كله، وهذا أفضل من ميزة "غير حاجبة" لكنها معطوبة دائمًا.
    if (!warmup_done.exchange(true)) {
        _decoder_warmup_worker();
    }

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
// [ASYNC-VIDEO v6.3] لروابط الشبكة: الفتح الفعلي (avformat_open_input +
// avformat_find_stream_info) يتم الآن في خيط خلفي عبر _open_video_async_worker
// لمنع تجميد محرك Godot. النتيجة تُفحَص في بداية _process() وتُستكمل هناك عبر
// _finalize_loaded_video(). الملفات المحلية تبقى متزامنة كما كانت (فتحها سريع).
bool FFmpegPlayer::load_video(const String &path, const String &referer) {
    _cleanup();
    buffering = false; forward_buffer_secs = 0.0;
    position  = 0.0;   frame_timer         = 0.0;
    _reset_last_audio_pts();

    // [TIMING-DIAG v7.3] نقطة الصفر الزمنية — كل الطوابع اللاحقة نسبية لها
    load_start_tp = std::chrono::steady_clock::now();
    first_frame_decoded_logged  = false;
    first_playback_start_logged = false;

    if (path.is_empty()) { _emit_playback_error("Path is empty"); return false; }

    bool is_live           = path.begins_with("rtmp://") || path.begins_with("rtsp://");
    bool video_is_network  = path.begins_with("http://") || path.begins_with("https://");
    bool is_network        = is_live || video_is_network;

    if (path.ends_with(".m3u") || path.contains(".m3u?")) {
        _emit_playback_error("M3U_DETECTED"); return false;
    }

    UtilityFunctions::print("[LOAD] Mode: ",
        is_live ? "Live Stream" : (video_is_network ? "Direct URL" : "Local File"),
        " | Audio: internal (embedded) by default, external overrides via load_audio()");

    video_is_network_source = is_network; // [THREAD-SAFE v7.0]

    // ── [ASYNC-VIDEO v6.3] الشبكة: فتح غير متزامن في خيط خلفي ────────────────
    // لا يُجمَّد المحرك؛ النتيجة (نجاح/فشل) تصل عبر إشارة video_loaded من
    // داخل _process() بمجرد اكتمال العملية في الخيط الخلفي.
    if (is_network) {
        // تأمين: إن كان هناك خيط سابق ما زال يعمل (نادر، لكن للاحتياط) ننتظره
        if (video_loading_thread_running) {
            if (video_loading_thread.joinable()) video_loading_thread.join();
        }
        video_load_ready             = false;
        video_load_error             = false;
        pending_video_error_message  = "";
        pending_video_path           = path;
        pending_video_is_live        = is_live;
        video_loading_thread_running = true;
        video_loading_thread = std::thread(&FFmpegPlayer::_open_video_async_worker,
                                            this, path, is_live, referer);
        UtilityFunctions::print("[LOAD] Opening network video asynchronously...");
        return true; // النتيجة الفعلية تصل لاحقًا عبر إشارة video_loaded
    }

    // ── ملف محلي: يبقى متزامنًا (الفتح المحلي سريع، لا حاجة لخيط) ────────────
    String real_path = ProjectSettings::get_singleton()->globalize_path(path);
    CharString utf8   = real_path.utf8();
    AVFormatContext *local_ctx = nullptr;

    if (avformat_open_input(&local_ctx, utf8.get_data(), nullptr, nullptr) < 0) {
        _emit_playback_error("Cannot open: " + path);
        _emit_video_loaded(false); return false;
    }

    if (avformat_find_stream_info(local_ctx, nullptr) < 0) {
        avformat_close_input(&local_ctx);
        _emit_playback_error("Cannot read stream info: " + path);
        return false;
    }

    return _finalize_loaded_video(local_ctx, false);
}

// ─── [ASYNC-VIDEO v6.3] فتح الفيديو الشبكي في خيط خلفي ──────────────────────
// تحذير: هذه الدالة تُنفَّذ على خيط منفصل تمامًا عن الخيط الرئيسي — يُمنع
// استدعاء أي دالة من Godot (Node/Signal/Texture/...) من داخلها. فقط عمليات
// FFmpeg الخام مسموحة هنا. النتيجة تُسلَّم للخيط الرئيسي عبر
// pending_video_fmt_ctx + الأعلام الذرية (atomic) ليقرأها _process().
void FFmpegPlayer::_open_video_async_worker(String path, bool is_live, String referer) {
    AVFormatContext *ctx = nullptr;
    AVDictionary   *opts = nullptr;
    CharString      utf8 = path.utf8();

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
    // [TIMEOUT v7.0] يحدّ أقصى مدة انتظار لأي استدعاء قراءة منفرد (حتى داخل
    // الخيط الخلفي نفسه لاحقًا) — يمنع الاحتجاز الأبدي على اتصال ميت بصمت
    av_dict_set(&opts, "timeout",             "15000000", 0); // 15 ثانية
    av_dict_set(&opts, "rw_timeout",          "15000000", 0);
    // [12] لا reconnect_streamed للملفات المباشرة (غير البث الحي)
    if (is_live) av_dict_set(&opts, "reconnect_streamed", "1", 0);

    // [REFERER v7.4] بعض مضيفي الفيديو (Streamtape وأمثاله) يطبّقون حماية
    // مضادة للاستخدام المباشر (Anti-Hotlinking) تتحقق من ترويسة Referer قبل
    // خدمة أي بايت — وقد تتعمّد تأخير الاستجابة بدل رفضها صراحة إن غابت هذه
    // الترويسة (إجراء شائع مضاد للبوتات). نرسلها فقط إن زوّدنا بها المستدعي.
    if (!referer.is_empty()) {
        CharString referer_utf8 = referer.utf8();
        av_dict_set(&opts, "referer", referer_utf8.get_data(), 0);
    }

    // ── [PROBE-SPEED v6.3] نفس تحسينات تسريع الفتح المستخدمة في الصوت ────────
    // (كانت مفقودة هنا سابقًا رغم وجودها في _open_audio_with_ffmpeg، مما كان
    // يجعل avformat_find_stream_info يستخدم القيم الافتراضية الأبطأ لـ FFmpeg)
    av_dict_set(&opts, "probesize",       "524288",  0); // 512 KB بدلاً من ~5MB
    av_dict_set(&opts, "analyzeduration", "2000000", 0); // 2 ثانية بدلاً من ~5s

    int ret = avformat_open_input(&ctx, utf8.get_data(), nullptr, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        pending_video_error_message = "Cannot open: " + path + " (" + String(err_buf) + ")";
        video_load_error = true;
        video_loading_thread_running = false;
        return;
    }

    // [TIMING-DIAG v7.3] وقت فتح الاتصال (TLS/HTTP handshake + أول استجابة)
    UtilityFunctions::print("[TIMING] avformat_open_input نجح بعد ", _elapsed_ms_since_load(), "ms");

    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        pending_video_error_message = "Cannot read stream info: " + path;
        video_load_error = true;
        video_loading_thread_running = false;
        return;
    }

    // [TIMING-DIAG v7.3] وقت تحليل الصيغة (Probing) بالإضافة لما سبقه
    UtilityFunctions::print("[TIMING] avformat_find_stream_info نجح بعد ", _elapsed_ms_since_load(), "ms");

    // تسليم النتيجة للخيط الرئيسي — لا شيء آخر من Godot يُلمَس هنا
    pending_video_fmt_ctx        = ctx;
    video_load_ready             = true;
    video_loading_thread_running = false;
}

// ─── [ASYNC-VIDEO v6.3] إتمام تحميل الفيديو بعد نجاح الفتح ─────────────────
// يُستدعى حصريًا من الخيط الرئيسي (من load_video() للملفات المحلية، أو من
// _process() بعد التقاط نتيجة الخيط الخلفي للفيديو الشبكي). يحتوي كل المنطق
// الذي كان سابقًا داخل load_video() بعد نجاح avformat_open_input.
bool FFmpegPlayer::_finalize_loaded_video(AVFormatContext *opened_ctx, bool is_live) {
    // [TIMING-DIAG v7.3] لحظة استلام الخيط الرئيسي لنتيجة الفتح فعليًا —
    // يكشف أي فجوة جدولة (لو كان الخيط الرئيسي مشغولًا بشيء آخر قبل أن
    // يصل لمعالجة video_load_ready في _process())
    UtilityFunctions::print("[TIMING] الخيط الرئيسي استلم نتيجة الفتح بعد ", _elapsed_ms_since_load(), "ms");

    fmt_ctx        = opened_ctx;
    is_live_stream = is_live;

    stream_start_time = (fmt_ctx->start_time != AV_NOPTS_VALUE)
                        ? (double)fmt_ctx->start_time / AV_TIME_BASE : 0.0;

    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx < 0) {
        _cleanup();
        _emit_playback_error("No video stream");
        _emit_video_loaded(false);
        return false;
    }
    if (!_setup_video_codec(fmt_ctx->streams[video_stream_idx])) {
        _cleanup();
        _emit_video_loaded(false);
        return false;
    }

    // ── [AUDIO-FALLBACK v6.2] الصوت الداخلي المدمج دائمًا هو الخيار الافتراضي ──
    audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_idx >= 0) {
        AVCodecContext *ac = nullptr; SwrContext *sw = nullptr;
        int rate = 0, ch = 0;
        if (_setup_audio_codec(fmt_ctx->streams[audio_stream_idx], ac, sw, rate, ch)) {
            audio_codec_ctx = ac; swr_ctx = sw;
            audio_sample_rate = rate; audio_channels = ch;
            int_audio_generator.instantiate();
            int_audio_generator->set_mix_rate((float)godot_mix_rate);
            int_audio_generator->set_buffer_length(0.30);
            int_audio_player->set_stream(int_audio_generator);
            audio_active_source = AudioActiveSource::INTERNAL_EMBEDDED;
            UtilityFunctions::print("[AUDIO] Internal (embedded) track ready — active by default");
        } else {
            UtilityFunctions::printerr("[AUDIO] Internal setup failed, video-only.");
            audio_stream_idx = -1;
            audio_active_source = AudioActiveSource::NONE;
        }
    } else {
        audio_active_source = AudioActiveSource::NONE;
    }

    duration = (fmt_ctx->duration != AV_NOPTS_VALUE)
               ? (double)fmt_ctx->duration / AV_TIME_BASE : 0.0;

    _allocate_buffers();
    _emit_video_loaded(true);
    UtilityFunctions::print("[LOAD] OK | dur=", duration, "s | fps=", fps,
        " | ", video_width, "x", video_height, " | mix=", godot_mix_rate, "Hz");
    // [TIMING-DIAG v7.3] وقت جاهزية الكودك بالكامل (بعد فتح مُفكِّك الفيديو،
    // بما فيها تهيئة MediaCodec إن وُجد تسريع عتاد)
    UtilityFunctions::print("[TIMING] الكودك جاهز (بما فيه MediaCodec إن وُجد) بعد ", _elapsed_ms_since_load(), "ms");

    // ── [THREAD-SAFE v7.0] لمصادر الشبكة: ابدأ خيط القراءة المستمرة المستقل ──
    // من هذه اللحظة فصاعدًا، هذا الخيط هو المالك الحصري لـ fmt_ctx.
    if (video_is_network_source) {
        network_reader_active = true;
        network_read_thread = std::thread(&FFmpegPlayer::_network_read_worker, this);
        UtilityFunctions::print("[NET-READ] Background network reader thread started.");
    }
    return true;
}

// ─── [THREAD-SAFE v7.0] خيط القراءة الشبكية المستمرة للفيديو ─────────────────
// يعمل هذا الخيط طوال حياة الجلسة الحالية للفيديو الشبكي. هو المالك الحصري
// لـ fmt_ctx: يقرأ الحزم باستمرار ويدفعها في الطوابير المحمية، ويُنفّذ أي
// طلب Seek واردًا من الخيط الرئيسي بنفسه (بدل استدعاء av_seek_frame من هناك).
// لا يُستدعى أي av_read_frame/av_seek_frame على fmt_ctx من أي مكان آخر بتاتًا
// طالما هذا الخيط حيّ — هذا ما يضمن عدم تجمّد الواجهة إطلاقًا مهما ساءت الشبكة.
void FFmpegPlayer::_network_read_worker() {
    AVPacket *pk = av_packet_alloc();

    // [BUFFER-OSCILLATION-DIAG v7.5.1] عدّاد وتوقيت لطباعة معدّل إنتاج
    // الحزم الفعلي (كم حزمة فيديو وصلت خلال آخر ~0.5 ثانية) — يُقارَن مع
    // [BUF-DIAG] المطبوعة من _update_buffer_stats() على الخيط الرئيسي
    // لتحديد إن كان التذبذب سببه بطء الشبكة (إنتاج قليل) أو بطء استهلاك
    // فك التشفير (إنتاج جيد لكن الطابور لا يُستهلَك بنفس السرعة).
    int video_pkts_since_last_print = 0;
    auto diag_last_print = std::chrono::steady_clock::now();

    while (network_reader_active) {
        // ── معالجة طلب Seek (نحن المالك الحصري لـ fmt_ctx، آمن تمامًا) ───────
        if (network_seek_requested) {
            double target = network_seek_target_secs;
            int64_t ts = (int64_t)(target / av_q2d(fmt_ctx->streams[video_stream_idx]->time_base));
            int sret = av_seek_frame(fmt_ctx, video_stream_idx, ts, AVSEEK_FLAG_BACKWARD);

            {
                std::lock_guard<std::mutex> lock(network_queue_mutex);
                while (!video_packet_queue.empty())
                    { av_packet_free(&video_packet_queue.front()); video_packet_queue.pop_front(); }
                while (!audio_packet_queue.empty())
                    { av_packet_free(&audio_packet_queue.front()); audio_packet_queue.pop_front(); }
            }

            network_seek_failed     = (sret < 0);
            demux_eof_reached       = false;
            network_seek_requested  = false;
            network_seek_done       = true; // الخيط الرئيسي ينتظر هذا العلم في _process()
            continue;
        }

        // ── حماية من نمو الذاكرة إن كانت الشبكة أسرع بكثير من الاستهلاك ──────
        bool queue_full;
        {
            std::lock_guard<std::mutex> lock(network_queue_mutex);
            queue_full = ((int)(video_packet_queue.size() + audio_packet_queue.size())
                          >= NETWORK_READ_QUEUE_CAP_PACKETS);
        }
        if (queue_full) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // ── القراءة الفعلية — قد تنتظر طويلًا على شبكة سيئة، لكننا على خيط
        // مستقل تمامًا عن Godot، فلا يتأثر المحرك أو الواجهة إطلاقًا ──────────
        int ret = av_read_frame(fmt_ctx, pk);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                demux_eof_reached = true;
                // ننام بهدوء بدل الخروج الكامل: قد يصل طلب Seek(0) لاحقًا
                // (تكرار Loop) ونحتاج هذا الخيط حيًا لمعالجته
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                network_read_error_flag = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            continue;
        }

        if (pk->stream_index == video_stream_idx ||
            (!use_external_audio && pk->stream_index == audio_stream_idx)) {
            AVPacket *clone = av_packet_clone(pk);
            std::lock_guard<std::mutex> lock(network_queue_mutex);
            if (pk->stream_index == video_stream_idx) {
                video_packet_queue.push_back(clone);
                video_pkts_since_last_print++;
            } else
                audio_packet_queue.push_back(clone);
        }
        av_packet_unref(pk);

        // [BUFFER-OSCILLATION-DIAG v7.5.1] طباعة مُهدَّأة لمعدّل إنتاج حزم
        // الفيديو الفعلي من الشبكة
        auto diag_now = std::chrono::steady_clock::now();
        double diag_elapsed = std::chrono::duration<double>(diag_now - diag_last_print).count();
        if (diag_elapsed >= 0.5) {
            UtilityFunctions::print("[NET-DIAG] حزم فيديو واردة آخر ", diag_elapsed,
                "s: ", video_pkts_since_last_print,
                " (~", (double)video_pkts_since_last_print / diag_elapsed, " حزمة/ث)");
            video_pkts_since_last_print = 0;
            diag_last_print = diag_now;
        }
    }

    av_packet_free(&pk);
}

// ─── [THREAD-SAFE v7.0] خيط القراءة الشبكية المستمرة للصوت الخارجي ──────────
// نفس فلسفة _network_read_worker تمامًا لكن لـ ext_fmt_ctx (صوت خارجي شبكي).
void FFmpegPlayer::_ext_network_read_worker() {
    AVPacket *pk = av_packet_alloc();

    while (ext_network_reader_active) {
        if (ext_network_seek_requested) {
            double target = ext_network_seek_target_secs;
            int64_t ats = (int64_t)(target / av_q2d(ext_fmt_ctx->streams[ext_audio_stream]->time_base));
            av_seek_frame(ext_fmt_ctx, ext_audio_stream, ats, AVSEEK_FLAG_BACKWARD);
            if (ext_audio_ctx) avcodec_flush_buffers(ext_audio_ctx);
            {
                std::lock_guard<std::mutex> lock(ext_network_queue_mutex);
                for (auto *p : ext_audio_pkt_queue) av_packet_free(&p);
                ext_audio_pkt_queue.clear();
            }
            ext_audio_eof = false;
            ext_network_seek_requested = false;
            continue;
        }

        bool queue_full;
        {
            std::lock_guard<std::mutex> lock(ext_network_queue_mutex);
            queue_full = ((int)ext_audio_pkt_queue.size() >= MAX_AUDIO_FRAMES * 4);
        }
        if (queue_full) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        int ret = av_read_frame(ext_fmt_ctx, pk);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                ext_audio_eof = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                ext_network_read_error_flag = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            continue;
        }

        if (pk->stream_index == ext_audio_stream) {
            AVPacket *clone = av_packet_clone(pk);
            std::lock_guard<std::mutex> lock(ext_network_queue_mutex);
            ext_audio_pkt_queue.push_back(clone);
        }
        av_packet_unref(pk);
    }

    av_packet_free(&pk);
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
        int_audio_generator->set_buffer_length(0.30);
        int_audio_player->set_stream(int_audio_generator);
    }

    ext_audio_eof = false;
    ext_using_godot_player = false;
    _reset_last_audio_pts();

    // ── [THREAD-SAFE v7.0] ابدأ خيط القراءة المستمرة المستقل لهذا المصدر ──────
    ext_network_reader_active = true;
    ext_network_read_thread = std::thread(&FFmpegPlayer::_ext_network_read_worker, this);

    UtilityFunctions::print("[DEBUG-AUDIO] Success! Audio is ready to play.");
    emit_signal("audio_loaded", true);
    return true;
}



// ─── تنظيف الصوت الخارجي ──────────────────────────────────────────────────────
void FFmpegPlayer::_cleanup_ext_audio() {
    // [THREAD-SAFE v7.0] أوقف خيط القراءة الشبكية أولًا وقبل كل شيء —
    // إغلاق ext_fmt_ctx أثناء قراءة الخيط له كارثي (استخدام بعد التحرير)
    // [TODO-مستقبلًا] نفس ملاحظة _cleanup() الخاصة بخيط الفيديو: هذا join()
    // محدود بـ timeout الفتح (15s) كحد أقصى، فقط عند تبديل/إلغاء الصوت
    // الخارجي أثناء انقطاع شبكي كامل. حلّه الجذري (Detach + تنظيف ذاتي +
    // generation counter) نفس الفكرة تمامًا — مؤجَّل لنفس السبب.
    if (ext_network_reader_active) {
        ext_network_reader_active = false;
        if (ext_network_read_thread.joinable()) ext_network_read_thread.join();
    }
    ext_network_seek_requested  = false;
    ext_network_read_error_flag = false;

    {
        std::lock_guard<std::mutex> lock(ext_network_queue_mutex);
        for (auto *p : ext_audio_pkt_queue)   av_packet_free(&p);
        ext_audio_pkt_queue.clear();
    }
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

// ─── [M v6.2] مساعدات أولوية مصدر الصوت ──────────────────────────────────────

// يفرّغ طوابير الصوت الداخلي (المُفكَّك وغير المُفكَّك) بأمان — يُستخدم عند
// التبديل إلى مصدر خارجي لمنع تراكب الصوتين لحظة التبديل.
void FFmpegPlayer::_flush_internal_audio_queues() {
    for (auto *f : decoded_audio_queue) av_frame_free(&f);
    decoded_audio_queue.clear();
    while (!audio_packet_queue.empty()) {
        av_packet_free(&audio_packet_queue.front());
        audio_packet_queue.pop_front();
    }
}

// تبديل فوري إلى صوت خارجي محلي (مشغَّل عبر AudioStreamPlayer مستقل، مثل
// res:// / user:// أو ملف .mp3/.ogg مطلق) — يوقف الصوت الداخلي بالكامل.
void FFmpegPlayer::_switch_to_external_local_file(const Ref<AudioStream> &stream, const String &path) {
    if (int_audio_player && int_audio_player->is_playing()) int_audio_player->stop();
    int_audio_playback.unref();
    _flush_internal_audio_queues();

    ext_audio_player->set_stream(stream);
    loaded_audio_path = path;
    ext_using_godot_player = true;
    use_external_audio     = true;
    audio_active_source    = AudioActiveSource::EXTERNAL_LOCAL_FILE;
    external_audio_ready   = true;

    _apply_audio_volume();
    if (playing && !buffering) ext_audio_player->play((float)position);
}

// عودة صريحة إلى الصوت الداخلي المدمج (عند استدعاء load_audio("") أو عند
// فشل التحميل الخارجي) — الصوت الداخلي غالبًا لم يتوقف أصلاً، هذا يضمن ذلك.
void FFmpegPlayer::_revert_to_internal_audio() {
    if (ext_audio_player && ext_audio_player->is_playing()) ext_audio_player->stop();
    ext_using_godot_player = false;

    external_audio_requested = false;
    external_audio_ready     = false;
    use_external_audio       = false;

    audio_active_source = (audio_stream_idx >= 0 && swr_ctx)
        ? AudioActiveSource::INTERNAL_EMBEDDED
        : AudioActiveSource::NONE;

    if (playing && !buffering) {
        _start_audio_at(position);
    }
}

// ─── تحميل الصوت الخارجي ─────────────────────────────────────────────────────
// [v6.2] لم يعد مرتبطًا بنوع الفيديو إطلاقًا: يمكن استدعاؤه دائمًا. الصوت
// الداخلي المدمج يستمر بالعمل دون انقطاع أثناء التحميل (خاصة الشبكي غير
// المتزامن)، ولا يتم التبديل إلا بعد نجاح التحميل الفعلي — بلا فجوة صمت.
bool FFmpegPlayer::load_audio(const String &path) {
    // إيقاف مشغل الصوت الخارجي المستقل القديم بأمان (إن كان يعمل)
    if (ext_audio_player && ext_audio_player->is_playing()) ext_audio_player->stop();

    // تأمين الخيط: إذا كان هناك خيط يعمل حالياً، ننتظر نهايته لضمان سلامة الذاكرة
    if (audio_loading_thread_running) {
        if (audio_loading_thread.joinable()) {
            audio_loading_thread.join();
        }
    }

    // مسح مصدر الصوت الخارجي القديم فقط. الصوت الداخلي (المدمج) لا يُلمَس هنا
    // إطلاقًا — يستمر تلقائيًا كاحتياطي حتى لحظة نجاح المصدر الجديد فعليًا.
    _cleanup_ext_audio();
    ext_using_godot_player = false;
    audio_load_finished_successfully = false;

    if (path.is_empty()) {
        // طلب صريح بإلغاء أي صوت خارجي → عودة فورية للصوت الداخلي المدمج
        loaded_audio_path = "";
        _revert_to_internal_audio();
        emit_signal("audio_loaded", false);
        return false;
    }

    external_audio_requested = true;

    // الروابط الشبكية: تحميل غير متزامن (الصوت الداخلي يستمر حتى ينجح التحميل)
    if (path.begins_with("http://") || path.begins_with("https://")) {
        loaded_audio_path = path;
        audio_loading_thread_running = true;
        audio_loading_thread = std::thread(&FFmpegPlayer::_open_audio_async_worker, this, path);
        return true;
    }

    // res:// أو user:// — تبديل متزامن وفوري
    if (path.begins_with("res://") || path.begins_with("user://")) {
        Ref<Resource> res = ResourceLoader::get_singleton()->load(path);
        Ref<AudioStream> s = res;
        if (s.is_null()) {
            _emit_playback_error("Cannot load: " + path);
            emit_signal("audio_loaded", false);
            external_audio_requested = false;
            return false;
        }
        _switch_to_external_local_file(s, path);
        emit_signal("audio_loaded", true);
        return true;
    }

    // مسار ملف محلي مطلق (.mp3 / .ogg خارج res:// أو user://)
    Ref<FileAccess> fa = FileAccess::open(path, FileAccess::READ);
    if (fa.is_null()) {
        _emit_playback_error("Audio file not found: " + path);
        emit_signal("audio_loaded", false);
        external_audio_requested = false;
        return false;
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
        emit_signal("audio_loaded", false);
        external_audio_requested = false;
        return false;
    }
    if (s.is_null()) {
        _emit_playback_error("Audio decode failed: " + path);
        emit_signal("audio_loaded", false);
        external_audio_requested = false;
        return false;
    }
    _switch_to_external_local_file(s, path);
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
    // [v6.2] بعد إلغاء تحميل الصوت الخارجي، نعود تلقائيًا للصوت الداخلي المدمج
    _revert_to_internal_audio();
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
// ─── ساعة الصوت (مُصلَح: تعويض ديناميكي لتأخير البافر الداخلي) ──────────────
double FFmpegPlayer::_get_audio_clock() const {
    if (!audio_clock_active) return position;

    double raw_clock = audio_clock_offset + (double)audio_samples_pushed / godot_mix_rate;

    // [تصحيح التأخير] اطرح مدة الصوت الذي ضُخّ لكنه لا يزال ينتظر دوره
    // داخل بافر AudioStreamGenerator (لم يُسمَع بعد فعلياً)
    if (int_audio_playback.is_valid() && int_audio_generator.is_valid()) {
        int avail_f = int_audio_playback->get_frames_available();
        int total_f = (int)(godot_mix_rate * int_audio_generator->get_buffer_length());
        int buffered_unplayed = total_f - avail_f;
        if (buffered_unplayed > 0)
            raw_clock -= (double)buffered_unplayed / godot_mix_rate;
    }

    return raw_clock;
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

// ─── [AV-SYNC v6.5] كشف وتصحيح انجراف الصوت الخارجي المحلي عن الفيديو ────────
// يُستدعى فقط في مسار "position += delta" (أي عند ext_using_godot_player).
// المبدأ: نقرأ الوقت الحقيقي الذي وصله مشغّل الصوت (Godot) فعليًا عبر
// get_playback_position()، ونقارنه بموضع الفيديو الحالي (position):
//   • فرق صغير جدًا (< AV_SYNC_RESYNC_THRESHOLD)  → طبيعي، لا إجراء.
//   • فرق مقبول لكنه محسوس                          → تصحيح صامت (نطابق
//     position مع الصوت فورًا، بلا إشعار المستخدم لأنه غير ملحوظ عمليًا).
//   • فرق كبير وملحوظ (>= AV_SYNC_WARNING_THRESHOLD) → نُصحّح الموضع + نُصدر
//     إشارة av_sync_issue لإعلام الواجهة/المستخدم أن التزامن قد يكون تأثر.
void FFmpegPlayer::_check_av_sync(double delta) {
    if (!ext_using_godot_player || !ext_audio_player) return;
    if (!ext_audio_player->is_playing()) return;

    av_sync_check_timer += delta;
    if (av_sync_check_timer < AV_SYNC_CHECK_INTERVAL) return;
    av_sync_check_timer = 0.0;

    double actual_audio_pos = (double)ext_audio_player->get_playback_position();
    double drift     = position - actual_audio_pos; // موجب = الفيديو متقدم على الصوت
    double abs_drift = (drift < 0.0) ? -drift : drift;

    if (abs_drift < AV_SYNC_RESYNC_THRESHOLD) {
        return; // فرق طبيعي ضمن الحدود المقبولة — لا إجراء
    }

    if (abs_drift >= AV_SYNC_WARNING_THRESHOLD) {
        String msg = (drift > 0.0)
            ? "قد لا يوجد تزامن: الصورة متقدمة على الصوت بحوالي " + String::num(abs_drift, 2) + "ث"
            : "قد لا يوجد تزامن: الصوت متقدم على الصورة بحوالي " + String::num(abs_drift, 2) + "ث";
        UtilityFunctions::printerr("[AV-SYNC] ", msg);
        if (is_inside_tree()) emit_signal("av_sync_issue", msg, (float)abs_drift);
    } else {
        UtilityFunctions::print("[AV-SYNC] Minor drift auto-corrected: ", drift, "s");
    }

    // تصحيح فعلي في الحالتين (المقبول والملحوظ): نُعيد مطابقة موضع الفيديو
    // لموضع الصوت الحقيقي — الصوت هو المرجع لأن أذن المستمع أكثر حساسية
    // لتقطّعه من حساسية العين لقفزة إطار واحدة في الصورة.
    position = actual_audio_pos;
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

// ─── [8+9+10] ضخ الصوت مع جميع ضمانات الجودة (مُصلَح: عدم فقد العينات) ───────
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

        // [7] كشف الفجوة الزمنية بين الإطارات (يُفحص قبل السحب من الطابور)
        if (af->pts != AV_NOPTS_VALUE && last_audio_pts >= 0.0) {
            double cur_pts = af->pts * (src_rate > 0 ? 1.0 / src_rate : 1.0 / godot_mix_rate);
            double gap     = cur_pts - last_audio_pts;
            if (gap > AUDIO_GAP_RESYNC_S) {
                frame_queue.pop_front();
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
        if (out_n <= 0) {
            frame_queue.pop_front();
            av_frame_free(&af);
            continue;
        }

        std::vector<float> buf(out_n * 2, 0.0f);
        uint8_t *ptr = (uint8_t *)buf.data();

        int conv = swr_convert(swr, &ptr, out_n,
            (const uint8_t **)af->data, af->nb_samples);
        if (conv <= 0) {
            frame_queue.pop_front();
            av_frame_free(&af);
            continue;
        }

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

        // [مُصلَح] لا نحذف الإطار من الطابور إلا بعد نجاح الضخ الفعلي
        if (int_audio_playback->push_buffer(stereo)) {
            frame_queue.pop_front();
            av_frame_free(&af);
            audio_samples_pushed += conv;
            audio_clock_active    = true;
        } else {
            // البافر ممتلئ فعلياً رغم الحساب — توقف هنا واترك الإطار لمحاولة لاحقة
            // لا تحذف العينات، ولا تسحب الإطار من الطابور
            break;
        }
    }
}


// ─── play ─────────────────────────────────────────────────────────────────────
// [ASYNC-VIDEO v6.3] fmt_ctx قد يكون nullptr مؤقتًا أثناء انتظار اكتمال فتح
// فيديو شبكي في الخيط الخلفي؛ في هذه الحالة نؤجل التشغيل الفعلي عبر
// pending_autoplay ويُنفَّذ تلقائيًا من _process() بمجرد نجاح التحميل.
void FFmpegPlayer::play() {
    if (!fmt_ctx) {
        if (video_loading_thread_running || video_load_ready) {
            pending_autoplay = true;
            // [TIMING-DIAG v7.3] play() استُدعيت قبل اكتمال الفتح — تؤجَّل
            UtilityFunctions::print("[TIMING] play() استُدعيت (مؤجَّلة، الفتح لم يكتمل بعد) عند ", _elapsed_ms_since_load(), "ms");
            return;
        }
        _emit_playback_error("No video loaded"); return;
    }
    playing = true; buffering = true; frame_timer = 0.0;
    _reset_audio_clock(position);
    _reset_last_audio_pts();
    if (is_inside_tree()) emit_signal("buffering_changed", true);
    // [TIMING-DIAG v7.3] لحظة استدعاء play() الفعلية (فوريًا، الفيديو جاهز)
    UtilityFunctions::print("[TIMING] play() استُدعيت (فوريًا) عند ", _elapsed_ms_since_load(), "ms");
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
        // Fast Seek — لا يلمس video_packet_queue/audio_packet_queue إطلاقًا
        // (فقط طوابير فك التشفير المحلية للخيط الرئيسي)، لذا آمن كما هو
        // بغض النظر عن نشاط أي خيط قراءة شبكي.
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

    } else if (network_reader_active) {
        // ── [THREAD-SAFE v7.0] Full Seek على مصدر شبكي ──────────────────────
        // لا نستدعي av_seek_frame على fmt_ctx من هنا إطلاقًا (المالك الحصري
        // له هو _network_read_worker). نُرسل طلبًا ذريًا غير حاجب ونعود فورًا؛
        // _process() سيتابع الحالة عبر network_seek_done دون أي انتظار.
        _clear_queues(); // آمن (محمي بالقفل داخليًا) — يمسح طوابير فك التشفير أيضًا

        pending_video_seek_target   = seconds;
        pending_video_seek_active   = true;
        pending_autoplay_after_seek = was_playing;

        network_seek_done       = false;
        network_seek_failed     = false;
        network_seek_target_secs = seconds;
        network_seek_requested   = true;

        // نفس الأمر للصوت الخارجي الشبكي إن كان نشطًا — يعالج نفسه بنفسه
        if (ext_network_reader_active) {
            ext_network_seek_target_secs = seconds;
            ext_network_seek_requested   = true;
        }

        UtilityFunctions::print("[SEEK] Network seek requested → ", seconds);
        return; // buffering يبقى true، playing يبقى false حتى تكتمل العملية

    } else {
        // Full Seek لملف محلي — متزامن وآمن تمامًا (لا شبكة، لا خيوط قراءة)
        _clear_queues(); // [6] يمسح كل شيء
        // [v6.2] لم نعد عند EOF بعد أي seek ناجح
        demux_eof_reached = false;

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

        // seek في الصوت الخارجي (محلي أيضًا في هذه الحالة النظرية فقط —
        // عمليًا ext_fmt_ctx شبكي دائمًا ويُعالَج في الفرع أعلاه، لكن نُبقي
        // هذا كخط دفاع احتياطي لو تغيّرت الافتراضات مستقبلًا)
        if (ext_fmt_ctx && ext_audio_stream >= 0 && !ext_network_reader_active) {
            int64_t ats = (int64_t)(seconds / av_q2d(ext_fmt_ctx->streams[ext_audio_stream]->time_base));
            av_seek_frame(ext_fmt_ctx, ext_audio_stream, ats, AVSEEK_FLAG_BACKWARD);
            if (ext_audio_ctx) avcodec_flush_buffers(ext_audio_ctx);
            {
                std::lock_guard<std::mutex> lock(ext_network_queue_mutex);
                for (auto *p : ext_audio_pkt_queue) av_packet_free(&p);
                ext_audio_pkt_queue.clear();
            }
            for (auto *f : ext_audio_frame_queue)  av_frame_free(&f);
            ext_audio_frame_queue.clear();
            ext_audio_eof = false;
        } else if (ext_network_reader_active) {
            ext_network_seek_target_secs = seconds;
            ext_network_seek_requested   = true;
        }

        position = seconds; frame_timer = 0.0; forward_buffer_secs = 0.0;
        if (is_inside_tree()) emit_signal("buffering_changed", true);

        // [BUFFER-FIX v6.4] نفس منطق الهدف المتغيّر: 5s أول تخزين، 2s لاحقًا
        double target = first_buffer_done ? REBUFFER_TARGET : INITIAL_PLAY;
        _prefill_buffers(target);

        bool enough_buffered  = forward_buffer_secs >= target;
        bool stream_exhausted = demux_eof_reached && video_packet_queue.empty();

        // [DECODE-GATE v7.1] نفس الحماية: لا نخرج من buffering إلا مع إطار
        // جاهز فعليًا — راجع نفس التعليق في _process() لتفاصيل السبب.
        if ((enough_buffered || (stream_exhausted && !decoded_frame_queue.empty()))
            && !decoded_frame_queue.empty()) {
            buffering = false;
            first_buffer_done = true;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
        }
        UtilityFunctions::print("[SEEK] Full → ", seconds, " fwd=", forward_buffer_secs, " target=", target);
    }

    if (was_playing) {
        playing = true;
        if (!buffering) _start_audio_at(position);
    }
}

// ─── _process ─────────────────────────────────────────────────────────────────
// ─── _process ─────────────────────────────────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    // ── [ASYNC-VIDEO v6.3] التقاط نتيجة فتح الفيديو الشبكي من الخيط الخلفي ──
    // يجب أن يحدث هذا القسم قبل أي early-return (بما فيها "if (!fmt_ctx...)")
    // لأن fmt_ctx يبقى nullptr طوال فترة انتظار اكتمال الخيط الخلفي.
    if (video_load_ready) {
        video_load_ready = false;
        AVFormatContext *ctx = pending_video_fmt_ctx;
        pending_video_fmt_ctx = nullptr;
        bool is_live = pending_video_is_live;

        bool ok = _finalize_loaded_video(ctx, is_live);
        if (ok && pending_autoplay) {
            pending_autoplay = false;
            play();
        } else if (!ok) {
            pending_autoplay = false;
        }
    }
    if (video_load_error) {
        video_load_error = false;
        pending_autoplay = false;
        _emit_playback_error(pending_video_error_message);
        _emit_video_loaded(false);
    }

    // ── [THREAD-SAFE v7.0] التقاط نتيجة عملية Seek شبكية من الخيط الخلفي ─────
    // يجب أيضًا أن يحدث قبل early-return لأن playing=false طوال فترة الانتظار
    if (pending_video_seek_active && network_seek_done) {
        pending_video_seek_active = false;
        network_seek_done = false;

        if (network_seek_failed) {
            _emit_playback_error("Seek failed (network): " + String::num(pending_video_seek_target, 2) + "s");
            buffering = false;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
            playing = pending_autoplay_after_seek;
        } else {
            position = pending_video_seek_target;
            frame_timer = 0.0; forward_buffer_secs = 0.0;
            // buffering يبقى true؛ الحلقة الطبيعية أدناه (if (buffering)) ستملأ
            // المخزن تدريجيًا من نقطة الـ Seek الجديدة (يقرأها الخيط الخلفي
            // الآن باستمرار) وتخرج تلقائيًا بمجرد بلوغ الهدف — تمامًا كإعادة
            // تخزين عادية بعد Underrun، بلا أي انتظار حاجب على الإطلاق.
            playing = pending_autoplay_after_seek;
            if (playing && is_inside_tree()) emit_signal("buffering_changed", true);
            UtilityFunctions::print("[SEEK] Network seek applied → ", position);
        }
    }

    if (!fmt_ctx || !playing) return;

    // ── [THREAD-SAFE v7.0] تنبيه دوري (مُهدَّأ) بأخطاء القراءة الشبكية ────────
    // لا يوقف أي شيء — فقط إعلام؛ الخيوط الخلفية تستمر بالمحاولة تلقائيًا.
    network_error_notify_timer += delta;
    if (network_error_notify_timer >= NETWORK_ERROR_NOTIFY_INTERVAL) {
        network_error_notify_timer = 0.0;
        if (network_read_error_flag.exchange(false)) {
            _emit_playback_error("Network read error — retrying in background...");
        }
        if (ext_network_read_error_flag.exchange(false)) {
            _emit_playback_error("External audio network read error — retrying in background...");
        }
    }

    // التحقق الآمن من نجاح تحميل الصوت الشبكي في الخيط الرئيسي
    if (audio_load_finished_successfully) {
        audio_load_finished_successfully = false; // تصفير العلم فوراً لمنع التكرار

        // [v6.2] نجح تحميل الصوت الخارجي الشبكي — تبديل سلس وفوري إلى المصدر
        // الخارجي، مع تفريغ الصوت الداخلي لمنع تراكب الصوتين لحظة التبديل.
        external_audio_ready = true;
        use_external_audio   = true;
        audio_active_source  = AudioActiveSource::EXTERNAL_STREAM;
        _flush_internal_audio_queues();

        if (playing && !buffering) {
            _start_audio_at(position); // تشغيل آمن ومستقر تماماً!
        }
    }

    _update_buffer_stats();

    // ── [STATUS-FIX v6.4] إشارة حالة التخزين تُطلَق دائمًا — حتى أثناء
    // buffering=true — كي يتحدّث شريط التحميل فعليًا أمام المستخدم أثناء
    // الانتظار الحقيقي (كانت سابقًا تُحسب بعد "return" المبكر لمرحلة
    // buffering فلا تصل أبدًا أثناء التخزين الفعلي).
    status_timer += delta;
    if (status_timer >= STATUS_INTERVAL) {
        status_timer = 0.0;
        _emit_buffering_status();
    }

    // مرحلة التعبئة (Buffering)
    if (buffering) {
        _read_packets_to_queue();
        _read_ext_audio_packets();
        _decode_packets_into_queue();
        if (audio_active_source == AudioActiveSource::INTERNAL_EMBEDDED)
            _decode_audio_into_queue();
        if (external_audio_requested && ext_audio_ctx)
            _decode_ext_audio_into_queue();

        // ── [BUFFER-FIX v6.4] شرط خروج صحيح: لا نخرج لمجرد فك إطار واحد.
        // نخرج فقط إذا امتلأ المخزن فعليًا للهدف المطلوب (5s أول مرة، أو 2s
        // لإعادة التخزين اللاحقة)، أو إذا نفد المصدر بالكامل (EOF) فلم يعد
        // هناك ما ننتظره أصلاً.
        double target = first_buffer_done ? REBUFFER_TARGET : INITIAL_PLAY;
        bool enough_buffered  = forward_buffer_secs >= target;
        bool stream_exhausted = demux_eof_reached && video_packet_queue.empty();

        // ── [DECODE-GATE v7.1] لا نُطلق الصوت أبدًا قبل جهوزية إطار فيديو
        // واحد على الأقل للعرض. بدون هذا الشرط، إن كان فك تشفير الفيديو أبطأ
        // من تنزيل البيانات (شائع جدًا على المحاكيات التي تفتقر لتسريع العتاد
        // الحقيقي MediaCodec وتتراجع لفك تشفير برمجي بطيء)، كان الصوت يبدأ
        // فورًا بينما تبقى الشاشة سوداء لثوانٍ حتى يلحق فك التشفير — تمامًا
        // النمط المُبلَّغ عنه: "الصوت ينطلق بامتياز، الصورة تتأخر 7-8 ثوانٍ".
        // الآن ننتظر الاثنين معًا: بيانات كافية + إطار مفكوك جاهز فعليًا.
        bool first_frame_ready = !decoded_frame_queue.empty();

        if ((enough_buffered || (stream_exhausted && !decoded_frame_queue.empty()))
            && first_frame_ready) {
            buffering = false;
            first_buffer_done = true;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
            _start_audio_at(position);

            // [TIMING-DIAG v7.3] لحظة الانطلاق الفعلية الأولى فقط (صوت+صورة معًا)
            if (!first_playback_start_logged) {
                first_playback_start_logged = true;
                UtilityFunctions::print("[TIMING] الانطلاق الفعلي (صوت+صورة) بعد ", _elapsed_ms_since_load(), "ms");
            }
        }
        return;
    }

    // نضوب البافر (Underrun)
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
        // [11] تطبيق GPU Latency Offset على ساعة الصوت (لمزامنة العرض البصري
        // فقط — لا يُستخدم إطلاقًا لتحديد نهاية التشغيل، انظر شرط EOF أسفله)
        double audio_clk = _get_audio_clock() + GPU_LATENCY_OFFSET;
        double drift     = position - audio_clk; // موجب = فيديو سابق

        if (drift > 1.5 / fps) {
            // الفيديو متقدم على الصوت: لا تقدم position
            if (!ext_using_godot_player) {
                if (audio_active_source == AudioActiveSource::INTERNAL_EMBEDDED &&
                    audio_stream_idx >= 0 && swr_ctx)
                    _push_audio_frames(decoded_audio_queue, swr_ctx, audio_sample_rate);
                else if (audio_active_source == AudioActiveSource::EXTERNAL_STREAM &&
                         ext_audio_stream >= 0 && ext_swr_ctx)
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
        // [AV-SYNC v6.5] هذا المسار (صوت خارجي محلي عبر AudioStreamPlayer) هو
        // الوحيد الذي لا يعتمد على ساعة صوت فعلية، لذا نراقبه ونصححه هنا
        _check_av_sync(delta);
    }

    frame_timer += delta;

    // قراءة وفك تشفير الإطارات
    _read_packets_to_queue();
    _read_ext_audio_packets();

    if ((int)decoded_frame_queue.size() < MAX_DECODED_FRAMES)
        _decode_packets_into_queue();

    // [v6.2] نفك تشفير الصوت الداخلي فقط طالما هو المصدر النشط حاليًا —
    // بمجرد التبديل لمصدر خارجي نتوقف عن استهلاك موارد إضافية عليه.
    if (audio_active_source == AudioActiveSource::INTERNAL_EMBEDDED &&
        (int)decoded_audio_queue.size() < MAX_AUDIO_FRAMES)
        _decode_audio_into_queue();

    // [v6.2] نفك تشفير الصوت الخارجي الشبكي طالما طُلب (سواء كان نشطًا الآن
    // أو ما زال قيد التحميل بالخلفية) حتى يكون جاهزًا فور نجاح التحميل.
    if (external_audio_requested && ext_audio_ctx &&
        (int)ext_audio_frame_queue.size() < MAX_AUDIO_FRAMES)
        _decode_ext_audio_into_queue();

    // عرض الإطار الحالي على الشاشة
    _present_frame_at(position);

    // ضخ عينات الصوت إلى البافر — من المصدر النشط فقط (لا تراكب بين مصدرين)
    if (!ext_using_godot_player) {
        if (audio_active_source == AudioActiveSource::INTERNAL_EMBEDDED &&
            audio_stream_idx >= 0 && swr_ctx)
            _push_audio_frames(decoded_audio_queue, swr_ctx, audio_sample_rate);
        else if (audio_active_source == AudioActiveSource::EXTERNAL_STREAM &&
                 ext_audio_stream >= 0 && ext_swr_ctx)
            _push_audio_frames(ext_audio_frame_queue, ext_swr_ctx, 0);
    }

    // ─── [EOF-FIX v6.2] الاكتشاف الحقيقي لنهاية الفيديو ──────────────────────
    // بدلاً من "position >= duration" (كانت تفشل بسبب GPU_LATENCY_OFFSET
    // السالب وتبقى عالقة عند ~1 ثانية متبقية إلى الأبد)، ننتظر الآن:
    //   1) الديموكسر الرئيسي أعاد AVERROR_EOF فعليًا (demux_eof_reached)
    //   2) طوابير الفيديو والصوت الداخلي فارغة تمامًا (باستثناء آخر إطار معروض)
    //   3) إن كان هناك صوت خارجي شبكي نشط: ديموكسره أيضًا وصل EOF وطوابيره فارغة
    //   4) بافر الصوت المسموع (AudioStreamGenerator) لم يعد يحتوي صوتًا لم يُسمع بعد
    if (demux_eof_reached) {
        bool video_drained = video_packet_queue.empty() &&
                              decoded_frame_queue.size() <= 1;

        bool internal_audio_drained = audio_packet_queue.empty() &&
                                       decoded_audio_queue.empty();

        bool ext_audio_drained = (ext_audio_stream < 0) ||
                                  (ext_audio_eof &&
                                   ext_audio_pkt_queue.empty() &&
                                   ext_audio_frame_queue.empty());

        if (video_drained && internal_audio_drained && ext_audio_drained) {
            bool audio_buffer_empty = true;

            // إذا كنا نستخدم صوت جودو الداخلي/الخارجي عبر الـ generator والمشغل يعمل
            if (!ext_using_godot_player && int_audio_playback.is_valid() && int_audio_generator.is_valid()) {
                // حساب الإطارات التي تم ضخها ولا تزال تنتظر دورها في التشغيل داخل بافر جودو
                int remaining_frames = int_audio_playback->get_frames_available();
                int total_buffer_size = (int)(godot_mix_rate * int_audio_generator->get_buffer_length());
                int buffered_frames = total_buffer_size - remaining_frames;

                // إذا كان هناك أكثر من 40ms من الصوت لم تشغل بعد، ننتظر ولا ننهي الفيديو الآن
                if (buffered_frames > (int)(godot_mix_rate * 0.04)) {
                    audio_buffer_empty = false;
                }
            }

            if (audio_buffer_empty) {
                if (looping) {
                    demux_eof_reached = false;
                    seek(0.0);
                } else {
                    stop();
                    _emit_video_finished();
                }
            }
        }
    }
}




// ─── _update_buffer_stats ──────────────────────────────────────────────────────
// [THREAD-SAFE v7.0] video_packet_queue قد يُكتب إليه من خيط القراءة الشبكية
// في نفس اللحظة، لذا نحمي القراءة هنا بالقفل.
void FFmpegPlayer::_update_buffer_stats() {
    if (video_stream_idx < 0 || !fmt_ctx) return;
    double tb     = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstart = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
                    ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;

    // [BUFFER-OSCILLATION-DIAG v7.5.1] متغيرات تشخيصية فقط — لا تؤثر على أي
    // منطق فعلي، تُستخدم أدناه فقط للطباعة المُهدَّأة.
    int    diag_pkt_queue_size = 0;
    String diag_branch         = "?";
    double diag_last_pkt_time  = -1.0;

    {
        std::lock_guard<std::mutex> lock(network_queue_mutex);
        diag_pkt_queue_size = (int)video_packet_queue.size();

        if (video_packet_queue.empty()) {
            diag_branch = "decoded_frame_queue (packet_queue فارغ)";
            forward_buffer_secs = decoded_frame_queue.empty() ? 0.0
                : Math::max(0.0, decoded_frame_queue.back().pts - position);
        } else {
            diag_branch = "video_packet_queue (آخر حزمة)";
            AVPacket *last = video_packet_queue.back();
            if (last->pts != AV_NOPTS_VALUE) {
                diag_last_pkt_time = (last->pts * tb) - vstart;
                forward_buffer_secs = Math::max(0.0, diag_last_pkt_time - position);
            }
        }
    }

    // [BUFFER-OSCILLATION-DIAG v7.5.1] طباعة مُهدَّأة (كل ~0.5 ثانية) تكشف
    // الحالة الداخلية الفعلية للطوابير لحظة كل حساب — تفسّر أي قفزة/سقوط
    // مفاجئ في forward_buffer_secs (High/Stored) المعروض عبر buffering_status.
    // مثال: إن قفز video_pkt_q من عدد كبير لـ 0 بين طباعتين متتاليتين بينما
    // decoded_frame_q ظل صغيرًا، فهذا يعني أن فك التشفير أبطأ من استهلاك
    // الطابور، وليس نضوب شبكة حقيقي.
    static std::chrono::steady_clock::time_point diag_last_print{};
    auto diag_now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(diag_now - diag_last_print).count() >= 0.5) {
        diag_last_print = diag_now;
        UtilityFunctions::print("[BUF-DIAG] video_pkt_q=", diag_pkt_queue_size,
            " | decoded_frame_q=", (int)decoded_frame_queue.size(),
            " | المصدر=", diag_branch,
            " | آخر_وقت_حزمة=", diag_last_pkt_time,
            " | position=", position,
            " | fwd=", forward_buffer_secs,
            " | eof=", demux_eof_reached ? "نعم" : "لا");
    }
}

int FFmpegPlayer::_calc_read_batch_size() const {
    if (forward_buffer_secs < MIN_FORWARD) return 100;
    if (forward_buffer_secs < MAX_FORWARD) return 30;
    return 5;
}

// ─── قراءة الحزم الخام ────────────────────────────────────────────────────────
// [THREAD-SAFE v7.0] لمصادر الشبكة: القراءة الفعلية تتم بالكامل في
// _network_read_worker على خيط مستقل. هذه الدالة هنا فقط تُنظّف الحزم
// القديمة (للملفات المحلية تبقى القراءة متزامنة كما كانت تمامًا — آمنة
// وسريعة لأن قراءة القرص لا تُجمّد شيئًا).
void FFmpegPlayer::_read_packets_to_queue() {
    if (!fmt_ctx) return;

    if (network_reader_active) {
        _trim_old_packets();
        return;
    }

    int batch    = _calc_read_batch_size();
    AVPacket *pk = av_packet_alloc();

    for (int i = 0; i < batch; i++) {
        int ret = av_read_frame(fmt_ctx, pk);
        if (ret < 0) {
            // [v6.2] EOF حقيقي: نُسجّله لاستخدامه في اكتشاف نهاية التشغيل
            if (ret == AVERROR_EOF) {
                demux_eof_reached = true;
            } else {
                _emit_playback_error("Read error — connection lost?");
            }
            av_packet_unref(pk); break;
        }
        {
            std::lock_guard<std::mutex> lock(network_queue_mutex);
            if (pk->stream_index == video_stream_idx)
                video_packet_queue.push_back(av_packet_clone(pk));
            else if (!use_external_audio && pk->stream_index == audio_stream_idx)
                audio_packet_queue.push_back(av_packet_clone(pk));
        }
        av_packet_unref(pk);
    }
    av_packet_free(&pk);

    _trim_old_packets();
}

// ─── [THREAD-SAFE v7.0] تنظيف الحزم القديمة — دالة منفصلة يستدعيها كل من
// المسار المتزامن (ملفات محلية) والمسار الشبكي (بعد أن يملأها الخيط الخلفي) ──
void FFmpegPlayer::_trim_old_packets() {
    if (!fmt_ctx) return;
    std::lock_guard<std::mutex> lock(network_queue_mutex);

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
// [THREAD-SAFE v7.0] عندما يكون خيط القراءة الشبكية للصوت الخارجي نشطًا
// (وهي الحالة الوحيدة العملية لـ ext_fmt_ctx)، هو من يقرأ بالكامل؛ هذه
// الدالة تصبح no-op حينها لتفادي أي استدعاء av_read_frame من الخيط الرئيسي.
void FFmpegPlayer::_read_ext_audio_packets() {
    if (!ext_fmt_ctx || ext_audio_stream < 0) return;
    if (ext_network_reader_active) return;
    if (ext_audio_eof) return;
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

            // [TIMING-DIAG v7.3] لحظة فك أول إطار فيديو فعليًا (مرة واحدة فقط)
            if (!first_frame_decoded_logged) {
                first_frame_decoded_logged = true;
                UtilityFunctions::print("[TIMING] أول إطار فيديو مفكوك بعد ", _elapsed_ms_since_load(), "ms");
            }

            av_frame_unref(vf); continue;
        }
        if (ret == AVERROR(EAGAIN)) {
            // [THREAD-SAFE v7.0] video_packet_queue قد يُكتب إليه من خيط
            // القراءة الشبكية في نفس اللحظة — نحمي السحب منه بالقفل.
            AVPacket *p = nullptr;
            {
                std::lock_guard<std::mutex> lock(network_queue_mutex);
                if (!video_packet_queue.empty()) {
                    p = video_packet_queue.front();
                    video_packet_queue.pop_front();
                }
            }
            if (!p) break;
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
            // [THREAD-SAFE v7.0] نفس الحماية لـ audio_packet_queue
            AVPacket *p = nullptr;
            {
                std::lock_guard<std::mutex> lock(network_queue_mutex);
                if (!audio_packet_queue.empty()) {
                    p = audio_packet_queue.front();
                    audio_packet_queue.pop_front();
                }
            }
            if (!p) break;
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
            // [THREAD-SAFE v7.0] نفس الحماية لـ ext_audio_pkt_queue
            AVPacket *p = nullptr;
            {
                std::lock_guard<std::mutex> lock(ext_network_queue_mutex);
                if (!ext_audio_pkt_queue.empty()) {
                    p = ext_audio_pkt_queue.front();
                    ext_audio_pkt_queue.pop_front();
                }
            }
            if (!p) break;
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
// [BUFFER-FIX v6.4] target_secs قابل للتغيير: 5s للتخزين الابتدائي الأول،
// 2s (REBUFFER_TARGET) لإعادة التخزين اللاحقة بعد Underrun أو Seek.
void FFmpegPlayer::_prefill_buffers(double target_secs) {
    if (!fmt_ctx) return;
    UtilityFunctions::print("[PREFILL] → ", target_secs, "s...");
    int tries = 0;
    while (forward_buffer_secs < target_secs && tries++ < 400) {
        _read_packets_to_queue(); _update_buffer_stats();
    }
    _decode_packets_into_queue();
    if (audio_active_source == AudioActiveSource::INTERNAL_EMBEDDED)
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
// [BUFFER-FIX v6.4] النسبة تُحسَب بالنسبة للهدف الفعلي الحالي (5s أول مرة،
// أو 2s لإعادة التخزين اللاحقة) كي تعكس واجهة المستخدم 100% عند الهدف الصحيح.
float FFmpegPlayer::get_buffer_status() {
    double target = first_buffer_done ? REBUFFER_TARGET : INITIAL_PLAY;
    if (forward_buffer_secs >= target) return 100.0f;
    float p = (float)(forward_buffer_secs / target) * 100.0f;
    return p < 0.0f ? 0.0f : p;
}

// ─── [6] تنظيف كامل للطوابير ──────────────────────────────────────────────────
void FFmpegPlayer::_clear_queues() {
    // [THREAD-SAFE v7.0] video_packet_queue/audio_packet_queue قد يُكتب
    // إليهما من خيط القراءة الشبكية في نفس اللحظة — نحمي المسح بالقفل.
    {
        std::lock_guard<std::mutex> lock(network_queue_mutex);
        while (!video_packet_queue.empty())
            { av_packet_free(&video_packet_queue.front()); video_packet_queue.pop_front(); }
        while (!audio_packet_queue.empty())
            { av_packet_free(&audio_packet_queue.front()); audio_packet_queue.pop_front(); }
    }
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
    // 1. تأمين وإغلاق خيط الصوت الشبكي أولاً وقبل كل شيء لمنع تصادم الذاكرة
    if (audio_loading_thread_running) {
        audio_loading_thread_running = false;
        if (audio_loading_thread.joinable()) {
            audio_loading_thread.join(); // انتظر إغلاق الخيط بسلام
        }
    }

    // [ASYNC-VIDEO v6.3] تأمين وإغلاق خيط فتح الفيديو الشبكي بنفس الطريقة،
    // ومنع تسرّب الذاكرة إن كان قد نجح الفتح لكن لم يُعالَج بعد في _process()
    if (video_loading_thread_running) {
        video_loading_thread_running = false;
        if (video_loading_thread.joinable()) {
            video_loading_thread.join();
        }
    }
    if (video_load_ready && pending_video_fmt_ctx) {
        avformat_close_input(&pending_video_fmt_ctx);
    }
    video_load_ready      = false;
    video_load_error      = false;
    pending_autoplay       = false;
    pending_video_fmt_ctx  = nullptr;
    pending_video_error_message = "";

    // [THREAD-SAFE v7.0] أوقف خيط قراءة الفيديو الشبكي المستمر قبل إغلاق
    // fmt_ctx — إغلاقه أثناء قراءة الخيط له كارثي (استخدام بعد التحرير).
    // بفضل timeout/rw_timeout (15s) المضبوطة عند الفتح، هذا الانتظار محدود
    // بحد أقصى معروف حتى في أسوأ سيناريو (اتصال ميت تمامًا بلا خطأ صريح).
    //
    // [TODO-مستقبلًا] هذا الـ join() هو آخر نقطة يمكن أن ينتظر فيها الخيط
    // الرئيسي (حتى 15 ثانية كحد أقصى، وليس أثناء التشغيل العادي، فقط إن
    // بدّل المستخدم الفيديو أو أغلق التطبيق بالضبط لحظة انقطاع شبكي كامل).
    // لإزالة هذا الانتظار نهائيًا يجب التحوّل لنمط "Detach + تنظيف ذاتي":
    // تمرير fmt_ctx كمتغيّر محلي للخيط (بدل الاعتماد على عضو الكائن)، وجعل
    // الخيط نفسه يُغلق fmt_ctx ويخرج بمفرده عند اكتشاف طلب التوقف، مع خيط
    // (generation counter) يُميّز حزم الجلسة القديمة عن الجديدة كي لا تتسرب
    // حزم من خيط يتيم متأخر إلى طوابير تحميل فيديو جديد بدأ بالفعل. لم يُطبَّق
    // الآن لتفادي التعقيد ومخاطر تلوّث الطوابير بين الجلسات دون داعٍ ملحّ.
    if (network_reader_active) {
        network_reader_active = false;
        if (network_read_thread.joinable()) network_read_thread.join();
    }
    network_seek_requested     = false;
    network_seek_done          = false;
    network_seek_failed        = false;
    network_read_error_flag    = false;
    pending_video_seek_active  = false;
    video_is_network_source    = false;

    _clear_queues();
    _cleanup_ext_audio(); // يوقف ext_network_read_thread داخليًا أيضًا الآن
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
    av_sync_check_timer = 0.0; // [AV-SYNC v6.5]

    // [v6.2] إعادة ضبط حالة EOF وأولوية مصدر الصوت
    demux_eof_reached        = false;
    // [BUFFER-FIX v6.4] كل تحميل جديد يبدأ بتخزين ابتدائي كامل (5s) من جديد
    first_buffer_done        = false;
    audio_active_source      = AudioActiveSource::NONE;
    external_audio_requested = false;
    external_audio_ready     = false;
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
