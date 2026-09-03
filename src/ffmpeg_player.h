/**
 * ffmpeg_player.h
 * GDExtension — FFmpeg Video Player (Unified) for Godot 4 (Android ARM64/ARM32)
 *
 * الإصدار 7.4 — دعم ترويسة Referer لتفادي تأخير/حجب مضيفي الفيديو المحميين
 *
 * ─── الجديد في v7.4 ──────────────────────────────────────────────────────────
 *
 *  [REFERER] load_video(path, referer="") — معامل اختياري جديد. بعض مضيفي
 *        الفيديو (Streamtape وأمثاله) يطبّقون حماية مضادة للاستخدام المباشر
 *        (Anti-Hotlinking) تتحقق من ترويسة Referer، وقد تتعمّد تأخير
 *        الاستجابة (لا رفضها صراحة) إن غابت — إجراء شائع مضاد للبوتات. مرّر
 *        صفحة الفيديو الأصلية كـ Referer عند الحاجة لتفادي هذا التأخير.
 *
 * ─── الجديد في v7.3 ──────────────────────────────────────────────────────────
 *
 *  [TIMING-DIAG] لتحديد بدقة أين تُصرَف ثواني التأخير الملاحَظة على بعض
 *        الروابط (مثل خدمات مثل Streamtape التي تتضمن حل رابط عبر توكن)،
 *        أُضيفت طباعات زمنية نسبية لحظة استدعاء load_video() عند: نجاح فتح
 *        الاتصال، نجاح تحليل الصيغة، فك أول إطار فيديو، ولحظة الانطلاق
 *        الفعلية (صوت+صورة معًا). لا تُغيّر أي سلوك — تشخيص فقط.
 *
 * ─── الجديد في v7.2 ──────────────────────────────────────────────────────────
 *
 *  [STARTUP-FIX] INITIAL_PLAY كان 5.0 ثانية — انتظار إلزامي طويل قبل أي
 *        انطلاق، بصرف النظر عن جودة الشبكة الفعلية. هذا قرار تصميم صارم من
 *        v6.4، وليس محدودية جهاز أو فك تشفير كما افتُرض سابقًا: التطبيقات
 *        الاحترافية (يوتيوب، تيك توك) تنطلق بجزء من ثانية إلى ثانية واحدة
 *        فقط، معتمدة على إعادة التخزين التفاعلية عند الحاجة بدل انتظار مسبق
 *        ضخم يتحمّله الجميع دائمًا. أصبح الآن 1.0 ثانية فقط لانطلاق أسرع
 *        بكثير، مع بقاء REBUFFER_TARGET (2s) كما هو لإعادة التخزين بعد نضوب
 *        فعلي أثناء التشغيل (هامش أكبر قليلًا عن قصد، لأن النضوب دليل شبكة
 *        متعثرة بالفعل).
 *
 * ─── الجديد في v7.1 ──────────────────────────────────────────────────────────
 *
 *  [DECODE-GATE] كان الخروج من buffering يعتمد فقط على توفر بيانات كافية
 *        (forward_buffer_secs)، فيُطلَق الصوت فورًا حتى لو لم يُفكَّ أي إطار
 *        فيديو بعد — نمط شائع على الأجهزة/المحاكيات ذات فك التشفير البرمجي
 *        البطيء (لا تسريع عتاد MediaCodec فعلي): الصوت يبدأ ممتازًا، والشاشة
 *        تبقى سوداء لثوانٍ (شوهد حتى 7-8 ثوانٍ) حتى يلحق فك التشفير. الآن لا
 *        نخرج من buffering ولا نُشغِّل الصوت إلا بعد جهوزية إطار مفكوك واحد
 *        على الأقل فعليًا (decoded_frame_queue غير فارغ)، فينطلق الصوت
 *        والصورة معًا دائمًا مهما بلغت سرعة فك التشفير.
 *
 * ─── الجديد في v7.0 ──────────────────────────────────────────────────────────
 *
 *  [THREAD-SAFE-NETWORK] القراءة المستمرة من الشبكة (av_read_frame) والـ Seek
 *        (av_seek_frame) كانا يُنفَّذان على الخيط الرئيسي لكل من fmt_ctx
 *        (الفيديو) و ext_fmt_ctx (الصوت الخارجي الشبكي). إن تجمّد الاتصال
 *        (Socket مفتوح لكن بلا بيانات ولا خطأ) كان هذا يُجمّد محرك Godot
 *        بالكامل لمدة غير محدودة. الآن أصبح لكل منهما خيط خلفي مستقل تمامًا
 *        (_network_read_worker / _ext_network_read_worker) يملك سياقه حصريًا؛
 *        الخيط الرئيسي لا يستدعي أي دالة av_* عليهما إطلاقًا — فقط يقرأ من
 *        طوابير محمية بـ std::mutex يملؤها هذان الخيطان. عمليات الـ Seek على
 *        مصدر شبكي أصبحت طلبات ذرية (atomic) غير حاجبة يعالجها كل خيط بنفسه.
 *
 *  [TIMEOUT] أُضيف timeout/rw_timeout (15 ثانية) لفتح الفيديو الشبكي (كان
 *        موجودًا مسبقًا فقط لفتح الصوت الخارجي)، فيحدّ أقصى مدة انتظار لأي
 *        استدعاء قراءة منفرد حتى داخل الخيط الخلفي نفسه — يضمن ألا يُحتجَز
 *        أي خيط إلى الأبد على اتصال ميت تمامًا بلا خطأ صريح.
 *
 *  [محدودية معروفة — TODO مستقبلًا] عند إغلاق الخيوط الخلفية في _cleanup()
 *        (تبديل فيديو جديد أو إغلاق التطبيق)، ننتظرها بـ join() قبل تحرير
 *        fmt_ctx/ext_fmt_ctx. هذا الانتظار محدود بـ 15s كحد أقصى (بفضل
 *        TIMEOUT أعلاه)، لكنه قد يُحسّ به فقط إن حدث التبديل/الإغلاق بالضبط
 *        أثناء انقطاع شبكي كامل — وليس أثناء التشغيل العادي إطلاقًا. الحل
 *        الجذري لإزالته نهائيًا هو نمط "Detach + تنظيف ذاتي": يُمرَّر
 *        fmt_ctx كمتغيّر محلي لكل خيط (بدل الاعتماد على عضو الكائن)، ويُغلقه
 *        الخيط نفسه ويخرج بمفرده دون أن ينتظره الخيط الرئيسي، مع رقم جيل
 *        (generation counter) يُميّز حزم الجلسة القديمة عن الجديدة كي لا
 *        تتسرب حزم من خيط يتيم متأخر إلى طوابير جلسة تحميل جديدة بدأت
 *        بالفعل. أُجِّل هذا لتفادي التعقيد الإضافي ومخاطر تلوّث الطوابير بين
 *        الجلسات دون داعٍ ملحّ حاليًا.
 *
 * ─── الجديد في v6.5 ──────────────────────────────────────────────────────────
 *
 *  [AV-SYNC] مراقبة دورية (كل ثانية) لموضع الصوت الخارجي المحلي الحقيقي
 *        (AudioStreamPlayer::get_playback_position()) مقابل موضع الفيديو،
 *        مع تصحيح صامت للفروق الصغيرة وإصدار إشارة av_sync_issue للفروق
 *        الكبيرة الملحوظة. غير مطلوب للصوت المدمج/الشبكي لأن موضع الفيديو
 *        هناك مُشتَق حرفيًا من ساعة الصوت (لا انجراف ممكن أصلاً).
 *
 * ─── الجديد في v6.4 ──────────────────────────────────────────────────────────
 *
 *  [BUFFER-FIX] شرط الخروج من buffering كان: "forward_buffer_secs >= INITIAL_PLAY
 *        || !decoded_frame_queue.empty()". بما أن أول إطار يُفكّ خلال أول Tick
 *        تقريبًا، كان الشرط الثاني (OR) يُخرِج من التخزين فورًا بغض النظر عن
 *        حجم البيانات الفعلي المُخزَّن — فلا ينتظر أبدًا 5 ثوانٍ كاملة على شبكة
 *        ضعيفة، مما يسبب انطلاقًا فوريًا بجزء من الثانية ثم نضوبًا سريعًا وتكرار
 *        التقطّع. الآن الشرط يعتمد فقط على forward_buffer_secs (أو نفاد المصدر
 *        كليًا عبر EOF)، ويُستخدم هدف أقصر (REBUFFER_TARGET=2s) لإعادة التخزين
 *        بعد أول انطلاق ناجح (first_buffer_done) بدل إعادة انتظار 5s في كل مرة.
 *
 *  [STATUS-FIX] إشارة buffering_status (المغذّية لشريط التحميل في الواجهة)
 *        كانت لا تُطلَق إطلاقًا أثناء buffering=true بسبب "return" مبكر في
 *        _process() قبل الوصول لكود الإشارة. الآن تُطلَق دائمًا (أثناء التخزين
 *        وأثناء التشغيل العادي) فيعكس الشريط التقدّم الحقيقي أمام المستخدم.
 *
 * ─── الجديد في v6.3 ──────────────────────────────────────────────────────────
 *
 *  [ASYNC-VIDEO] فتح الفيديو الشبكي في خيط خلفي:
 *        avformat_open_input()/avformat_find_stream_info() لروابط الشبكة كانا
 *        يُنفَّذان بشكل متزامن على الخيط الرئيسي في load_video()، مما يُجمّد
 *        محرك Godot بالكامل لثوانٍ (تظهر كشاشة سوداء + توقف الواجهة). الآن يتم
 *        تنفيذهما في std::thread منفصل تمامًا كما هو الحال مع الصوت الشبكي،
 *        وتُسلَّم النتيجة إلى الخيط الرئيسي عبر أعلام atomic تُفحَص في _process().
 *
 *  [PROBE-SPEED] إضافة probesize=512KB و analyzeduration=2s لفتح الفيديو
 *        الشبكي (لم تكونا موجودتين سابقًا هنا رغم وجودهما في فتح الصوت
 *        الخارجي)، لتسريع avformat_find_stream_info() على الروابط الشبكية.
 *
 * ─── الجديد في v6.2 ──────────────────────────────────────────────────────────
 *
 *  [EOF] الإنهاء الحقيقي:
 *        video_finished الآن يعتمد على نفاد الديموكسر فعليًا (AVERROR_EOF)
 *        وتفريغ كل الطوابير، بدل مقارنة position >= duration التي كانت
 *        تفشل بسبب GPU_LATENCY_OFFSET السالب.
 *
 *  [AUDIO-FALLBACK] أولوية مصدر الصوت:
 *        يُشغَّل الصوت المدمج مع الفيديو تلقائيًا وفوريًا عند التشغيل (سواء
 *        كان الفيديو محليًا أو رابط شبكة). إذا استُدعيت load_audio() برابط
 *        صوت خارجي صالح، يتم التبديل إليه بسلاسة فور جاهزيته (بلا فجوة صمت)
 *        ويتوقف الصوت الداخلي. إذا فشل تحميل الصوت الخارجي أو لم يُطلب
 *        إطلاقًا، يستمر الصوت الداخلي المدمج تلقائيًا كخيار افتراضي/احتياطي.
 *        هذا يتيح تبديل اللغة (دبلجة) عبر رابط صوت خارجي دون رفع نسخ فيديو
 *        متكررة، مع بقاء الفيديو قابلاً للتشغيل بصوته الأصلي عند عدم وجود
 *        رابط دبلجة.
 *
 * ─── التحسينات السابقة (v5→v6) ──────────────────────────────────────────────
 *
 *  [6]  Audio Overrun Protection:
 *       decoded_audio_queue / ext_audio_frame_queue تُمسح بالكامل عند كل
 *       Seek أو تغيير مصدر. حجم الطابور محدود بـ MAX_AUDIO_FRAMES (Ring-Buffer).
 *
 *  [7]  Audio Auto-Recovery:
 *       إذا تجاوزت الفجوة الزمنية بين إطارين صوتيين متتاليين AUDIO_GAP_RESYNC_S
 *       يُمسح الكودك ويُعاد ضبط الساعة. إذا تجاوزت AUDIO_GAP_SKIP_S يُجرى
 *       av_seek_frame للنقطة الحالية دون تشغيل ما فاته.
 *
 *  [8]  Audio Interpolation & Soft Clipping:
 *       swr_ctx يستخدم Kaiser filter (β=9). بعد التحويل يُطبَّق Soft Clip
 *       على كل عينة لإزالة الطقطقة الناتجة عن Overflow.
 *
 *  [9]  Silence Filling:
 *       عند فراغ طابور الإطارات الصوتية يُضخّ 20ms من الصمت (zero-padding)
 *       للحفاظ على استقرار محرك AudioStreamGenerator في Godot.
 *
 *  [10] Smart Channel Mapping:
 *       Mono → يُنسخ للقناتين (L/R). 5.1/7.1 → Downmix تلقائي إلى Stereo
 *       عبر av_channel_layout وخوارزمية swr المُدمجة.
 *
 *  [11] Hard Frame Drop + GPU Latency Offset:
 *       إذا تأخر الفيديو عن الصوت > HARD_DROP_THRESHOLD (20ms)
 *       يُسقَط الإطار فوراً. GPU_LATENCY_OFFSET = -0.02s لتعويض تأخر العرض
 *       (يُستخدم فقط لمزامنة العرض البصري، وليس لاكتشاف نهاية التشغيل).
 *
 *  [12] إصلاح فتح روابط الصوت HTTP/HTTPS:
 *       خيارات avformat محسّنة: timeout/rw_timeout، إزالة reconnect_streamed،
 *       إضافة http_persistent=0، seekable=0 (وضع Streaming نقي).
 */

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_ogg_vorbis.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <list>
#include <vector>
#include <deque>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

namespace godot {

struct DecodedFrame {
    PackedByteArray data;
    double          pts;
};

class FFmpegPlayer : public Node {
    GDCLASS(FFmpegPlayer, Node)

public:
    FFmpegPlayer();
    ~FFmpegPlayer();

    // ─── Video API ────────────────────────────────────────────────────────────
    bool load_video(const String &path, const String &referer = "");
    void play();
    void pause();
    void stop();
    void seek(double seconds);

    bool   is_playing()              const;
    double get_duration()            const;
    double get_position()            const;
    int    get_video_width()         const;
    int    get_video_height()        const;
    double get_fps()                 const;
    Ref<ImageTexture> get_current_frame_texture() const;

    void  set_loop(bool enable);
    bool  get_loop() const;

    // ─── Audio API ────────────────────────────────────────────────────────────
    bool  load_audio(const String &path);
    void  unload_audio();
    void  set_audio_volume(float vol);
    float get_audio_volume()       const;
    void  set_audio_muted(bool muted);
    bool  is_audio_muted()         const;
    String get_loaded_audio_path() const;
    bool  is_using_external_audio() const { return use_external_audio; }

    // ─── Buffer / Status ──────────────────────────────────────────────────────
    double get_forward_buffer()  const { return forward_buffer_secs; }
    bool   is_buffering()        const { return buffering; }
    float  get_buffer_status();

    void _ready()               override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    // ── [A] سياق الملف الرئيسي ───────────────────────────────────────────────
    AVFormatContext *fmt_ctx          = nullptr;
    int              video_stream_idx = -1;
    int              audio_stream_idx = -1;

    AVCodecContext  *video_codec_ctx  = nullptr;
    SwsContext      *sws_ctx          = nullptr;
    int    video_width  = 0;
    int    video_height = 0;
    double fps          = 0.0;
    double duration     = 0.0;

    uint8_t          *frame_buffer   = nullptr;
    Ref<ImageTexture> current_texture;

    AVCodecContext  *audio_codec_ctx  = nullptr;
    SwrContext      *swr_ctx          = nullptr;
    int              audio_sample_rate = 0;
    int              audio_channels    = 0;

    // ── [B] سياق الصوت الخارجي عبر FFmpeg ───────────────────────────────────
    AVFormatContext *ext_fmt_ctx      = nullptr;
    AVCodecContext  *ext_audio_ctx    = nullptr;
    SwrContext      *ext_swr_ctx      = nullptr;
    int              ext_audio_stream = -1;
    std::atomic<bool> ext_audio_eof{false};

    // ── [C] طوابير الحزم الخام ───────────────────────────────────────────────
    // [THREAD-SAFE v7.0] هذه الطوابير أصبحت مشتركة بين الخيط الرئيسي (فك
    // التشفير) والخيوط الخلفية للقراءة الشبكية (الدفع) — محمية بالأقفال أدناه.
    std::list<AVPacket*>  video_packet_queue;
    std::list<AVPacket*>  audio_packet_queue;
    std::deque<AVPacket*> ext_audio_pkt_queue;
    std::mutex network_queue_mutex;     // يحمي video_packet_queue + audio_packet_queue
    std::mutex ext_network_queue_mutex; // يحمي ext_audio_pkt_queue

    // ── [D] طوابير الإطارات المُفكَّكة ──────────────────────────────────────
    std::deque<DecodedFrame> decoded_frame_queue;
    std::deque<AVFrame*>     decoded_audio_queue;
    std::deque<AVFrame*>     ext_audio_frame_queue;

    // خيوط المعالجة الخلفية والتحكم بالصوت الشبكي
    std::thread audio_loading_thread;
    std::atomic<bool> audio_loading_thread_running{false};
    std::atomic<bool> audio_load_finished_successfully{false};

    // ── [ASYNC-VIDEO v6.3] خيط فتح الفيديو الشبكي (يمنع تجميد المحرك) ───────
    // avformat_open_input()/avformat_find_stream_info() على روابط الشبكة قد
    // تستغرقان ثوانٍ؛ تنفيذهما هنا في خيط خلفي منفصل تمامًا عن الخيط الرئيسي.
    // لا يُلمَس أي كائن Godot (Node/Texture/Signal) من داخل هذا الخيط إطلاقًا؛
    // فقط AVFormatContext* خام وأعلام atomic تُسلَّم للخيط الرئيسي ليقرأها
    // ويكمل العمل (register signals, allocate buffers...) في _process().
    std::thread        video_loading_thread;
    std::atomic<bool>  video_loading_thread_running{false};
    std::atomic<bool>  video_load_ready{false};   // نجح الفتح، بانتظار المعالجة في _process
    std::atomic<bool>  video_load_error{false};   // فشل الفتح
    AVFormatContext   *pending_video_fmt_ctx = nullptr;
    String             pending_video_error_message;
    String             pending_video_path;
    bool               pending_video_is_live = false;
    bool               pending_autoplay      = false; // play() استُدعيت قبل اكتمال الفتح الشبكي

    // ── [THREAD-SAFE v7.0] قراءة الفيديو الشبكي المستمرة في خيط مستقل ────────
    // بعد نجاح الفتح، يصبح هذا الخيط المالك الحصري لـ fmt_ctx: لا يستدعي
    // الخيط الرئيسي أي av_read_frame/av_seek_frame عليه إطلاقًا بعد ذلك.
    // هذا يضمن عدم تجمّد الواجهة إطلاقًا مهما ساءت الشبكة أو حتى لو تجمّد
    // الاتصال بالكامل بدون خطأ صريح (timeout/rw_timeout يحدّان أيضًا أقصى
    // مدة انتظار لأي استدعاء منفرد داخل هذا الخيط نفسه).
    std::thread        network_read_thread;
    std::atomic<bool>  network_reader_active{false};
    bool               video_is_network_source = false; // يُضبط في load_video()

    // ── [TIMING-DIAG v7.3] قياس زمني تشخيصي — يطبع فقط، لا يغيّر أي سلوك ──────
    // يحدد بدقة أين تُصرَف ثواني التأخير: فتح الاتصال؟ تحليل الصيغة؟ أول
    // إطار مفكوك؟ لحظة الانطلاق الفعلية؟ بدل التخمين.
    std::chrono::steady_clock::time_point load_start_tp;
    bool first_frame_decoded_logged = false;
    bool first_playback_start_logged = false;
    double _elapsed_ms_since_load() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - load_start_tp).count();
    }

    std::atomic<bool>   network_seek_requested{false};
    std::atomic<double> network_seek_target_secs{0.0};
    std::atomic<bool>   network_seek_done{false};
    std::atomic<bool>   network_seek_failed{false};
    bool                pending_video_seek_active   = false; // ننتظر network_seek_done في _process()
    double              pending_video_seek_target   = 0.0;
    bool                pending_autoplay_after_seek = false;

    std::atomic<bool> network_read_error_flag{false};
    double            network_error_notify_timer = 0.0;
    static constexpr double NETWORK_ERROR_NOTIFY_INTERVAL = 5.0; // لا نُكرر التحذير أكثر من مرة كل 5 ثوانٍ
    static const int  NETWORK_READ_QUEUE_CAP_PACKETS = 600; // سقف أمان لمنع نمو ذاكرة غير محدود

    // ── [THREAD-SAFE v7.0] نفس الفلسفة، لكن للصوت الخارجي الشبكي (ext_fmt_ctx) ──
    std::thread        ext_network_read_thread;
    std::atomic<bool>  ext_network_reader_active{false};
    std::atomic<bool>   ext_network_seek_requested{false};
    std::atomic<double> ext_network_seek_target_secs{0.0};
    std::atomic<bool>  ext_network_read_error_flag{false};

    static const int MAX_DECODED_FRAMES = 8;
    static const int MAX_AUDIO_FRAMES   = 32;

    // ── [E] حالة التشغيل ─────────────────────────────────────────────────────
    bool   playing            = false;
    bool   looping            = false;
    bool   buffering          = false;
    bool   is_live_stream     = false;
    bool   use_external_audio = false;   // v6.2: يعكس الآن حالة التشغيل الفعلية
                                          // (true فقط بعد نجاح تفعيل مصدر خارجي)
    double position           = 0.0;
    double stream_start_time  = 0.0;
    double frame_timer        = 0.0;

    // ── [F] بافر الفيديو الأمامي ─────────────────────────────────────────────
    double forward_buffer_secs = 0.0;
    const double MAX_FORWARD   = 40.0;
    const double MIN_FORWARD   = 20.0;
    // [STARTUP-FIX v7.2] كان 5.0 — انتظار 5 ثوانٍ كاملة قبل أي انطلاق مهما
    // كانت الشبكة ممتازة. هذا قرار تصميم صارم منّا، وليس بطء جهاز أو فك
    // تشفير. الآن نبدأ بأقل قدر ممكن (1 ثانية) — تمامًا كما تفعل التطبيقات
    // الاحترافية (يوتيوب/تيك توك/ExoPlayer): انطلاق سريع، ثم الاعتماد على
    // آلية إعادة التخزين عند النضوب (REBUFFER_TARGET أدناه) للتعامل مع أي
    // تعثّر أثناء التشغيل بدل انتظار ضخم مسبق يتحمّله الجميع دائمًا.
    const double INITIAL_PLAY  = 1.0;

    // ── [BUFFER-FIX v6.4] هدف تخزين لإعادة التخزين بعد نضوب أثناء التشغيل ─────
    // أكبر قليلًا من INITIAL_PLAY عن قصد: بما أن نضوبًا حدث فعلًا (دليل على
    // شبكة متعثرة)، نمنح هامش أمان أكبر قليلًا هنا لتفادي نضوب فوري متكرر —
    // لكن يبقى أقصر بكثير من الـ 5 ثوانٍ القديمة.
    const double REBUFFER_TARGET = 2.0;
    bool first_buffer_done = false; // true بعد أول تخزين ابتدائي ناجح

    // ── [G] ساعة الصوت ───────────────────────────────────────────────────────
    int     godot_mix_rate       = 44100;
    int64_t audio_samples_pushed = 0;
    double  audio_clock_offset   = 0.0;
    bool    audio_clock_active   = false;

    double _get_audio_clock() const;

    // ── [H] AudioStreamGenerator ─────────────────────────────────────────────
    AudioStreamPlayer                 *int_audio_player  = nullptr;
    Ref<AudioStreamGenerator>          int_audio_generator;
    Ref<AudioStreamGeneratorPlayback>  int_audio_playback;

    AudioStreamPlayer *ext_audio_player    = nullptr;
    String             loaded_audio_path;
    bool               ext_using_godot_player = false;

    float audio_volume = 1.0f;
    bool  audio_muted  = false;

    // ── [I] ثوابت بافر الصوت ────────────────────────────────────────────────
    static constexpr double AUDIO_BUFFER_MAX_MS = 250.0;
    static constexpr double AUDIO_MIN_PUSH_MS   =  20.0;

    // ── [J] مؤقت إشارات الحالة ──────────────────────────────────────────────
    double status_timer = 0.0;
    static constexpr double STATUS_INTERVAL = 0.5;

    // ── [K] ثوابت الاستشعار الزمني (v6) ─────────────────────────────────────

    // [7] Audio Auto-Recovery
    double last_audio_pts       = -1.0;
    bool   audio_resync_needed  = false;
    static constexpr double AUDIO_GAP_RESYNC_S = 0.5;
    static constexpr double AUDIO_GAP_SKIP_S   = 1.0;

    // [11] Hard Frame Drop + GPU Latency
    static constexpr double HARD_DROP_THRESHOLD = 0.020;
    static constexpr double GPU_LATENCY_OFFSET  = -0.020;

    // ── [AV-SYNC v6.5] كشف/تصحيح انجراف الصوت الخارجي المحلي عن الفيديو ─────
    // يُطبَّق فقط عندما audio_active_source == EXTERNAL_LOCAL_FILE (ext_using_
    // godot_player) لأنه المسار الوحيد الذي لا يملك ساعة صوت داخلية دقيقة —
    // position يتقدّم بالزمن الفعلي (delta) بدل الاعتماد على عينات مضخوخة
    // فعليًا. الصوت المدمج والشبكي مُتزامنان بالتعريف (position = audio_clk).
    double av_sync_check_timer = 0.0;
    static constexpr double AV_SYNC_CHECK_INTERVAL   = 1.0;  // فحص كل ثانية
    static constexpr double AV_SYNC_RESYNC_THRESHOLD = 0.15; // فرق مقبول → تصحيح صامت
    static constexpr double AV_SYNC_WARNING_THRESHOLD= 0.75; // فرق ملحوظ → تحذير + تصحيح

    // ── [L] الإنهاء الحقيقي (EOF من الديموكسر، v6.2) ────────────────────────
    // true فقط عندما يُعيد av_read_frame على fmt_ctx الرئيسي القيمة AVERROR_EOF
    // فعليًا (نفاد البيانات من المصدر)، لا عند وصول position إلى duration.
    std::atomic<bool> demux_eof_reached{false};

    // ── [M] أولوية مصدر الصوت (v6.2) ─────────────────────────────────────────
    enum class AudioActiveSource {
        NONE,               // لا يوجد أي مسار صوت (فيديو بلا صوت إطلاقًا)
        INTERNAL_EMBEDDED,  // الصوت المدمج داخل حاوية الفيديو نفسها (افتراضي)
        EXTERNAL_STREAM,    // صوت خارجي عبر FFmpeg (رابط شبكة، عبر المولّد المشترك)
        EXTERNAL_LOCAL_FILE // صوت خارجي عبر مشغّل Godot المستقل (res:// / user:// / mp3 / ogg)
    };
    AudioActiveSource audio_active_source = AudioActiveSource::NONE;

    // true بمجرد استدعاء load_audio() بمسار غير فارغ (سواء نجح التحميل بعد أم لا)
    bool external_audio_requested = false;
    // true فقط بعد أن يصبح المصدر الخارجي جاهزًا فعليًا وتم التبديل إليه
    bool external_audio_ready     = false;

    // ── الدوال الداخلية ──────────────────────────────────────────────────────
    bool _setup_video_codec(AVStream *vstream);
    bool _setup_audio_codec(AVStream *astream, AVCodecContext *&ctx_out,
                            SwrContext *&swr_out, int &rate_out, int &ch_out);
    bool _open_audio_with_ffmpeg(const String &path);
    void _open_audio_async_worker(String path);
    void _cleanup_ext_audio();

    // [ASYNC-VIDEO v6.3] فتح الفيديو الشبكي في خيط خلفي + إتمام التحميل
    void _open_video_async_worker(String path, bool is_live, String referer);
    bool _finalize_loaded_video(AVFormatContext *opened_ctx, bool is_live);

    // [THREAD-SAFE v7.0] خيوط القراءة المستمرة الشبكية (فيديو + صوت خارجي)
    void _network_read_worker();
    void _ext_network_read_worker();
    void _trim_old_packets();

    void _read_packets_to_queue();
    void _read_ext_audio_packets();
    void _prefill_buffers(double target_secs); // [BUFFER-FIX v6.4] هدف قابل للتغيير
    void _update_buffer_stats();
    int  _calc_read_batch_size() const;

    void _decode_packets_into_queue();
    void _decode_audio_into_queue();
    void _decode_ext_audio_into_queue();

    // [8+9+10+11] ضخ الصوت
    void _push_audio_frames(std::deque<AVFrame*> &queue, SwrContext *swr, int src_rate);
    void _fill_silence(int frames_count);

    // [11] عرض الإطار
    bool _present_frame_at(double pos);

    // [7] التعافي التلقائي
    void _handle_audio_gap(double gap_secs);
    void _reset_last_audio_pts();

    // [AV-SYNC v6.5] كشف وتصحيح انجراف الصوت الخارجي المحلي عن الفيديو
    void _check_av_sync(double delta);

    void _apply_audio_volume();
    void _start_audio_at(double pos);
    void _stop_audio();
    void _pause_audio();
    void _resume_audio(double pos);
    void _reset_audio_clock(double pos);

    // [M] مساعدات أولوية مصدر الصوت (v6.2)
    void _flush_internal_audio_queues();
    void _switch_to_external_local_file(const Ref<AudioStream> &stream, const String &path);
    void _revert_to_internal_audio();

    void _allocate_buffers();
    void _clear_queues();
    void _cleanup();

    void _emit_video_loaded(bool success);
    void _emit_video_finished();
    void _emit_frame_updated();
    void _emit_playback_error(const String &message);
    void _emit_buffering_status();
};

} // namespace godot
