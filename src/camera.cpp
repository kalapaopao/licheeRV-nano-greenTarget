// =============================================================================
// camera —— 板端主程序（LicheeRV Nano / SG2002 + GC4653）
//
// 一条数据链，四个线程：
//
//   [采集线程] VI 取帧 80Hz
//        ├─ 抽点下采样 → 绿灯检测 → 多目标筛选 → 卡尔曼 → 角度      （green_detect / target_track）
//        ├─ 画检测框（写回 Y 平面，网页和录像才看得到）
//        ├─ 发布结果给串口线程（uart_out，50Hz 发给电控）
//        ├─ 每帧一行交给记录器（frame_log，起飞/落地时落盘）
//        └─ 把帧交给编码线程（只保留最新一帧，编码跟不上就丢）
//   [编码线程] 硬件 JPEG → 网页预览 + AVI 录像 + 视频侧车
//   [串口线程] 发目标帧；收电控 0x02 帧并回调记录器
//   [HTTP 线程] MJPEG 预览/抓图
//
// 改这里之前先读：docs/DIAGNOSTICS.md（日志与开关）、docs/UART_PROTOCOL_V2_DESIGN.md（协议）、
// docs/DATA_RECORDING.md（记录格式）。参数全部来自 config.yaml，经 start.sh 转成环境变量。
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <ctime>
#include <vector>
#include <deque>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <arpa/inet.h>
#include "avi_mjpeg.h"
#include "green_detect.h"
#include "target_track.h"
#include "uart_out.h"
#include "frame_timing.h"
#include "frame_log.h"
#include "diag.h"
#include "osd.h"

// ---- 全局对象与运行期开关 ----
static greendet::Detector g_detector;      // 多候选收集（不门控）
static gt::TargetTracker g_target;         // 多目标筛选 + 卡尔曼 + 状态机
static gt::UartOut g_uart;                 // 发给电控（独立线程）
static gt::FrameLog g_log;                 // 飞行数据记录（预录环形缓冲 + 触发落盘）
// UartOut 的接收回调：串口线程上下文，只格式化入队
static void mcu_log_sink(const gt::McuState& m, gt::MonoUs rx_us, void* user) {
    static_cast<gt::FrameLog*>(user)->on_mcu(m, rx_us);
}
// 给记录器取一份"当前电控状态"快照：从没收到过电控帧就返回 nullptr（记录器会写空值）
static const gt::McuState* mcu_snapshot(gt::McuState* buf) {
    if (!g_uart.mcu_snapshot(buf, nullptr, nullptr)) return nullptr;
    return buf;
}
static bool g_track_on = true;             // track_enable：关闭时退回旧的单目标行为
static int g_max_candidates = 8;
static unsigned long green_frames=0,green_hits=0,green_reports=0;
static double det_total_ms=0.0;
static unsigned long det_count=0;
static double samp_total_ms=0.0, track_total_ms=0.0;
static int g_detect_stride=1;
static unsigned long g_frame_idx=0;
static int g_sample_steps=2;   // 每块 n×n 采样点数（1..4）
static bool g_detect_on=true;  // detect_enable：false 时完全不跑检测
static bool g_osd_on=false;    // osd_enable：把电控状态烧进画面左上角
// exit_after_land：true = 一次飞行（一个会话）归档完就退出进程，不常驻待命
static bool g_exit_after_land=false;
// 视频预录窗口（毫秒），由 LOG_PREROLL_MS 决定；0 = 不做视频预录
static unsigned g_video_preroll_ms=3000;
static int g_osd_scale=1;      // 字号倍数（1..3）；字库本身已是 8×16，scale=1 就够看
// 电控 STATE 的短标签（OSD 只有 ASCII 字库）
static const char* mcu_state_txt(uint8_t s){
    if(s==gt::kMcuStateFlight)return "FLIGHT";
    if(s==gt::kMcuStateStop)return "STOP";
    return "STATIC";
}
static bool g_draw_test_box=false;
static unsigned long g_uart_sent_prev=0;
// ---- 帧时间戳（见 frame_timing.h）：帧号 + 采集时刻 + 原始 PTS ----
// capture_mono_us 是「程序拿到帧」的时刻，不是曝光时刻；u64PTS 板端实测单位是微秒
// （1280x720/80fps 时每帧差≈12500），但仍只记录、不参与任何计算。
static uint32_t g_capture_frame_id=0;          // 相机帧号：每成功取到一帧 +1
static gt::MonoUs g_last_track_capture_us=0;   // 上次真正跑 Tracker 那帧的采集时刻（dt 基准）
static gt::MonoUs g_last_pub_capture_us=0;     // 最近一次发布给串口那帧的采集时刻
static uint32_t g_last_pub_frame_id=0;         // 最近一次发布给串口那帧的帧号
static gt::MonoUs g_last_target_log_us=0;      // GREEN_TARGET 日志限频用
static double g_det_max_ms=0.0;                // 统计窗口内：抽样→发布 的最大耗时
static double g_proc_total_ms=0.0,g_proc_max_ms=0.0;  // 抽样→结果完成（不含画框/日志/发布）
static double g_lat_total_ms=0.0,g_lat_max_ms=0.0;    // 取帧→结果完成
static const char* target_state_name(gt::TargetState s){
    switch(s){
        case gt::TargetState::kConfirmed: return "confirmed";
        case gt::TargetState::kCoasting:  return "coasting";
        case gt::TargetState::kLost:      return "lost";
    }
    return "lost";
}
// NPU is intentionally not linked or implemented in this version.
extern "C" {
#include "sample_comm.h"
#include <cvi_sys.h>
#include "cvi_venc.h"
#include "cvi_isp.h"
#include "cvi_ae.h"
}

// VI 的帧缓冲来自固定 VB 池，物理地址会反复出现；
// 每帧都 Mmap 会造成巨大开销（实测 231ms/帧），这里缓存映射结果。
struct VaCache { CVI_U64 phy; unsigned len; unsigned char* va; };
static VaCache g_va[16]; static int g_va_n=0; static unsigned long g_map_miss=0;
// 必须用缓存映射：未缓存的 CMA 每次字节访问都走总线，实测 223ms/帧。
static unsigned char* map_cached(CVI_U64 phy, unsigned len){
    for(int i=0;i<g_va_n;++i)
        if(g_va[i].phy==phy && g_va[i].len==len) return g_va[i].va;
    unsigned char* v=(unsigned char*)CVI_SYS_MmapCache(phy,len);
    if(v && g_va_n<16){ g_va[g_va_n].phy=phy; g_va[g_va_n].len=len; g_va[g_va_n].va=v; ++g_va_n; }
    ++g_map_miss;
    return v;
}
static bool env_u32(const char* name,unsigned fallback,unsigned low,unsigned high,unsigned& out){
    const char* s=getenv(name);if(!s){out=fallback;return true;}
    if(!*s)return false;
    errno=0;char* end=nullptr;unsigned long v=strtoul(s,&end,10);
    if(errno || *end || v<low || v>high)return false;
    out=static_cast<unsigned>(v);return true;
}
// 浮点环境变量：缺失时用 fallback，非法值返回 false（调用方决定是否致命）
static bool env_f32(const char* name,float fallback,float low,float high,float& out){
    const char* s=getenv(name);if(!s){out=fallback;return true;}
    if(!*s)return false;
    errno=0;char* end=nullptr;double v=strtod(s,&end);
    if(errno || *end || v<low || v>high)return false;
    out=static_cast<float>(v);return true;
}
// 布尔环境变量：接受 0/1/true/false（大小写不敏感）
static bool env_flag(const char* name,bool fallback,bool& out){
    const char* s=getenv(name);if(!s){out=fallback;return true;}
    if(!*s)return false;
    if(!strcmp(s,"1")||!strcasecmp(s,"true")||!strcasecmp(s,"on")||!strcasecmp(s,"yes")){out=true;return true;}
    if(!strcmp(s,"0")||!strcasecmp(s,"false")||!strcasecmp(s,"off")||!strcasecmp(s,"no")){out=false;return true;}
    return false;
}
static std::atomic<int> running(1);
static_assert(ATOMIC_INT_LOCK_FREE==2,"Signal handler requires lock-free atomic int");
static std::mutex jpeg_mutex;
static std::vector<unsigned char> latest;
// 当前打开的 AVI 归属：非空 = 正在录某次飞行（值是会话目录）；空 = 连续录像或没在录
static std::string avi_owner;
static unsigned long generation=0;
static std::mutex input_mutex;
static VIDEO_FRAME_INFO_S pending{};
static bool has_pending=false;
// 与 pending 配对：编码线程拿不到 FrameTiming，只能靠这两项写视频侧车
static uint32_t pending_frame_id=0;
static gt::MonoUs pending_capture_us=0;
static double g_last_health_s=-1.0;   // health.csv 1 Hz 限频
static std::atomic<unsigned long> encoded(0),errors(0);
static std::atomic<unsigned long> network_sent(0),network_bytes(0),preview_replaced(0);
static bool recording_enabled=false;
// display_enable: false 时不画叠加、不起编码线程（实战模式，省电降温）
static bool g_display_on=true;
// http_enable: false 时不起网页服务（只用来省 CPU；录像/编码不受影响）
static bool g_http_on=true;
// 需要「编码并写录像」吗：开预览或开录像任一为真
static bool g_encode_on=true;
static std::string recording_dir;
static std::atomic<int> recording_failed(0);
static AviMjpegWriter avi;
// Future: board-side native detection code will be added here; current code performs capture only.
// 被哪个信号停下的：写进 run_history 的 exit 行，用来区分"正常运行完"和"被 stop.sh 停掉"
static volatile sig_atomic_t g_stop_signal=0;
static void stop(int sig){g_stop_signal=sig;running=0;}
static double now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static bool sendall(int fd,const void* ptr,size_t n){
    const char* p=static_cast<const char*>(ptr);
    while(running && n){ssize_t r=send(fd,p,n,MSG_NOSIGNAL);if(r>0){p+=r;n-=r;}else if(r<0 && errno==EINTR)continue;else return false;}
    return !n;
}
// Handle each browser connection independently. A long-lived MJPEG client must never
// block another browser tab, the HTML page, or the snapshot endpoint.
static void http_client(int fd){
    timeval timeout{1,0};
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    int sndbuf=128*1024;
    setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof(sndbuf));
    char request[4096]{};
    ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    const bool snapshot=strncmp(request,"GET /snapshot.jpg ",18)==0;
    const bool stream=strncmp(request,"GET /live.mjpg ",15)==0;
    if(!snapshot && !stream){
        const char page[]=R"PAGE(<!doctype html><html lang="zh"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LicheeRV Nano | GC4653 camera</title>
<style>body{margin:0;background:#111923;color:#eef5ff;font:16px sans-serif}main{max-width:1280px;margin:auto;padding:16px}h2{font-size:20px}img{width:100%;height:auto;background:#000}small{color:#aebeca}</style>
<main><h2>GC4653 · 1280 × 720 · 80 FPS 采集</h2>
<img src="/live.mjpg" alt="摄像头实时画面">
<p><small>曝光和增益由 config.yaml 控制。原始图传，没有模型识别框。80 FPS 是摄像头采集速率，不代表网页帧率。</small></p></main></html>)PAGE";
        const char header[]="HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        sendall(fd,header,sizeof(header)-1);
        sendall(fd,page,sizeof(page)-1);
        close(fd);
        return;
    }
    if(stream){
        const char hdr[]="HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store, no-cache\r\nPragma: no-cache\r\nConnection: close\r\n\r\n";
        if(!sendall(fd,hdr,sizeof(hdr)-1)){
            close(fd);
            return;
        }
    }
    unsigned long seen=0;
    while(running){
        std::vector<unsigned char> jpg;
        {
            std::lock_guard<std::mutex> guard(jpeg_mutex);
            if(generation!=seen){
                seen=generation;
                jpg=latest;
            }
        }
        if(jpg.empty()){
            usleep(5000);
            continue;
        }
        char hdr[256];
        int count=snprintf(hdr,sizeof(hdr),snapshot?
            "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n":
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",jpg.size());
        if(count<=0 || !sendall(fd,hdr,static_cast<size_t>(count)) || !sendall(fd,jpg.data(),jpg.size()))break;
        ++network_sent;
        network_bytes+=jpg.size();
        if(snapshot)break;
        if(!sendall(fd,"\r\n",2))break;
        // Limit *network preview* to about 20 FPS per client; the sensor and
        // hardware JPEG encoder continue independently at their native 80 FPS.
        usleep(50000);
    }
    close(fd);
}
static void http_server(int server){
    struct ClientSlot { std::thread worker; std::atomic<bool> active{false}; };
    ClientSlot slots[4];
    while(running){
        pollfd p{server,POLLIN,0};
        if(poll(&p,1,100)<=0)continue;
        int fd=accept(server,nullptr,nullptr);
        if(fd<0)continue;
        ClientSlot* slot=nullptr;
        for(auto& candidate:slots){
            if(!candidate.active.load()){
                if(candidate.worker.joinable())candidate.worker.join();
                slot=&candidate;break;
            }
        }
        if(!slot){
            const char busy[]="HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendall(fd,busy,sizeof(busy)-1);
            close(fd);
            continue;
        }
        slot->active=true;
        slot->worker=std::thread([fd,slot](){http_client(fd);slot->active=false;});
    }
    close(server);
    for(auto& slot:slots)if(slot.worker.joinable())slot.worker.join();
}
static void encoder_worker(){
    unsigned long avi_index=0;   // AVI 内第几帧（0 起），与 CSV 的 frame_id 不是一回事
    // ---- 视频预录环形缓冲（trigger 模式）------------------------------------
    // 会话是在 STATE 0→1 触发时才打开的，AVI 也在那时才创建 —— 所以视频天然
    // 从"触发那一刻"开始，起飞前的画面全丢。而 frames.csv 有 preroll 回滚，
    // 于是出现「数据有预录、视频没有」的不一致（实测：CSV 第 0 帧 t=0，
    // 视频第 0 帧 t=2.99s，正好是触发时刻）。
    // 这里把最近 log_preroll_ms 内的编码帧留在内存，会话一打开就先把它们写进 AVI。
    struct PreRollFrame { std::vector<unsigned char> jpg; uint32_t fid; gt::MonoUs cap_us; uint64_t pts; };
    std::deque<PreRollFrame> preroll_ring;
    size_t preroll_bytes=0;
    const size_t kPreRollByteCap=20u*1024*1024;   // 硬上限 20MB：板子 MemAvailable 约 79MB
    // 每帧最多保留 preroll_ms 内的帧；帧率上限按 100fps 估，防止配置写大时爆内存
    auto preroll_trim=[&](gt::MonoUs now_us){
        const gt::MonoUs cutoff=now_us-(gt::MonoUs)g_video_preroll_ms*1000;
        const size_t frame_cap=(size_t)(g_video_preroll_ms/1000)*100+4;
        while(!preroll_ring.empty() &&
              (preroll_ring.front().cap_us<cutoff || preroll_bytes>kPreRollByteCap ||
               preroll_ring.size()>frame_cap)){
            preroll_bytes-=preroll_ring.front().jpg.size();
            preroll_ring.pop_front();
        }
    };
    while(running){
        // 从主线程拿"最新一帧"（拿不到就等 1ms），同时取出该帧的帧号/采集时刻
        VIDEO_FRAME_INFO_S frame{};
        bool got=false;
        uint32_t fid=0;
        gt::MonoUs cap_us=0;
        {
            std::lock_guard<std::mutex> guard(input_mutex);
            if(has_pending){
                frame=pending;
                fid=pending_frame_id;
                cap_us=pending_capture_us;
                has_pending=false;
                got=true;
            }
        }
        if(!got){
            usleep(1000);
            continue;
        }

        VENC_RECV_PIC_PARAM_S receive{};
        receive.s32RecvPicNum=1;
        int e=CVI_VENC_StartRecvFrame(0,&receive);
        if(!e)e=CVI_VENC_SendFrame(0,&frame,1000);
        if(!e){
            VENC_PACK_S packets[64]{};
            VENC_STREAM_S stream{};
            stream.pstPack=packets;
            stream.u32PackCount=64;
            e=CVI_VENC_GetStream(0,&stream,1000);
            if(!e){
                std::vector<unsigned char> jpg;
                for(unsigned i=0;i<stream.u32PackCount;i++){
                    const auto& p=packets[i];
                    if(p.pu8Addr && p.u32Len>=p.u32Offset)jpg.insert(jpg.end(),p.pu8Addr+p.u32Offset,p.pu8Addr+p.u32Len);
                }
                CVI_VENC_ReleaseStream(0,&stream);
                if(jpg.size()<4 || jpg[0]!=0xff || jpg[1]!=0xd8)e=-1;
                else{
                    // Persist the same hardware JPEG frame sent to the browser: no second VI/VENC.
                    if(recording_enabled){
                        // 录像归属：记录开着且是触发式 → 本次飞行的视频写进「会话目录/video.avi」，
                        // 和数据（frames.csv/meta.txt）在同一个文件夹里；
                        // 否则退回原来的连续录像（recordings/ 下一个大文件），供没开记录时调试用。
                        std::string sdir;
                        const bool in_sess=g_log.running() && g_log.session_video_dir(&sdir);
                        bool flush_preroll=false;
                        if(in_sess){
                            if(avi_owner!=sdir){
                                if(avi.is_open() && !avi.finish())recording_failed=1;
                                if(!avi.open_named(sdir,"video")){
                                    recording_failed=1; running=0;
                                }else{
                                    avi_owner=sdir;
                                    avi_index=0;      // 每段视频的帧号从 0 开始
                                    flush_preroll=true;   // 新会话：先补预录帧
                                }
                            }
                        }else if(!avi_owner.empty()){
                            // 会话刚刚结束：把这一段 AVI 收尾
                            if(!avi.finish())recording_failed=1;
                            avi_owner.clear();
                            avi_index=0;
                        }else if(!avi.is_open() && (!g_log.running() || !g_log.trigger_mode())){
                            // 没开记录、或连续记录模式：保持全程一个文件
                            if(!avi.open_named(recording_dir,"")){
                                recording_failed=1; running=0;
                            }
                        }
                        // 新会话刚开：把触发前攒下的帧按时间顺序先写进 AVI，
                        // 这样视频也和 frames.csv 一样带 preroll，起飞瞬间不会缺头。
                        if(flush_preroll && avi.is_open()){
                            const size_t n=preroll_ring.size();
                            // 打点耗时：这段同步写入会独占编码线程，期间实时帧被覆盖丢弃。
                            // 长短取决于要写的字节数和当时页缓存压力（实测 0.19s ~ 2.55s）。
                            const gt::MonoUs fl_t0=gt::monotonic_us();
                            size_t fl_bytes=0;
                            for(auto& pf:preroll_ring){
                                fl_bytes+=pf.jpg.size();
                                if(!avi.write(recording_dir,pf.jpg.data(),pf.jpg.size(),pf.pts)){
                                    recording_failed=1; running=0; break;
                                }
                                g_log.on_video(avi_index++,pf.fid,pf.cap_us,pf.pts);
                            }
                            preroll_ring.clear();
                            preroll_bytes=0;
                            DIAGF(gt::kDiagLog,"VIDEO_PREROLL flushed=%zu 帧 %.2fMB 耗时=%.0fms\n",
                                  n,fl_bytes/1048576.0,(gt::monotonic_us()-fl_t0)/1000.0);
                        }
                        if(avi.is_open()){
                            if(!avi.write(recording_dir,jpg.data(),jpg.size(),frame.stVFrame.u64PTS)){
                                recording_failed=1;
                                running=0;
                            }else{
                                g_log.on_video(avi_index++,fid,cap_us,frame.stVFrame.u64PTS);
                            }
                        }else if(g_video_preroll_ms>0 && g_log.running() && g_log.trigger_mode()){
                            // 触发式记录、还没到触发点：把这一帧留在环形缓冲里等起飞
                            preroll_bytes+=jpg.size();
                            preroll_ring.push_back(PreRollFrame{
                                std::vector<unsigned char>(jpg.begin(),jpg.end()),fid,cap_us,
                                frame.stVFrame.u64PTS});
                            preroll_trim(cap_us);
                        }
                    }
                    {
                        std::lock_guard<std::mutex> guard(jpeg_mutex);
                        latest.swap(jpg);
                        ++generation;
                    }
                    ++encoded;
                }
            }
        }
        CVI_VENC_StopRecvFrame(0);
        CVI_VI_ReleaseChnFrame(0,0,&frame);
        if(e){
            ++errors;
            fprintf(stderr,"VENC encode rc=0x%x\n",e);
            if(errors>=5)running=0;
        }
    }
    std::lock_guard<std::mutex> guard(input_mutex);
    if(has_pending){
        CVI_VI_ReleaseChnFrame(0,0,&pending);
        has_pending=false;
    }
}
int main(int argc,char** argv){
    // ======================================================================
    // 0. 命令行：正常运行时由 start.sh 拉起，参数是  USB_BIND_IP OUTPUT_DIR [80] [--recording-internal]
    // ======================================================================
    if(argc==2 && (!strcmp(argv[1],"--recoding") || !strcmp(argv[1],"--recording"))){
        execl("/bin/sh","sh","/code/fly_LicheeRV/start.sh","--recoding",static_cast<char*>(nullptr));
        perror("Cannot launch recording supervisor");
        return 2;
    }
    if(argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))){
        printf("Usage: ./camera --recoding (alias --recording)\n"
               "       sh start.sh [--recoding|seconds] (default preview 120 seconds)\n");
        return 0;
    }
    if((argc!=3 && argc!=4 && argc!=5) ||
       (argc==5 && strcmp(argv[4],"--recording-internal"))){
        fprintf(stderr,"Usage: %s USB_BIND_IP OUTPUT_DIR [80] [--recording-internal]\n",argv[0]);
        return 2;
    }
    recording_enabled=argc==5;
    if(recording_enabled)recording_dir=argv[2];

    // 孤儿保护：start.sh（监督进程）一旦死掉，内核给我发 SIGTERM，跟着优雅退出。
    // 否则会出现"监督进程没了、相机还在满负荷跑"——没人看温度、没人重启，最危险的状态。
    prctl(PR_SET_PDEATHSIG,SIGTERM);
    if(getppid()==1){
        fprintf(stderr,"parent already gone, refusing to run unsupervised\n");
        return 3;
    }
    if(argc>=4 && strcmp(argv[3],"80"))return 2;

    // 几个不经过 start.sh 校验的调试开关（直接跑二进制时才用到）
    const char* disp=getenv("DISPLAY_ENABLE");
    if(disp && !strcmp(disp,"false"))g_display_on=false;
    const char* http_env=getenv("HTTP_ENABLE");
    if(http_env && !strcmp(http_env,"false"))g_http_on=false;
    // 只要开预览或开录像，就得建 VENC 通道并起编码线程（网页可以单独关掉）
    g_encode_on = g_display_on || recording_enabled;
    if(getenv("DETECT_STRIDE")){
        int v=atoi(getenv("DETECT_STRIDE"));
        if(v>=1&&v<=10)g_detect_stride=v;
    }
    if(getenv("DETECT_SAMPLE_STEPS")){
        int v=atoi(getenv("DETECT_SAMPLE_STEPS"));
        if(v>=1&&v<=4)g_sample_steps=v;
    }
    if(getenv("DETECT_ENABLE") && !strcmp(getenv("DETECT_ENABLE"),"false"))g_detect_on=false;
    if(getenv("DRAW_TEST_BOX") && !strcmp(getenv("DRAW_TEST_BOX"),"1"))g_draw_test_box=true;
    if(getenv("OSD_ENABLE") && !strcmp(getenv("OSD_ENABLE"),"true"))g_osd_on=true;
    if(getenv("EXIT_AFTER_LAND") && !strcmp(getenv("EXIT_AFTER_LAND"),"true"))g_exit_after_land=true;
    if(getenv("OSD_SCALE")){
        int v=atoi(getenv("OSD_SCALE"));
        if(v>=1&&v<=3)g_osd_scale=v;
    }

    // ======================================================================
    // 1. 参数：全部来自 config.yaml（start.sh 解析后转成环境变量）。
    //    非法值一律报错退出，绝不"悄悄用默认值"——联调时算错却没人发现最要命。
    // ======================================================================
    bool track_on=true, uart_on=false, inv_yaw=false, inv_pitch=false;
    float cam_fx=1024.0f, cam_fy=1024.0f, cam_cx=640.0f, cam_cy=360.0f;
    // 宽度裁剪（0 = 不裁）。用于把保护盖造成的左右黑边裁掉，减小录像体积。
    unsigned cam_crop_w=0;
    float opt_yaw=0.0f, opt_pitch=0.0f, target_dia=0.05f;
    float aim_up=1.5f;   // 瞄准点向上偏移系数（= 装甲板中心到灯中心距离 / 灯直径），抄 HERO 的 1.5
    float sel_conf=0.5f, sel_aspect=1.5f, sel_maxdist=60.0f;
    float kf_q=50.0f, kf_r=0.001f, kf_initpos=10.0f, kf_initvel=2.0f;
    unsigned sel_confirm=5, sel_lost=10, sel_maxcand=8;
    unsigned uart_baud=115200, uart_rate=50;

    const char* uart_dev=getenv("UART_DEVICE");
    if(!uart_dev||!*uart_dev)uart_dev="/dev/ttyS1";

    // 串口帧格式：ascii = v1 文本帧（$GT,...，串口助手可直接看）；
    //             binary = v2 定长二进制帧（28 字节 A5 5A ... CRC16，见 uart_frame.h）
    // 默认 ascii，保证不配置时线上字节行为与之前完全一致。
    const char* uart_format=getenv("UART_FORMAT");
    if(!uart_format||!*uart_format)uart_format="ascii";
    const bool uart_binary = !strcmp(uart_format,"binary");
    if(!uart_binary && strcmp(uart_format,"ascii")){
        fprintf(stderr,"Invalid UART_FORMAT '%s' (expect ascii|binary)\n",uart_format);
        return 2;
    }
    // 接收方向（电控 → 视觉 CMD=0x02）：默认开，收到就把电控信息打到终端；只打印不处理。
    bool uart_rx_print=true;
    unsigned uart_rx_hz=10;
    {
        const char* rp=getenv("UART_RX_PRINT");
        if(rp&&*rp){
            if(!strcmp(rp,"false"))uart_rx_print=false;
            else if(strcmp(rp,"true")){
                fprintf(stderr,"Invalid UART_RX_PRINT '%s' (expect true|false)\n",rp);
                return 2;
            }
        }
        const char* rh=getenv("UART_RX_PRINT_HZ");
        if(rh&&*rh){
            const int v=atoi(rh);
            if(v<1||v>100){
                fprintf(stderr,"Invalid UART_RX_PRINT_HZ '%s' (expect 1..100)\n",rh);
                return 2;
            }
            uart_rx_hz=static_cast<unsigned>(v);
        }
    }

    bool ok =
        env_flag("TRACK_ENABLE",true,track_on) &&
        env_flag("UART_ENABLE",false,uart_on) &&
        env_flag("CAM_INVERT_YAW",false,inv_yaw) &&
        env_flag("CAM_INVERT_PITCH",false,inv_pitch) &&
        env_f32("CAM_FX",1024.0f,1.0f,20000.0f,cam_fx) &&
        env_f32("CAM_FY",1024.0f,1.0f,20000.0f,cam_fy) &&
        env_f32("CAM_CX",640.0f,-10000.0f,20000.0f,cam_cx) &&
       !env_u32("CAM_CROP_WIDTH",0,0,1280,cam_crop_w) ||
        env_f32("CAM_CY",360.0f,-10000.0f,20000.0f,cam_cy) &&
        env_f32("OPTICAL_YAW_DEG",0.0f,-90.0f,90.0f,opt_yaw) &&
        env_f32("OPTICAL_PITCH_DEG",0.0f,-90.0f,90.0f,opt_pitch) &&
        env_f32("TARGET_DIAMETER_M",0.05f,0.001f,10.0f,target_dia) &&
        env_f32("TARGET_UP_OFFSET_FACTOR",1.5f,0.0f,5.0f,aim_up) &&
        env_f32("SEL_CONF_WEIGHT",0.5f,0.0f,1.0f,sel_conf) &&
        env_f32("SEL_MAX_ASPECT",1.5f,1.0f,10.0f,sel_aspect) &&
        env_f32("SEL_MAX_DIST_DEG",60.0f,1.0f,180.0f,sel_maxdist) &&
        env_f32("KF_PROCESS_NOISE",50.0f,0.0f,1e6f,kf_q) &&
        env_f32("KF_MEASURE_NOISE",0.001f,1e-9f,1e3f,kf_r) &&
        env_f32("KF_INIT_POS_VAR",10.0f,1e-9f,1e6f,kf_initpos) &&
        env_f32("KF_INIT_VEL_VAR",2.0f,1e-9f,1e6f,kf_initvel) &&
        env_u32("SEL_CONFIRM_FRAMES",5,1,1000,sel_confirm) &&
        env_u32("SEL_LOST_FRAMES",10,1,1000,sel_lost) &&
        env_u32("SEL_MAX_CANDIDATES",8,1,32,sel_maxcand) &&
        env_u32("UART_BAUD",115200,1200,4000000,uart_baud) &&
        env_u32("UART_RATE_HZ",50,1,200,uart_rate);
    if(!ok){
        fprintf(stderr,"Invalid track/uart parameter in environment (check config.yaml)\n");
        return 2;
    }
    g_track_on=track_on;
    g_max_candidates=static_cast<int>(sel_maxcand);

    {
        gt::CameraCalib calib;
        calib.fx=cam_fx; calib.fy=cam_fy; calib.cx=cam_cx; calib.cy=cam_cy;
        calib.optical_yaw_deg=opt_yaw; calib.optical_pitch_deg=opt_pitch;
        calib.invert_yaw=inv_yaw; calib.invert_pitch=inv_pitch;
        // 瞄准点向上偏移：瞄准点 = 灯中心上方 aim_up × 灯直径（打装甲板，不是打灯）
        calib.aim_up_offset_factor=aim_up;
        gt::KalmanCfg kf;
        kf.process_noise=kf_q; kf.measure_noise=kf_r;
        kf.init_pos_var=kf_initpos; kf.init_vel_var=kf_initvel;
        gt::SelectCfg sel;
        sel.confirm_frames=static_cast<int>(sel_confirm);
        sel.lost_frames=static_cast<int>(sel_lost);
        sel.conf_weight=sel_conf; sel.max_aspect=sel_aspect;
        sel.max_candidates=static_cast<int>(sel_maxcand);
        sel.max_dist_deg=sel_maxdist;
        g_target.configure(calib,kf,sel,target_dia);
        DIAGF(gt::kDiagBoot, "TRACK calib fx=%.2f fy=%.2f cx=%.2f cy=%.2f opt=%.2f/%.2f inv=%d/%d "
               "confirm=%u lost=%u conf_w=%.2f aspect=%.2f dia=%.3fm aimup=%.2f\n",
               cam_fx,cam_fy,cam_cx,cam_cy,opt_yaw,opt_pitch,(int)inv_yaw,(int)inv_pitch,
               sel_confirm,sel_lost,sel_conf,sel_aspect,target_dia,aim_up);
    }

    // ======================================================================
    // 2. 飞行数据记录（T+ 预录触发）
    //    触发源是电控 CMD=0x02 的 STATE 边沿（STATIC→FLIGHT 起飞 / FLIGHT→* 落地）。
    //    视觉侧没有 IMU，所以 trigger 模式必须打开 uart_enable（只收不发也行）。
    // ======================================================================
    bool log_on=false, log_trigger=true, uart_tx=true, log_mcu_stream=false;
    // log_video_preroll 与 log_preroll 分开：数据预录（回滚 CSV 行）几乎不花时间，
    // 但视频预录要在触发瞬间把几十 MB 的 JPEG 一次性写进 SD 卡，
    // 会独占编码线程数秒、把起飞后的实时帧全部覆盖丢弃（实测 1.15~2.55s 空档）。
    // 所以视频预录默认关闭（0），要它的人显式打开。
    unsigned log_preroll=3000, log_video_preroll=0, log_postroll=2000, log_maxsec=30;
    const char* log_dir=getenv("LOG_DIR"); if(!log_dir||!*log_dir)log_dir="logs";
    const char* log_mode=getenv("LOG_MODE"); if(!log_mode||!*log_mode)log_mode="trigger";
    if(strcmp(log_mode,"trigger") && strcmp(log_mode,"continuous")){
        fprintf(stderr,"Invalid LOG_MODE '%s' (expect trigger|continuous)\n",log_mode);
        return 2;
    }
    log_trigger = !strcmp(log_mode,"trigger");
    if(!env_flag("LOG_ENABLE",false,log_on) ||
       !env_flag("UART_TX_ENABLE",true,uart_tx) ||
       !env_flag("LOG_MCU_STREAM",false,log_mcu_stream) ||
       !env_u32("LOG_PREROLL_MS",3000,0,10000,log_preroll) ||
       !env_u32("VIDEO_PREROLL_MS",0,0,10000,log_video_preroll) ||
       !env_u32("LOG_POSTROLL_MS",2000,0,10000,log_postroll) ||
       !env_u32("LOG_MAX_SECONDS",30,1,300,log_maxsec)){
        fprintf(stderr,"Invalid log parameter in environment (check config.yaml)\n");
        return 2;
    }
    if(log_on && log_trigger && !uart_on){
        fprintf(stderr,"log_mode=trigger requires uart_enable=true (needs MCU STATE edge); "
                       "use log_mode=continuous to record without the MCU link\n");
        return 2;
    }

    // ======================================================================
    // 3. 诊断：分类打印开关 + 运行状态历史（自启动时唯一的"事后可见"通道）
    // ======================================================================
    {
        bool master=true;
        bool b_boot=true,b_isp=true,b_target=true,b_perf=true,b_uart=true,b_mcu=true,b_log=true,b_avi=true;
        if(!env_flag("PRINT_ENABLE",true,master) ||
           !env_flag("PRINT_BOOT",true,b_boot) ||
           !env_flag("PRINT_ISP",true,b_isp) ||
           !env_flag("PRINT_TARGET",true,b_target) ||
           !env_flag("PRINT_PERF",true,b_perf) ||
           !env_flag("PRINT_UART",true,b_uart) ||
           !env_flag("PRINT_MCU",true,b_mcu) ||
           !env_flag("PRINT_LOG",true,b_log) ||
           !env_flag("PRINT_AVI",true,b_avi)){
            fprintf(stderr,"Invalid print_* parameter in environment (expect true|false)\n");
            return 2;
        }
        unsigned mask=0;
        if(master){
            if(b_boot)mask|=gt::kDiagBoot;
            if(b_isp)mask|=gt::kDiagIsp;
            if(b_target)mask|=gt::kDiagTarget;
            if(b_perf)mask|=gt::kDiagPerf;
            if(b_uart)mask|=gt::kDiagUart;
            if(b_mcu)mask|=gt::kDiagMcu;
            if(b_log)mask|=gt::kDiagLog;
            if(b_avi)mask|=gt::kDiagAvi;
        }
        gt::diag_set_mask(mask);
        const char* state_dir=getenv("STATE_DIR");
        if(state_dir && *state_dir){
            gt::diag_set_state_dir(state_dir);
            gt::diag_install_crash_handlers();
        }
        gt::diag_state("start","print_mask=0x%X log=%d uart=%d dev=%s fmt=%s",
                       mask,(int)log_on,(int)uart_on,uart_dev,uart_format);
    }

    if(uart_on && !track_on){
        fprintf(stderr,"uart_enable requires track_enable (nothing to send otherwise)\n");
        return 2;
    }

    // ======================================================================
    // 4. 启动外设：记录器 → 串口 → VI/ISP/VENC（顺序不能乱）
    // ======================================================================
    // 记录器先起：串口线程一启动就可能回调 on_mcu()，必须先就绪
    if(log_on){
        gt::LogCfg lc;
        lc.enable=true;
        lc.trigger_mode=log_trigger;
        lc.mcu_stream=log_mcu_stream;   // 默认 false：frames.csv 单文件即成品表
        lc.dir=log_dir;
        lc.preroll_ms=static_cast<int>(log_preroll);
        // 视频预录独立配置。0 = 不缓冲视频帧，触发时无需 flush，
        // 编码线程不会阻塞 → 起飞后一帧不丢（代价：视频从触发那一刻开始）。
        g_video_preroll_ms=log_video_preroll;
        lc.postroll_ms=static_cast<int>(log_postroll);
        lc.max_seconds=static_cast<int>(log_maxsec);
        char mbuf[512];
        snprintf(mbuf,sizeof(mbuf),"build=%s %s",__DATE__,__TIME__);
        g_log.add_meta(mbuf);
        snprintf(mbuf,sizeof(mbuf),
                 "cfg uart_dev=%s fmt=%s rate=%uHz tx=%d track=%d detect=%d stride=%d steps=%d record=%d osd=%d",
                 uart_dev,uart_format,uart_rate,(int)uart_tx,(int)track_on,(int)g_detect_on,
                 g_detect_stride,g_sample_steps,(int)recording_enabled,(int)g_osd_on);
        g_log.add_meta(mbuf);
        snprintf(mbuf,sizeof(mbuf),"cfg preroll_ms=%u video_preroll_ms=%u postroll_ms=%u max_seconds=%u",
                 log_preroll,log_video_preroll,log_postroll,log_maxsec);
        g_log.add_meta(mbuf);
        if(!g_log.start(lc)){
            fprintf(stderr,"LOG start failed: %s\n",g_log.last_error().c_str());
            return 4;
        }
    }
    if(uart_on){
        gt::UartCfg uc; uc.enable=true; uc.device=uart_dev;
        uc.baud=static_cast<int>(uart_baud); uc.rate_hz=static_cast<int>(uart_rate);
        uc.ascii=!uart_binary;   // false → 发 v2 二进制帧
        uc.rx_print=uart_rx_print;               // 收到电控帧就打印
        uc.rx_print_hz=static_cast<int>(uart_rx_hz);
        uc.tx_enable=uart_tx;                    // false = 只收不发（纯记录模式）
        if(g_log.running()){
            g_uart.set_mcu_sink(&mcu_log_sink,&g_log);   // 必须在 start() 之前挂上
        }
        if(!g_uart.start(uc)){
            // 串口是联调必需通道，打不开就明确报错退出，而不是静默降级后
            // 让电控「收不到数据」却查不出原因。需要纯视觉时把 uart_enable 设 false。
            fprintf(stderr,"UART start failed: %s\n",g_uart.last_error().c_str());
            return 3;
        }
        // 立刻发一帧「无目标」，让电控马上知道视觉已上线
        gt::TargetOut boot_state;
        boot_state.detected=false;
        boot_state.state=gt::TargetState::kLost;
        boot_state.age_ms=-1.0f;
        g_uart.publish(boot_state);
    }

    // ======================================================================
    // 5. 初始化成像链路：传感器模式 → sys/vi → VENC → 线程
    // ======================================================================
    const char* exposure_mode=getenv("RECORD80_EXPOSURE_MODE");
    if(!exposure_mode)exposure_mode="manual";
    if(strcmp(exposure_mode,"auto") && strcmp(exposure_mode,"manual")){fprintf(stderr,"Invalid exposure mode\n");return 2;}
    const char* blc_mode=getenv("RECORD80_BLC_MODE");
    if(!blc_mode)blc_mode="manual";
    if(strcmp(blc_mode,"default") && strcmp(blc_mode,"manual")){fprintf(stderr,"Invalid black level mode\n");return 2;}
    unsigned blc_r,blc_gr,blc_gb,blc_b;
    if(!env_u32("RECORD80_BLC_R",256,0,4095,blc_r) ||
       !env_u32("RECORD80_BLC_GR",256,0,4095,blc_gr) ||
       !env_u32("RECORD80_BLC_GB",256,0,4095,blc_gb) ||
       !env_u32("RECORD80_BLC_B",256,0,4095,blc_b)){
        fprintf(stderr,"Invalid RECORD80 black level value\n");return 2;
    }
    unsigned quality,auto_exp,auto_again,auto_dgain,auto_ispgain,manual_exp,manual_again,manual_dgain,manual_ispgain;
    if(!env_u32("RECORD80_JPEG_QUALITY",85,75,90,quality) ||
       !env_u32("RECORD80_AUTO_EXP_MAX_US",0,0,10000,auto_exp) ||
       !env_u32("RECORD80_AUTO_AGAIN_MAX",0,0,16384,auto_again) ||
       !env_u32("RECORD80_AUTO_DGAIN_MAX",0,0,4096,auto_dgain) ||
       !env_u32("RECORD80_AUTO_ISPGAIN_MAX",0,0,4096,auto_ispgain) ||
       !env_u32("RECORD80_MANUAL_EXP_US",3000,100,10000,manual_exp) ||
       !env_u32("RECORD80_MANUAL_AGAIN",1024,1024,16384,manual_again) ||
       !env_u32("RECORD80_MANUAL_DGAIN",1024,1024,4096,manual_dgain) ||
       !env_u32("RECORD80_MANUAL_ISPGAIN",1024,1024,4096,manual_ispgain) ||
       (auto_again && auto_again<1024) || (auto_dgain && auto_dgain<1024) ||
       (auto_ispgain && auto_ispgain<1024)){
        fprintf(stderr,"Invalid RECORD80 tuning value\n");return 2;
    }
    setvbuf(stdout,nullptr,_IOLBF,0);signal(SIGPIPE,SIG_IGN);
    DIAGF(gt::kDiagBoot, "FLY_LICHEERV camera_preview=true inference=disabled recording=%s\n",
           recording_enabled?"enabled":"disabled");
    // 网页服务开关：http_enable=false 时连 socket 都不建（否则端口仍在 LISTEN 却不 accept，
    // 外面看起来像"没关干净"，实测过一次）
    int server=-1;
    if(g_http_on){
    server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return 2;
    int reuse=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(8081);
    const char*web_ip=getenv("WEB_IP_BIND")?getenv("WEB_IP_BIND"):argv[1];
    if(!strcmp(web_ip,"auto"))address.sin_addr.s_addr=INADDR_ANY;
    else if(inet_pton(AF_INET,web_ip,&address.sin_addr)!=1){perror("HTTP parse");close(server);return 2;}
    if(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(server,4)){perror("HTTP bind");close(server);return 2;}
    DIAGF(gt::kDiagBoot, "WEB_BIND ip=%s\n",web_ip);
    }else{
        DIAGF(gt::kDiagBoot, "WEB_SKIPPED (http_enable=false)\n");
    }
    // 只支持 80fps 一种模式（之前试过 60fps，寄存器表和时钟都对不上，已废弃）
    const char* mode="80";
    setenv("MAIX_SENSOR_FPS",mode,1);
    DIAGF(gt::kDiagBoot, "sensor_mode=%s preview=1280x720 quality=%u preview_limiter=none exposure_mode=%s\n",mode,quality,exposure_mode);
    DIAGF(gt::kDiagBoot, "RECORD80_BLC_CONFIG mode=%s offsets=%u,%u,%u,%u\n",blc_mode,blc_r,blc_gr,blc_gb,blc_b);
    SAMPLE_INI_CFG_S ini{};SAMPLE_VI_CONFIG_S cfg{};
    // 传感器模式：与 80fps 原版（camera_36record.cpp）完全一致的做法 ——
    // 无论 /mnt/data/sensor_cfg.ini 写的是什么，都在代码里覆盖为 720P_60FPS，
    // 再配合 36MHz 时钟（0x030028c4 = 0x00210001）得到约 80fps。
    // 关键在于驱动：src/sensor/ 里的用户态驱动带 GC4653_MODE_1280X720P60
    // （SDK 预编译的 libsns_gc4653 只有 1440P，强行跑 720P 会色道错位变全绿）。
    if(SAMPLE_COMM_VI_ParseIni(&ini)){close(server);return 3;}
    const bool mode_720p = getenv("SENSOR_4M30") ? false : true;
    if(mode_720p) ini.enSnsType[0]=GCORE_GC4653_MIPI_720P_60FPS_10BIT;
    CVI_VI_SetDevNum(ini.devNum);
    if(SAMPLE_COMM_VI_IniToViCfg(&ini,&cfg)){close(server);return 3;}
    if(getenv("PREVIEW_DIAG_LINEAR"))cfg.astViInfo[0].stChnInfo.enCompressMode=COMPRESS_MODE_NONE;
    DIAGF(gt::kDiagBoot, "DIAG configured_compress=%d\n",cfg.astViInfo[0].stChnInfo.enCompressMode);
    DIAGF(gt::kDiagBoot, "STEP sys_init begin\n");fflush(stdout);
    SIZE_S size = mode_720p ? SIZE_S{1280,720} : SIZE_S{2560,1440};
    DIAGF(gt::kDiagBoot, "SENSOR_MODE %s size=%ux%u detect=%s display=%s\n",
           mode_720p?"720P_60FPS":"4M_30FPS", size.u32Width, size.u32Height,
           g_detect_on?"on":"off", g_display_on?"on":"off");
    /* VB pool follows the VI channel size: production 720P mode is 1280x720. */
    int sys_rc = SAMPLE_PLAT_SYS_INIT(size);
    DIAGF(gt::kDiagBoot, "POOL_SIZE %ux%u sys_init_rc=%d\n", size.u32Width, size.u32Height, sys_rc);
    fflush(stdout);
    if(sys_rc){
        close(server);
        return 4;
    }
    int vi_rc=SAMPLE_PLAT_VI_INIT(&cfg);
    DIAGF(gt::kDiagBoot, "STEP vi_init rc=%d\n",vi_rc);
    fflush(stdout);
    if(vi_rc){
        SAMPLE_COMM_SYS_Exit();
        close(server);
        return 5;
    }

    // 停止信号：start.sh 的 stop / 我们的孤儿保护都用 SIGTERM
    signal(SIGTERM,stop);
    signal(SIGINT,stop);
    signal(SIGPIPE,SIG_IGN);

    // VI 通道深度 = 2（两帧缓冲，保证 80fps 不丢帧）
    VI_CHN_ATTR_S vi{};
    int rc=CVI_VI_GetChnAttr(0,0,&vi);
    vi.u32Depth=2;
    if(!rc)rc=CVI_VI_SetChnAttr(0,0,&vi);
    // ---- 宽度裁剪（去掉保护盖造成的左右黑边）----------------------------
    // 只裁宽度、高度保持满幅；居中裁，左右各去 (sensor_w - crop_w)/2。
    // ⚠ VI 通道裁剪之后，送到 CPU 的帧就是裁剪后的：
    //     检测 / OSD / VENC 全部看到新尺寸，所以下面要同步：
    //       ① 相机主点 cx 左移 crop_x（否则目标角度整体偏）
    //       ② VENC 画幅
    //       ③ AVI 头里的画幅
    unsigned crop_w=size.u32Width, crop_h=size.u32Height, crop_x=0;
    if(cam_crop_w>0 && cam_crop_w<size.u32Width){
        // ⚠ 必须是 64 的倍数：VI 输出的 stride（每行字节数）按 64 对齐。
        // 实测 crop_w=1104 时 width=1104 但 stride=1152，VENC 把行尾填充当成像素，
        // 画面被拉斜、JPEG 体积从 19KB 暴涨到 84KB（视频不可用）。
        // 只要 stride == width，编码就正常。
        crop_w=(cam_crop_w/64u)*64u;
        if(crop_w<256u)crop_w=256u;
        crop_x=((size.u32Width-crop_w)/2u)/2u*2u;    // 起点 2 对齐
        VI_CROP_INFO_S crop{};
        crop.bEnable=CVI_TRUE;
        crop.enCropCoordinate=VI_CROP_ABS_COOR;
        crop.stCropRect.s32X=(CVI_S32)crop_x;
        crop.stCropRect.s32Y=0;
        crop.stCropRect.u32Width=crop_w;
        crop.stCropRect.u32Height=crop_h;
        int crop_rc=CVI_VI_SetChnCrop(0,0,&crop);
        DIAGF(gt::kDiagBoot,"VI_CROP enable=1 x=%u y=0 %ux%u (64 对齐) rc=0x%x\n",
              crop_x,crop_w,crop_h,crop_rc);
        if(crop_rc){
            fprintf(stderr,"VI_CROP_FAILED rc=0x%x (回退到满幅 %ux%u)\n",
                    crop_rc,size.u32Width,size.u32Height);
            crop_w=size.u32Width; crop_h=size.u32Height; crop_x=0;
        }else{
            // 裁剪后主点左移，保证角度换算和全幅一致
            g_target.shift_cx((float)crop_x);
            DIAGF(gt::kDiagBoot,"CALIB_CX_SHIFT cam_cx=%.1f - crop_x=%u -> cx=%.1f\n",
                  cam_cx,crop_x,g_target.principal_cx());
        }
    }else{
        DIAGF(gt::kDiagBoot,"VI_CROP enable=0 full=%ux%u cam_crop_width=%u\n",
              size.u32Width,size.u32Height,cam_crop_w);
    }
    avi.set_dimensions((int)crop_w,(int)crop_h);
    VI_PIPE_ATTR_S pipe{};
    int prc=CVI_VI_GetPipeAttr(0,&pipe);
    DIAGF(gt::kDiagBoot, "DIAG channel_rc=%x compress=%d video=%d depth=%u pipe_rc=%x pipe_compress=%d\n",rc,vi.enCompressMode,vi.enVideoFormat,vi.u32Depth,prc,pipe.enCompressMode);

    // 硬件 JPEG 编码通道（display_enable=false 且 record_enable=false 时才不建）
    VENC_CHN_ATTR_S attr{};
    // JPEG single-frame mode has no MJPEG frame-rate-control field.
    attr.stVencAttr.enType=PT_JPEG;
    attr.stVencAttr.u32MaxPicWidth=attr.stVencAttr.u32PicWidth=crop_w;
    attr.stVencAttr.u32MaxPicHeight=attr.stVencAttr.u32PicHeight=crop_h;
    attr.stVencAttr.u32BufSize=2*1024*1024;
    attr.stVencAttr.bByFrame=CVI_TRUE;
    attr.stVencAttr.stAttrJpege.bSupportDCF=CVI_FALSE;
    attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum=0;
    attr.stVencAttr.stAttrJpege.enReceiveMode=VENC_PIC_RECEIVE_SINGLE;
    attr.stGopAttr.enGopMode=VENC_GOPMODE_NORMALP;
    bool created=false;
    if(g_encode_on){
        if(!rc){
            rc=CVI_VENC_CreateChn(0,&attr);
            created=rc==0;
            DIAGF(gt::kDiagBoot, "VENC CreateChn rc=0x%x\n",rc);
        }
        if(!rc){
            VENC_JPEG_PARAM_S jpeg{};
            rc=CVI_VENC_GetJpegParam(0,&jpeg);
            if(!rc){
                jpeg.u32Qfactor=quality;
                rc=CVI_VENC_SetJpegParam(0,&jpeg);
            }
            DIAGF(gt::kDiagBoot, "VENC JPEG quality=%u rc=0x%x\n",quality,rc);
        }
        if(rc)running=0;
    }else{
        DIAGF(gt::kDiagBoot, "VENC skipped (display_enable=false && record_enable=false)\n");
    }
    std::thread server_thread;
    std::thread encoder_thread;
    if(g_http_on){
        server_thread=std::thread(http_server,server);
    }else{
        DIAGF(gt::kDiagBoot, "HTTP server skipped (http_enable=false)\n");
    }
    if(g_encode_on){
        encoder_thread=std::thread(encoder_worker);
    }
    DIAGF(gt::kDiagBoot, "CAMERA_READY display=%s http=%s preview=%s recorder=%s\n",
           g_display_on?"on":"off", g_http_on?"on":"off", g_encode_on?argv[1]:"none",
           recording_enabled?"on":"off");
    // 自启动最想知道的就是这一条：VI/ISP/VENC 全都起来了
    gt::diag_state("ready","display=%d http=%d record=%d uart=%d log=%d temp=%dC web=%s",
                   (int)g_display_on,(int)g_http_on,(int)recording_enabled,(int)g_uart.running(),
                   (int)g_log.running(),gt::diag_cpu_temp_c(),
                   g_http_on&&g_display_on?argv[1]:"off");
    double last_beat_s=0;
    // ======================================================================
    // 6. 主循环：一帧一轮
    //    取帧 → 抽点检测 → 筛选/卡尔曼/角度 → 画框 → 发串口 → 交编码 → 记一行
    // ======================================================================
    unsigned long count=0,dups=0,timeouts=0;
    uint64_t firstpts=0,lastpts=0;
    double start=now(),first=0,last=0,report=start;
    bool diag_applied=false;
    bool tuning_failed=false;
    bool blc_check_done=false;
    double last_ae_report=0;
    unsigned long last_encoded=0,last_sent=0,last_bytes=0,last_frame_count=0;
    uint64_t last_window_pts=0;
    while(running){
        VIDEO_FRAME_INFO_S frame{};
        if(CVI_VI_GetChnFrame(0,0,&frame,1000)){
            if(running)++timeouts;
            continue;
        }
        // 取帧成功立刻建立帧元数据：帧号 + 原始 PTS + 采集时刻（不是曝光时刻）。
        // 后面所有异步环节（串口线程）只拿这份 POD 拷贝，绝不持有 VIDEO_FRAME_INFO_S。
        gt::FrameTiming timing{};
        timing.frame_id=++g_capture_frame_id;
        timing.vi_pts_raw=frame.stVFrame.u64PTS;
        timing.capture_mono_us=gt::monotonic_us();
        // 电控快照必须在「取到帧的这一刻」读，不能等到检测之后：
        // 检测要 5~20ms，那期间新到的电控帧会被算进"这一帧"，导致数据比画面新
        // （实测过：检测后读比按时间戳重算新 0~9ms，中位 6ms）。这里固定住，
        // 后面 OSD 和记录器都用这一份，语义才是「该帧时刻的电控状态」。
        gt::McuState mcu_at_capture;
        int mcu_age_now=-1;
        gt::MonoUs mcu_rx_now=0;
        // 一次加锁同时取快照+年龄+到达时刻，保证三者属于同一帧电控数据
        const bool has_mcu= g_uart.mcu_snapshot(&mcu_at_capture,&mcu_age_now,&mcu_rx_now);
        const gt::McuState* mcu_now= has_mcu ? &mcu_at_capture : nullptr;
        double t=now();
        if(!diag_applied && t-start>2){
            diag_applied=true;
            ISP_EXPOSURE_ATTR_S ae{};int ae_rc=CVI_ISP_GetExposureAttr(0,&ae);
            DIAGF(gt::kDiagIsp, "RECORD80_AE_BEFORE rc=0x%x mode=%d exp_min=%u exp_max=%u\n",ae_rc,ae.enOpType,ae.stAuto.stExpTimeRange.u32Min,ae.stAuto.stExpTimeRange.u32Max);
            if(!ae_rc && !strcmp(exposure_mode,"manual")){
                ae.enOpType=OP_TYPE_MANUAL;ae.stManual.enGainType=AE_TYPE_GAIN;
                ae.stManual.enExpTimeOpType=ae.stManual.enAGainOpType=ae.stManual.enDGainOpType=ae.stManual.enISPDGainOpType=OP_TYPE_MANUAL;
                ae.stManual.u32ExpTime=manual_exp;ae.stManual.u32AGain=manual_again;
                ae.stManual.u32DGain=manual_dgain;ae.stManual.u32ISPDGain=manual_ispgain;
                ae_rc=CVI_ISP_SetExposureAttr(0,&ae);
                DIAGF(gt::kDiagIsp, "EXPOSURE_SET us=%u again=%u dgain=%u isp_gain=%u rc=%x\n",manual_exp,manual_again,manual_dgain,manual_ispgain,ae_rc);
             }else if(!ae_rc && !strcmp(exposure_mode,"auto")){
                ae.enOpType=OP_TYPE_AUTO;
                // 自动模式下把上限收紧：0 = 保持 ISP 默认上限
                if(auto_exp){
                    ae.stAuto.stExpTimeRange.u32Max=auto_exp;
                    if(ae.stAuto.stExpTimeRange.u32Min>auto_exp)ae.stAuto.stExpTimeRange.u32Min=auto_exp;
                }
                // auto 模式的曝光上限必须夹住，否则 AE 会为了提亮把曝光拉到几十毫秒：
                // 80fps 的帧周期只有 12.5ms，实测 AE 在暗处选了 exp_us=33155（33ms），
                // 结果是 host_fps 从 80 掉到 62，而且飞镖会被拖影彻底糊掉。
                // 上限取 10ms（帧周期的 80%），给读出和编码留余量。
                // 想做更长的曝光必须同时降帧率 —— 那是另一个功能，不在这里偷偷放开。
                {
                    const unsigned exp_budget=10000;   // us；80fps × 12.5ms 的 80%
                    if(ae.stAuto.stExpTimeRange.u32Max>exp_budget){
                        DIAGF(gt::kDiagIsp,"AE_EXP_CLAMP max=%u -> %u（80fps 帧周期只有 12.5ms）\n",
                              ae.stAuto.stExpTimeRange.u32Max,exp_budget);
                        ae.stAuto.stExpTimeRange.u32Max=exp_budget;
                    }
                    if(ae.stAuto.stExpTimeRange.u32Min>ae.stAuto.stExpTimeRange.u32Max)
                        ae.stAuto.stExpTimeRange.u32Min=ae.stAuto.stExpTimeRange.u32Max;
                }
                if(auto_again){
                    ae.stAuto.stAGainRange.u32Max=auto_again;
                    if(ae.stAuto.stAGainRange.u32Min>auto_again)ae.stAuto.stAGainRange.u32Min=auto_again;
                }
                if(auto_dgain){
                    ae.stAuto.stDGainRange.u32Max=auto_dgain;
                    if(ae.stAuto.stDGainRange.u32Min>auto_dgain)ae.stAuto.stDGainRange.u32Min=auto_dgain;
                }
                if(auto_ispgain){
                    ae.stAuto.stISPDGainRange.u32Max=auto_ispgain;
                    if(ae.stAuto.stISPDGainRange.u32Min>auto_ispgain)ae.stAuto.stISPDGainRange.u32Min=auto_ispgain;
                }
                ae_rc=CVI_ISP_SetExposureAttr(0,&ae);
                DIAGF(gt::kDiagIsp, "RECORD80_AE_AUTO_LIMIT exp_max=%u again_max=%u dgain_max=%u isp_gain_max=%u rc=0x%x\n",auto_exp,auto_again,auto_dgain,auto_ispgain,ae_rc);
            }
            if(ae_rc){
                tuning_failed=true;running=0;
            }
            if(!strcmp(blc_mode,"manual")){
                ISP_BLACK_LEVEL_ATTR_S blc{};int be=CVI_ISP_GetBlackLevelAttr(0,&blc);
                if(!be){
                    blc.Enable=CVI_TRUE;blc.enOpType=OP_TYPE_MANUAL;
                    blc.stManual.OffsetR=blc_r;blc.stManual.OffsetGr=blc_gr;
                    blc.stManual.OffsetGb=blc_gb;blc.stManual.OffsetB=blc_b;
                    be=CVI_ISP_SetBlackLevelAttr(0,&blc);
                }
                bool match=false;
                if(!be){
                    ISP_BLACK_LEVEL_ATTR_S readback{};be=CVI_ISP_GetBlackLevelAttr(0,&readback);
                    match=!be && readback.Enable && readback.enOpType==OP_TYPE_MANUAL &&
                        readback.stManual.OffsetR==blc_r && readback.stManual.OffsetGr==blc_gr &&
                        readback.stManual.OffsetGb==blc_gb && readback.stManual.OffsetB==blc_b;
                }
                DIAGF(gt::kDiagIsp, "RECORD80_BLC_SET offsets=%u,%u,%u,%u rc=0x%x readback_match=%d\n",blc_r,blc_gr,blc_gb,blc_b,be,match);
                if(be || !match){tuning_failed=true;running=0;}
            }
            // 三个只在排查 ISP 问题时才用的调试开关（默认不设 = 不执行）
            ISP_PUB_ATTR_S pub{};
            int pe=CVI_ISP_GetPubAttr(0,&pub);
            DIAGF(gt::kDiagIsp, "DIAG ISP pub fps=%f get=%x\n",pub.f32FrameRate,pe);
            if(!pe && getenv("PREVIEW_DIAG_PUBFPS")){
                pub.f32FrameRate=atof(getenv("PREVIEW_DIAG_PUBFPS"));
                DIAGF(gt::kDiagIsp, "DIAG set ISP pub fps=%f rc=%x\n",pub.f32FrameRate,CVI_ISP_SetPubAttr(0,&pub));
            }
            if(getenv("PREVIEW_DIAG_NOTNR")){
                ISP_TNR_ATTR_S a{};
                int e=CVI_ISP_GetTNRAttr(0,&a);
                DIAGF(gt::kDiagIsp, "DIAG TNR enabled=%d get=%x\n",a.Enable,e);
                if(!e){
                    a.Enable=CVI_FALSE;
                    DIAGF(gt::kDiagIsp, "DIAG disable TNR=%x\n",CVI_ISP_SetTNRAttr(0,&a));
                }
            }
            if(getenv("PREVIEW_DIAG_NODPC")){
                ISP_DP_DYNAMIC_ATTR_S a{};
                ISP_DP_STATIC_ATTR_S b{};
                int e=CVI_ISP_GetDPDynamicAttr(0,&a);
                DIAGF(gt::kDiagIsp, "DIAG DPC dynamic=%d get=%x\n",a.Enable,e);
                if(!e){
                    a.Enable=CVI_FALSE;
                    DIAGF(gt::kDiagIsp, "DIAG disable dynamicDPC=%x\n",CVI_ISP_SetDPDynamicAttr(0,&a));
                }
                e=CVI_ISP_GetDPStaticAttr(0,&b);
                DIAGF(gt::kDiagIsp, "DIAG DPC static=%d get=%x\n",b.Enable,e);
                if(!e){
                    b.Enable=CVI_FALSE;
                    DIAGF(gt::kDiagIsp, "DIAG disable staticDPC=%x\n",CVI_ISP_SetDPStaticAttr(0,&b));
                }
            }
        }
        // 每 5 秒回读一次实际曝光（确认"设下去的值"真的生效了）
        if(t-last_ae_report>=5){
            last_ae_report=t;
            ISP_EXP_INFO_S exp{};
            int e=CVI_ISP_QueryExposureInfo(0,&exp);
            DIAGF(gt::kDiagIsp,
                  "RECORD80_AE_INFO elapsed_s=%.2f rc=0x%x exp_us=%u again=%u dgain=%u isp_gain=%u luma=%u\n",
                  t-start,e,exp.u32ExpTime,exp.u32AGain,exp.u32DGain,exp.u32ISPDGain,exp.u8AveLum);
        }
        if(!blc_check_done && !strcmp(blc_mode,"manual") && t-start>10){
            blc_check_done=true;
            ISP_INNER_STATE_INFO_S inner{};
            int be=CVI_ISP_QueryInnerStateInfo(0,&inner);
            bool match=!be && inner.blcOffsetR==blc_r && inner.blcOffsetGr==blc_gr &&
                inner.blcOffsetGb==blc_gb && inner.blcOffsetB==blc_b;
            DIAGF(gt::kDiagIsp, "RECORD80_BLC_VERIFY elapsed_s=%.2f rc=0x%x offsets=%u,%u,%u,%u match=%d\n",t-start,be,inner.blcOffsetR,inner.blcOffsetGr,inner.blcOffsetGb,inner.blcOffsetB,match);
            if(!match){tuning_failed=true;running=0;}
        }
        // 前 4 秒是 ISP 预热，这段时间的帧直接归还
        if(t-start<4){
            CVI_VI_ReleaseChnFrame(0,0,&frame);
            continue;
        }
        if(!count){
            first=t;
            firstpts=frame.stVFrame.u64PTS;
            DIAGF(gt::kDiagBoot,
                  "VI geometry=%ux%u stride=%u,%u format=%d compress=%d video=%d lengths=%u,%u,%u\n",
                  frame.stVFrame.u32Width,frame.stVFrame.u32Height,
                  frame.stVFrame.u32Stride[0],frame.stVFrame.u32Stride[1],
                  frame.stVFrame.enPixelFormat,frame.stVFrame.enCompressMode,
                  frame.stVFrame.enVideoFormat,
                  frame.stVFrame.u32Length[0],frame.stVFrame.u32Length[1],frame.stVFrame.u32Length[2]);
        }
        else if(frame.stVFrame.u64PTS<=lastpts)++dups;
        last=t;
        lastpts=frame.stVFrame.u64PTS;
        ++count;
        // OSD 要用的目标结果（tgt 只在检测分支里有效，复制三个标量出来）
        bool osd_det=false;
        float osd_yaw=0.0f, osd_pitch=0.0f;
        {
            bool logged_row=false;   // 本帧是否已经写过记录行（每帧一行，跑没跑检测都写）
            // ---- 绿色圆灯检测（每帧，抽点 4 → 320x180 网格）----
            // 说明：VI 帧为 NV12（UV 交织）。抽点 Y 与 UV，步长 4；
            //       U/V 顺序对绿色判据无影响（两者都要求 < 128）。
            // 一次性打印帧几何：VI 通道裁剪后，宽会变小，但 stride（每行字节数）
        // 可能仍是原宽度。若 stride > width，说明帧里有行尾填充；
        // 这时 VENC 必须按 stride 解释，否则画面会被拉斜、JPEG 体积暴涨。
        {
            static bool geom_logged=false;
            if(!geom_logged){
                geom_logged=true;
                DIAGF(gt::kDiagBoot,"FRAME_GEOM width=%u height=%u stride=%u len=%u crop_w=%u stride_ok=%d\n",
                      frame.stVFrame.u32Width,frame.stVFrame.u32Height,
                      frame.stVFrame.u32Stride[0],frame.stVFrame.u32Length[0],crop_w,
                      frame.stVFrame.u32Stride[0]==frame.stVFrame.u32Width?1:0);
                if(frame.stVFrame.u32Stride[0]!=frame.stVFrame.u32Width){
                    fprintf(stderr,"FRAME_STRIDE_MISMATCH width=%u stride=%u —— 视频会拉斜，"
                                   "请把 cam_crop_width 改成 64 的倍数\n",
                            frame.stVFrame.u32Width,frame.stVFrame.u32Stride[0]);
                }
                fflush(stdout);
            }
        }
        const unsigned W=frame.stVFrame.u32Width,H=frame.stVFrame.u32Height;
            if(frame.stVFrame.pu8VirAddr[0]==nullptr){
                unsigned sy=frame.stVFrame.u32Length[0]?frame.stVFrame.u32Length[0]:frame.stVFrame.u32Stride[0]*H;
                unsigned su=frame.stVFrame.u32Length[1]?frame.stVFrame.u32Length[1]:frame.stVFrame.u32Stride[1]*((H+1)/2);
                frame.stVFrame.pu8VirAddr[0]=map_cached(frame.stVFrame.u64PhyAddr[0],sy);
                frame.stVFrame.pu8VirAddr[1]=map_cached(frame.stVFrame.u64PhyAddr[1],su);
            }
            static unsigned char gy[320*180],gu[160*90],gv[160*90];
            const int S=(W>=2000)?8:4,w=W/S,h=H/S,cw=w/2,ch=h/2;
            const bool do_detect = g_detect_on && g_track_on && ((g_frame_idx++ % (unsigned)g_detect_stride)==0);
            if(do_detect && frame.stVFrame.pu8VirAddr[0] && w<=320 && h<=180){
                timing.detect_start_us=gt::monotonic_us();
                const double td0=now();
                const unsigned char*Yp=frame.stVFrame.pu8VirAddr[0];
                const unsigned char*UVp=frame.stVFrame.pu8VirAddr[1];
                unsigned ystride=frame.stVFrame.u32Stride[0],uvstride=frame.stVFrame.u32Stride[1];
                const unsigned y_rows=frame.stVFrame.u32Length[0]/ystride; /* Y 有效行数 */
                const unsigned uv_rows=frame.stVFrame.u32Length[1]/frame.stVFrame.u32Stride[1];
                /* 块内 n×n 子采样取最大值：发光目标是亮斑，纯抽样会漏掉。
                   n 由 DETECT_SAMPLE_STEPS 控制：1=1.7ms 2=7ms 4=28ms。 */
                const int ns=g_sample_steps;
                const int sstep=(ns>1)?(S/ns):S;
                for(int yy=0;yy<h;++yy){
                    const int by=yy*S;
                    for(int xx=0;xx<w;++xx){
                        const int bx=xx*S;
                        unsigned m=0;
                        for(int sy=0;sy<ns;++sy){
                            const unsigned ry=(unsigned)(by+sy*sstep);
                            if(ry>=y_rows)break;
                            const unsigned char*row=Yp+(size_t)ry*ystride;
                            for(int sx=0;sx<ns;++sx){
                                const unsigned cx=(unsigned)(bx+sx*sstep);
                                if(cx>=W)break;
                                const unsigned v=row[cx];
                                if(v>m)m=v;
                            }
                        }
                        gy[(size_t)yy*w+xx]=(unsigned char)m;}}
                for(int yy=0;yy<ch;++yy){
                    const unsigned ur=(yy*S < uv_rows) ? yy*S : (uv_rows?uv_rows-1:0);
                    const unsigned char*r=UVp+(size_t)ur*uvstride;
                    for(int xx=0;xx<cw;++xx){gu[(size_t)yy*cw+xx]=r[(xx*S)*2];gv[(size_t)yy*cw+xx]=r[(xx*S)*2+1];}}
                samp_total_ms+=(now()-td0)*1000.0;
                const double td1=now();
                ++green_frames;
                // ---- ① 多候选收集：不再只返回最优一个 ----
                greendet::Blob cands[32];
                const int ncand=g_detector.detectMulti(gy,gu,gv,w,h,cands,g_max_candidates);
                // ---- ② 帧间隔：用「采集时刻」差，不把抽样/检测/日志/调度抖动算进 dt ----
                // 只有真正调用 Tracker::update 的帧才更新基准时刻，因此 detect_stride>1
                // 时 dt 自然等于跨帧的采集间隔；时间戳缺失/倒退时退回 80fps 默认值。
                const double t_now=now();
                const float dt=gt::capture_dt_s(timing.capture_mono_us,g_last_track_capture_us,0.0125f);
                g_last_track_capture_us=timing.capture_mono_us;
                // ---- ③ 多目标筛选 + 卡尔曼 + 角度换算 ----
                const gt::TargetOut& tgt=g_target.update(cands,ncand,S,dt,t_now*1000.0);
                timing.detect_end_us=gt::monotonic_us();
                osd_det=tgt.detected;
                osd_yaw=tgt.yaw_deg;
                osd_pitch=tgt.pitch_deg;
                track_total_ms+=(now()-td1)*1000.0;
                // ---- ④ 把选中目标的框叠加到 Y 平面（编码前，网页与录像都能看到）----
                // 纯亮度写入，不涉及色彩转换；框宽 3px，最小半径 12px 保证远处可见。
                if(g_display_on && tgt.has_box){
                    int cxp=(int)(tgt.gx*S), cyp=(int)(tgt.gy*S);
                    int rp=(int)(tgt.gr*S)+6;
                    if(rp<12)rp=12;          // 远处目标的最小框，保证肉眼可见
                    int x0=cxp-rp, x1=cxp+rp, y0=cyp-rp, y1=cyp+rp;
                    if(x0<0)x0=0;
                    if(y0<0)y0=0;
                    if(x1>(int)W-2)x1=(int)W-2;
                    if(y1>(int)H-2)y1=(int)H-2;
                    unsigned char*Yw=(unsigned char*)Yp;
                    for(int t=0;t<3;++t){
                        int ya=y0+t, yb=y1-t, xa=x0+t, xb=x1-t;
                        if(ya>=0&&ya<(int)H){
                            for(int x=x0;x<=x1;++x)Yw[(size_t)ya*ystride+x]=255;
                        }
                        if(yb>=0&&yb<(int)H){
                            for(int x=x0;x<=x1;++x)Yw[(size_t)yb*ystride+x]=255;
                        }
                        if(xa>=0&&xa<(int)W){
                            for(int y=y0;y<=y1;++y)Yw[(size_t)y*ystride+xa]=255;
                        }
                        if(xb>=0&&xb<(int)W){
                            for(int y=y0;y<=y1;++y)Yw[(size_t)y*ystride+xb]=255;
                        }
                    }
                    // 缓存映射下必须把脏行写回 DRAM，硬件 JPEG 才能看到框
                    CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[0],(CVI_VOID*)Yw, frame.stVFrame.u32Stride[0]*H);
                }
                // 调试：固定位置画框，用于验证画框+缓存刷新链路（无需真实目标）
                if(g_draw_test_box && frame.stVFrame.pu8VirAddr[0]){
                    unsigned char*Yw=(unsigned char*)Yp;
                    int bx0=(int)(W/4), bx1=(int)(W/4+W/4), by0=(int)(H/4), by1=(int)(H/4+H/4);
                    for(int t=0;t<4;++t){
                        int ya=by0+t, yb=by1-t, xa=bx0+t, xb=bx1-t;
                        for(int x=bx0;x<=bx1;++x){
                            if(ya<(int)H)Yw[(size_t)ya*ystride+x]=255;
                            if(yb>=0)Yw[(size_t)yb*ystride+x]=255;
                        }
                        for(int y=by0;y<=by1;++y){
                            if(xa<(int)W)Yw[(size_t)y*ystride+xa]=255;
                            if(xb>=0)Yw[(size_t)y*ystride+xb]=255;
                        }
                    }
                    CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[0],(CVI_VOID*)Yw, frame.stVFrame.u32Stride[0]*H);
                }
                // ---- ⑤ 日志：输出角度/角速度/距离/数据年龄，供联调核对 ----
                // 限频 10Hz：连续目标时原来 tgt.misses==0 会每帧 printf（80Hz），
                // 同步写 stdout 会拖慢主循环。计数照常累加，统计由 5 秒日志给出。
                if(tgt.detected){
                    ++green_hits;
                    const gt::MonoUs log_now=gt::monotonic_us();
                    if(log_now-g_last_target_log_us>=100000){
                        g_last_target_log_us=log_now;
                        DIAGF(gt::kDiagTarget, "GREEN_TARGET state=%s yaw=%.2f pitch=%.2f vyaw=%.2f vpitch=%.2f "
                               "dist=%.3f%s age=%.1fms cand=%d px=%.1f,%.1f r=%.1f "
                               "lyaw=%.2f lpitch=%.2f hits=%lu frame=%u\n",
                               target_state_name(tgt.state),tgt.yaw_deg,tgt.pitch_deg,
                               tgt.v_yaw_dps,tgt.v_pitch_dps,tgt.distance_m,
                               tgt.distance_valid?"":"(invalid)",tgt.age_ms,tgt.candidates,
                               tgt.gx*S,tgt.gy*S,tgt.gr*S,tgt.light_yaw_deg,tgt.light_pitch_deg,
                               green_hits,timing.frame_id);
                    }
                }
                // 无论有没有目标都发布：电控需要区分「视觉在线但没看到」和「视觉掉线」
                // 同时把该帧的采集时间戳交给串口线程：它在发送时重算「取帧→发送」年龄，
                // 只进诊断日志，不改变 ASCII 的 age 字段语义。
                g_uart.publish(tgt,timing);
                g_last_pub_capture_us=timing.capture_mono_us;
                g_last_pub_frame_id=timing.frame_id;
                // 一帧一行：在采集线程里、结果刚算完时入队（只做 snprintf+memcpy）
                if(g_log.running()){
                    // 用「取帧那一刻」的快照（不是检测完之后再读）
                    g_log.on_frame(timing,tgt,1,S,mcu_now,mcu_age_now,mcu_rx_now);
                    logged_row=true;
                }
                const double det_ms=(now()-td0)*1000.0;
                det_total_ms+=det_ms;
                if(det_ms>g_det_max_ms)g_det_max_ms=det_ms;
                const double proc_ms=timing.detect_duration_us()/1000.0;
                g_proc_total_ms+=proc_ms;
                if(proc_ms>g_proc_max_ms)g_proc_max_ms=proc_ms;
                const double lat_ms=timing.capture_latency_us()/1000.0;
                g_lat_total_ms+=lat_ms;
                if(lat_ms>g_lat_max_ms)g_lat_max_ms=lat_ms;
                ++det_count;
                if(green_frames%300==0){
                    DIAGF(gt::kDiagTarget, "GREEN_STAT frames=%lu hits=%lu rate=%.1f%%\n",
                           green_frames,green_hits,100.0*green_hits/(green_frames?green_frames:1));
                }
            }
            // ---- ④b OSD：把「这一帧采集时」的电控状态烧进画面左上角 ----
            // 必须在交给编码线程之前画：编码线程拿到的是已编码的 JPEG，改不了像素。
            // 因为画的是"帧自己那一份快照"，编码线程丢帧也不会让画面和数据错位。
            if(g_osd_on && g_display_on && frame.stVFrame.pu8VirAddr[0]!=nullptr){
                unsigned char*Yw=(unsigned char*)frame.stVFrame.pu8VirAddr[0];
                const unsigned ys=frame.stVFrame.u32Stride[0];
                const int ow=(int)W, oh=(int)H;
                const gt::McuState* m=mcu_now;
                const int mage=mcu_age_now;
                char l1[64],l2[64],l3[64],l4[64];
                if(m!=nullptr){
                    snprintf(l1,sizeof l1,"F%05u %s SEQ%03u AG%03dMS%s",
                             timing.frame_id,mcu_state_txt(m->state),
                             (unsigned)(m->seq&0xFF),mage,
                             m->imu_valid()?"":" IMU!");
                    snprintf(l2,sizeof l2,"GX%6.2f GY%6.2f GZ%6.2f",
                             m->gx,m->gy,m->gz);
                    snprintf(l3,sizeof l3,"R%6.1f P%6.1f Y%6.1f",
                             m->roll,m->pitch,m->yaw);
                }else{
                    snprintf(l1,sizeof l1,"F%05u MCU NO-DATA (UART?)",timing.frame_id);
                    l2[0]=0;
                    l3[0]=0;
                }
                snprintf(l4,sizeof l4,"TGT Y%6.1f P%5.1f %s",
                         osd_det?osd_yaw:0.0f,osd_det?osd_pitch:0.0f,
                         osd_det?"":"NO-TGT");

                const int sc=g_osd_scale;
                const int line_h=(gt::osd::kFontH+2)*sc;
                int box_w=0;
                const char* lines[4]={l1,l2,l3,l4};
                for(int i=0;i<4;i++){
                    const int tw=gt::osd::text_width(lines[i],sc);
                    if(tw>box_w)box_w=tw;
                }
                box_w+=8;
                const int box_h=line_h*4+6;
                gt::osd::fill_rect(Yw,(int)ys,ow,oh,2,2,box_w,box_h,16);   // 半透明做不了，铺近黑底
                for(int i=0;i<4;i++){
                    gt::osd::draw_text(Yw,(int)ys,ow,oh,6,4+i*line_h,sc,lines[i]);
                }
                // 缓存映射下必须写回，硬件 JPEG 才能看到（与画框同理）
                CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[0],(CVI_VOID*)Yw,
                                      frame.stVFrame.u32Stride[0]*H);
            }
            if(g_encode_on){
                std::lock_guard<std::mutex> guard(input_mutex);
                if(has_pending){
                    CVI_VI_ReleaseChnFrame(0,0,&pending);
                    ++preview_replaced;
                }
                pending=frame;
                has_pending=true;
                // 编码线程靠这两个字段写视频侧车（AVI 帧号 → 相机帧号）
                pending_frame_id=timing.frame_id;
                pending_capture_us=timing.capture_mono_us;
            }else{
                CVI_VI_ReleaseChnFrame(0,0,&frame);   // 无消费者，直接归还
            }
            // 这一帧没跑检测（detect 关闭 / stride 跳过 / 尺寸不匹配）：仍然记一行时间戳，
            // 保证 frames.csv 与相机帧号能一一对应，缺口由 frame_id 跳号暴露
            if(g_log.running() && !logged_row){
                g_log.on_frame(timing,gt::TargetOut(),0,0,mcu_now,mcu_age_now,mcu_rx_now);
            }
        }
        // ------------------------------------------------------------------
        // exit_after_land：一次飞行（一个会话）结束就退出进程
        // 会话的开启/关闭由记录器线程决定（STATE 边沿 + postroll），这里只读它的统计：
        //   sessions>=1 说明至少开过一段（收到过起飞），open==0 说明这段已经收尾写完了
        // 满足就说明"这次飞行已经归档完毕"，可以退出，省电降温、也让 start.sh 收工。
        // 默认关闭（exit_after_land=false），保持常驻待命的老行为。
        // ------------------------------------------------------------------
        if(g_exit_after_land && g_log.running()){
            const gt::LogStats ls=g_log.stats();
            if(ls.sessions>0 && ls.open==0){
                DIAGF(gt::kDiagLog,"EXIT_AFTER_LAND sessions=%lu 会话已归档，进程退出\n",ls.sessions);
                printf("EXIT_AFTER_LAND sessions=%lu\n",ls.sessions);
                fflush(stdout);
                // ⚠️ 必须先停掉 running，再 break。
                // 编码线程是 while(running) 循环，不置 0 的话 join() 会永远阻塞
                //（实测卡了 108 秒直到收到 SIGTERM），收尾看起来像"程序不退出"。
                running=0;
                break;
            }
        }
        // ------------------------------------------------------------------
        // 每 5 秒一次的性能统计。每行一个指标组，字段含义见 docs/DIAGNOSTICS.md
        // ------------------------------------------------------------------
        if(t-report>=5 && count>1){
            unsigned long enc=encoded.load(),sent=network_sent.load(),bytes=network_bytes.load();

            // 记录器环形缓冲里当前攒了多少行。
            // 触发式记录靠它做「起飞前回滚」；这个数在待机期应该持续增长到窗口上限，
            // 如果一直是 0，说明 on_frame 根本没进缓冲，预录就是假的。
            const gt::LogStats lst=g_log.stats();

            // 采集 / 编码 / 网络
            DIAGF(gt::kDiagPerf,
                  "capture_frames=%lu host_fps=%.4f pts_fps=%.4f duplicate_pts=%lu timeouts=%lu "
                  "encoded=%lu encode_errors=%lu encode_fps=%.3f sent_fps=%.3f jpeg_mbit_s=%.3f "
                  "preview_replaced=%lu ring=%d log_open=%d\n",
                  count,
                  (count-1)/(last-first),
                  lastpts>firstpts?(count-1)*1e6/(lastpts-firstpts):0,
                  dups,
                  timeouts,
                  enc,
                  errors.load(),
                  (enc-last_encoded)/(t-report),
                  (sent-last_sent)/(t-report),
                  (bytes-last_bytes)*8e-6/(t-report),
                  preview_replaced.load(),
                  lst.ring_rows,
                  lst.open ? 1 : 0);

            // 检测耗时；打印完把窗口统计清零
            if(det_count){
                DIAGF(gt::kDiagPerf,
                      "DETECT avg_ms=%.3f max_ms=%.3f proc_avg_ms=%.3f proc_max_ms=%.3f "
                      "lat_avg_ms=%.3f lat_max_ms=%.3f calls=%lu stride=%d map_miss=%lu "
                      "sample_ms=%.3f track_ms=%.3f ns=%d cand_max=%d\n",
                      det_total_ms/det_count,
                      g_det_max_ms,
                      g_proc_total_ms/det_count,
                      g_proc_max_ms,
                      g_lat_total_ms/det_count,
                      g_lat_max_ms,
                      det_count,
                      g_detect_stride,
                      g_map_miss,
                      samp_total_ms/det_count,
                      track_total_ms/det_count,
                      g_sample_steps,
                      g_max_candidates);
                det_total_ms=0;
                det_count=0;
                samp_total_ms=0;
                track_total_ms=0;
                g_det_max_ms=0;
                g_proc_total_ms=0;
                g_proc_max_ms=0;
                g_lat_total_ms=0;
                g_lat_max_ms=0;
            }

            // 串口发送与接收
            if(g_uart.running()){
                const unsigned long us=g_uart.sent_frames();
                DIAGF(gt::kDiagUart, "UART sent=%lu rate=%.2fHz errors=%lu | rx_frames=%lu rx_crc=%lu rx_other=%lu rx_age=%dms\n",
                       us,(us-g_uart_sent_prev)/(t-report),g_uart.send_errors(),
                       g_uart.rx_frames(),g_uart.rx_crc_errors(),g_uart.rx_other_frames(),g_uart.mcu_age_ms());
                g_uart_sent_prev=us;
            }

            // 飞行记录器
            if(g_log.running()){
                const gt::LogStats ls=g_log.stats();
                DIAGF(gt::kDiagLog, "LOG sessions=%lu open=%d triggers=%lu rows_frames=%lu rows_mcu=%lu video=%lu dropped=%lu last=%dms\n",
                       ls.sessions,ls.open,ls.triggers,ls.rows_frames,ls.rows_mcu,ls.rows_video,
                       ls.dropped,ls.last_session_ms);
            }

            {
                // 帧时间戳：相机帧号、最近发布帧的年龄、串口「取帧→开始写」年龄
                // （后者只做诊断，不写进 ASCII 的 age 字段）
                const gt::WriteStartAgeStats as=g_uart.take_write_start_age_stats();
                const double cap_age_ms=g_last_pub_capture_us?static_cast<double>(gt::monotonic_us()-g_last_pub_capture_us)/1000.0:-1.0;
                DIAGF(gt::kDiagPerf, "TIMING last_frame_id=%u last_pub_frame_id=%u last_capture_age_ms=%.1f "
                       "uart_write_start_age_last=%d uart_write_start_age_avg=%.2f "
                       "uart_write_start_age_max=%d uart_write_start_age_n=%lu\n",
                       g_capture_frame_id,g_last_pub_frame_id,cap_age_ms,as.last_ms,as.avg_ms,as.max_ms,as.samples);
                // 原始 PTS 只记录：板端实测单位是微秒（80fps 时每帧差≈12500），但代码不做换算，
                // 只用于观察单调性/重复；换传感器或驱动后需重新确认。
                const uint64_t win_delta=last_window_pts?(lastpts-last_window_pts):0;
                DIAGF(gt::kDiagPerf, "PTS_STAT last_raw=%llu window_delta_raw=%llu per_frame_raw=%.1f duplicate_pts=%lu\n",
                       static_cast<unsigned long long>(lastpts),static_cast<unsigned long long>(win_delta),
                       (count>last_frame_count)?static_cast<double>(win_delta)/static_cast<double>(count-last_frame_count):0.0,dups);
            }

            // 本窗口的实际帧率（host 侧 / 按 PTS 推算）
            double wfps=(count-last_frame_count)/(t-report);
            double wpts=(last_window_pts&&lastpts>last_window_pts)?(count-last_frame_count)*1000000.0/(lastpts-last_window_pts):0;
            DIAGF(gt::kDiagPerf, "CLOCK36_WINDOW elapsed_s=%.2f frames=%lu host_fps=%.4f pts_fps=%.4f\n",
                   t-start,count-last_frame_count,wfps,wpts);

            // 窗口基准推进
            last_frame_count=count;
            last_window_pts=lastpts;
            last_encoded=enc;
            last_sent=sent;
            last_bytes=bytes;
            report=t;
        }
        // 健康行 1 Hz：答「这一场为什么没记到」——板子活着吗、电控在发帧吗
        if(g_log.running() && (g_last_health_s<0.0 || t-g_last_health_s>=1.0)){
            g_last_health_s=t;
            g_log.on_health((count>1&&last>first)?(count-1)/(last-first):0.0,count,
                            encoded.load(),g_uart.rx_frames(),g_uart.mcu_age_ms());
        }
        // 运行状态心跳 30 秒一条：进程若被 kill/掉电，run_history 里最后一条就是"活到哪一刻"
        if(t-last_beat_s>=30.0){
            last_beat_s=t;
            const int temp_c=gt::diag_cpu_temp_c();
            gt::diag_progress(count,encoded.load(),temp_c);
            gt::diag_state("alive","frames=%lu fps=%.2f encoded=%lu temp=%dC mcu_age=%dms log_open=%d",
                           count,(count>1&&last>first)?(count-1)/(last-first):0.0,
                           encoded.load(),temp_c,g_uart.mcu_age_ms(),
                           g_log.running()?(int)g_log.stats().open:-1);
        }
    }
    // ======================================================================
    // 7. 收尾：顺序有讲究——先停生产者（编码线程/串口），再停消费者（记录器、HTTP），
    //    最后才拆 VI/ISP，否则会出现"往已销毁的通道写帧"。
    // 每一步都打一行 SHUTDOWN step，万一卡住能立刻看出卡在哪一步
    // （之前 exit_after_land 测试就卡过一次，没有任何时间线，只能靠猜）。
    // ======================================================================
    const gt::MonoUs shut_t0=gt::monotonic_us();
    DIAGF(gt::kDiagLog,"SHUTDOWN step=1 encoder_join begin\n");
    if(encoder_thread.joinable())encoder_thread.join();
    DIAGF(gt::kDiagLog,"SHUTDOWN step=2 uart_stop begin (+%.0fms)\n",(gt::monotonic_us()-shut_t0)/1000.0);
    g_uart.stop();                       // 串口线程会往 tty 写，必须先停
    DIAGF(gt::kDiagLog,"SHUTDOWN step=3 avi_finish begin (+%.0fms)\n",(gt::monotonic_us()-shut_t0)/1000.0);
    if(recording_enabled && !avi.finish())recording_failed=1;
    DIAGF(gt::kDiagLog,"SHUTDOWN step=4 log_stop begin (+%.0fms)\n",(gt::monotonic_us()-shut_t0)/1000.0);
    g_log.stop();                        // 记录器最后停：把缓冲里剩下的行写完并收尾 meta.txt
    DIAGF(gt::kDiagLog,"SHUTDOWN step=5 release_pending begin (+%.0fms)\n",(gt::monotonic_us()-shut_t0)/1000.0);
    {
        std::lock_guard<std::mutex> guard(input_mutex);
        if(has_pending){
            CVI_VI_ReleaseChnFrame(0,0,&pending);
            has_pending=false;
        }
    }
    if(server_thread.joinable())server_thread.join();
    if(created){
        CVI_VENC_StopRecvFrame(0);
        CVI_VENC_DestroyChn(0);
    }
    SAMPLE_COMM_VI_DestroyIsp(&cfg);
    SAMPLE_COMM_VI_DestroyVi(&cfg);
    SAMPLE_COMM_SYS_Exit();

    printf("stopped frames=%lu encoded=%lu errors=%lu tuning_failed=%d recording_failed=%d\n",
           count,encoded.load(),errors.load(),tuning_failed,recording_failed.load());

    // ======================================================================
    // 退出归因：自启动时这是"为什么停了"的唯一可靠来源（写进 state/run_history.log）
    // ======================================================================
    {
        const char* reason="normal";
        if(tuning_failed)reason="tuning_failed";
        else if(recording_failed.load())reason="recording_failed";
        else if(errors.load())reason="venc_errors";
        else if(g_stop_signal)reason="stopped_by_signal";
        const int ret=rc||errors||tuning_failed||recording_failed?1:0;
        gt::diag_state("exit","rc=%d reason=%s frames=%lu encoded=%lu errors=%lu "
                       "tuning_failed=%d recording_failed=%d stop_signal=%d timed_out=%lu temp=%dC",
                       ret,reason,count,encoded.load(),errors.load(),(int)tuning_failed,
                       recording_failed.load(),(int)g_stop_signal,timeouts,gt::diag_cpu_temp_c());
        return ret;
    }
}
