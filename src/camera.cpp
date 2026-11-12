// fly_LicheeRV native GC4653 preview, copied from verified record80 VI/VENC/MJPEG pipeline.
// No NPU inference or JavaScript drawing. Optional MJPEG AVI recording.
// TODO: insert C++ native algorithm at VI-frame boundary.
// Attribute/packet conventions: official sample_common_venc.c (see PREVIEW-HD.md).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <ctime>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "avi_mjpeg.h"
#include "green_detect.h"
#include "target_track.h"
#include "uart_out.h"
#include "frame_timing.h"
// 绿色圆灯检测：主循环里抽点下采样 → 多候选筛选 → 卡尔曼 → 角度 → 串口
static greendet::Detector g_detector;      // 多候选收集（不门控）
static gt::TargetTracker g_target;         // 多目标筛选 + 卡尔曼 + 状态机
static gt::UartOut g_uart;                 // 发给电控（独立线程）
static bool g_track_on = true;             // track_enable：关闭时退回旧的单目标行为
static int g_max_candidates = 8;
static unsigned long green_frames=0,green_hits=0,green_reports=0;
static double det_total_ms=0.0; static unsigned long det_count=0;
static double samp_total_ms=0.0, track_total_ms=0.0;
static int g_detect_stride=1; static unsigned long g_frame_idx=0;
static int g_sample_steps=2;   // 每块 n×n 采样点数（1..4）
static bool g_detect_on=true;  // detect_enable：false 时完全不跑检测
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
static unsigned long generation=0;
static std::mutex input_mutex;
static VIDEO_FRAME_INFO_S pending{};
static bool has_pending=false;
static std::atomic<unsigned long> encoded(0),errors(0);
static std::atomic<unsigned long> network_sent(0),network_bytes(0),preview_replaced(0);
static bool recording_enabled=false;
// display_enable: false 时不启动编码线程与 HTTP 服务（实战模式，省电降温）
static bool g_display_on=true;
static std::string recording_dir;
static std::atomic<int> recording_failed(0);
static AviMjpegWriter avi;
// Future: board-side native detection code will be added here; current code performs capture only.
static void stop(int){running=0;}
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
    ssize_t n=recv(fd,request,sizeof(request)-1,0);
    if(n<=0){close(fd);return;}
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
        close(fd);return;
    }
    if(stream){
        const char hdr[]="HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store, no-cache\r\nPragma: no-cache\r\nConnection: close\r\n\r\n";
        if(!sendall(fd,hdr,sizeof(hdr)-1)){close(fd);return;}
    }
    unsigned long seen=0;
    while(running){
        std::vector<unsigned char> jpg;
        {std::lock_guard<std::mutex> guard(jpeg_mutex);if(generation!=seen){seen=generation;jpg=latest;}}
        if(jpg.empty()){usleep(5000);continue;}
        char hdr[256];
        int count=snprintf(hdr,sizeof(hdr),snapshot?
            "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n":
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",jpg.size());
        if(count<=0 || !sendall(fd,hdr,static_cast<size_t>(count)) || !sendall(fd,jpg.data(),jpg.size()))break;
        ++network_sent;network_bytes+=jpg.size();
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
            sendall(fd,busy,sizeof(busy)-1);close(fd);continue;
        }
        slot->active=true;
        slot->worker=std::thread([fd,slot](){http_client(fd);slot->active=false;});
    }
    close(server);
    for(auto& slot:slots)if(slot.worker.joinable())slot.worker.join();
}
static void encoder_worker(){
    while(running){
        VIDEO_FRAME_INFO_S frame{};bool got=false;
        {std::lock_guard<std::mutex> guard(input_mutex);if(has_pending){frame=pending;has_pending=false;got=true;}}
        if(!got){usleep(1000);continue;}
        VENC_RECV_PIC_PARAM_S receive{};receive.s32RecvPicNum=1;
        int e=CVI_VENC_StartRecvFrame(0,&receive);
        if(!e)e=CVI_VENC_SendFrame(0,&frame,1000);
        if(!e){
            VENC_PACK_S packets[64]{};VENC_STREAM_S stream{};stream.pstPack=packets;stream.u32PackCount=64;
            e=CVI_VENC_GetStream(0,&stream,1000);
            if(!e){
                std::vector<unsigned char> jpg;
                for(unsigned i=0;i<stream.u32PackCount;i++){const auto& p=packets[i];if(p.pu8Addr && p.u32Len>=p.u32Offset)jpg.insert(jpg.end(),p.pu8Addr+p.u32Offset,p.pu8Addr+p.u32Len);}
                CVI_VENC_ReleaseStream(0,&stream);
                if(jpg.size()<4 || jpg[0]!=0xff || jpg[1]!=0xd8)e=-1;
                else{
                    // Persist the same hardware JPEG frame sent to the browser: no second VI/VENC.
                    if(recording_enabled && !avi.write(recording_dir,jpg.data(),jpg.size(),frame.stVFrame.u64PTS)){
                        recording_failed=1;
                        running=0;
                    }
                    {std::lock_guard<std::mutex> guard(jpeg_mutex);latest.swap(jpg);++generation;}
                    ++encoded;
                }
            }
        }
        CVI_VENC_StopRecvFrame(0);
        CVI_VI_ReleaseChnFrame(0,0,&frame);
        if(e){++errors;fprintf(stderr,"VENC encode rc=0x%x\n",e);if(errors>=5)running=0;}
    }
    std::lock_guard<std::mutex> guard(input_mutex);
    if(has_pending){CVI_VI_ReleaseChnFrame(0,0,&pending);has_pending=false;}
}
int main(int argc,char** argv){
    // Public entrypoint delegates to the supervisor for YAML, clock, lock and cleanup.
    if(argc==2 && (!strcmp(argv[1],"--recoding") || !strcmp(argv[1],"--recording"))){
        execl("/bin/sh","sh","/code/fly_LicheeRV/start.sh","--recoding",static_cast<char*>(nullptr));
        perror("Cannot launch recording supervisor");return 2;
    }
    if(argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))){
        printf("Usage: ./camera --recoding (alias --recording)\n"
               "       sh start.sh [--recoding|seconds] (default preview 120 seconds)\n");return 0;
    }
    if((argc!=3 && argc!=4 && argc!=5) ||
       (argc==5 && strcmp(argv[4],"--recording-internal"))){
        fprintf(stderr,"Usage: %s USB_BIND_IP OUTPUT_DIR [80] [--recording-internal]\n",argv[0]);return 2;
    }
    recording_enabled=argc==5;
    if(recording_enabled)recording_dir=argv[2];
    const char* mode="80";
    if(argc>=4 && strcmp(argv[3],"80"))return 2;
    const char* disp=getenv("DISPLAY_ENABLE");
    if(disp && !strcmp(disp,"false"))g_display_on=false;
    if(getenv("DETECT_STRIDE")){int v=atoi(getenv("DETECT_STRIDE"));if(v>=1&&v<=10)g_detect_stride=v;}
    if(getenv("DETECT_SAMPLE_STEPS")){int v=atoi(getenv("DETECT_SAMPLE_STEPS"));if(v>=1&&v<=4)g_sample_steps=v;}
    if(getenv("DETECT_ENABLE") && !strcmp(getenv("DETECT_ENABLE"),"false"))g_detect_on=false;
    if(getenv("DRAW_TEST_BOX") && !strcmp(getenv("DRAW_TEST_BOX"),"1"))g_draw_test_box=true;

    // ---- 阶段1：多目标筛选 + 卡尔曼 + 角度 + 串口 ----
    // 全部参数来自 YAML（start.sh 转成环境变量）。非法值一律报错退出，
    // 避免「悄悄用默认值」导致联调时数据算错却没人发现。
    bool track_on=true, uart_on=false, inv_yaw=false, inv_pitch=false;
    float cam_fx=1024.0f, cam_fy=1024.0f, cam_cx=640.0f, cam_cy=360.0f;
    float opt_yaw=0.0f, opt_pitch=0.0f, target_dia=0.05f;
    float aim_up=1.5f;   // 瞄准点向上偏移系数（= 装甲板中心到灯中心距离 / 灯直径），抄 HERO 的 1.5
    float sel_conf=0.5f, sel_aspect=1.5f, sel_maxdist=60.0f;
    float kf_q=50.0f, kf_r=0.001f, kf_initpos=10.0f, kf_initvel=2.0f;
    unsigned sel_confirm=5, sel_lost=10, sel_maxcand=8;
    unsigned uart_baud=115200, uart_rate=50;
    const char* uart_dev=getenv("UART_DEVICE"); if(!uart_dev||!*uart_dev)uart_dev="/dev/ttyS1";
    // 串口帧格式：ascii = v1 文本帧（$GT,...，串口助手可直接看）；
    //             binary = v2 定长二进制帧（28 字节 A5 5A ... CRC16，见 uart_frame.h）
    // 默认 ascii，保证不配置时线上字节行为与之前完全一致。
    const char* uart_format=getenv("UART_FORMAT"); if(!uart_format||!*uart_format)uart_format="ascii";
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
            else if(strcmp(rp,"true")){fprintf(stderr,"Invalid UART_RX_PRINT '%s' (expect true|false)\n",rp);return 2;}
        }
        const char* rh=getenv("UART_RX_PRINT_HZ");
        if(rh&&*rh){
            const int v=atoi(rh);
            if(v<1||v>100){fprintf(stderr,"Invalid UART_RX_PRINT_HZ '%s' (expect 1..100)\n",rh);return 2;}
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
    if(!ok){fprintf(stderr,"Invalid track/uart parameter in environment (check config.yaml)\n");return 2;}
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
        printf("TRACK calib fx=%.2f fy=%.2f cx=%.2f cy=%.2f opt=%.2f/%.2f inv=%d/%d "
               "confirm=%u lost=%u conf_w=%.2f aspect=%.2f dia=%.3fm aimup=%.2f\n",
               cam_fx,cam_fy,cam_cx,cam_cy,opt_yaw,opt_pitch,(int)inv_yaw,(int)inv_pitch,
               sel_confirm,sel_lost,sel_conf,sel_aspect,target_dia,aim_up);
    }

    if(uart_on && !track_on){
        fprintf(stderr,"uart_enable requires track_enable (nothing to send otherwise)\n");
        return 2;
    }
    if(uart_on){
        gt::UartCfg uc; uc.enable=true; uc.device=uart_dev;
        uc.baud=static_cast<int>(uart_baud); uc.rate_hz=static_cast<int>(uart_rate);
        uc.ascii=!uart_binary;   // false → 发 v2 二进制帧
        uc.rx_print=uart_rx_print;               // 收到电控帧就打印
        uc.rx_print_hz=static_cast<int>(uart_rx_hz);
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
    printf("FLY_LICHEERV camera_preview=true inference=disabled recording=%s\n",
           recording_enabled?"enabled":"disabled");
    int server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return 2;
    int reuse=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(8081);
    const char*web_ip=getenv("WEB_IP_BIND")?getenv("WEB_IP_BIND"):argv[1];
    if(!strcmp(web_ip,"auto"))address.sin_addr.s_addr=INADDR_ANY;
    else if(inet_pton(AF_INET,web_ip,&address.sin_addr)!=1){perror("HTTP parse");close(server);return 2;}
    if(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(server,4)){perror("HTTP bind");close(server);return 2;}
    printf("WEB_BIND ip=%s\n",web_ip);
    setenv("MAIX_SENSOR_FPS",mode,1);
    printf("sensor_mode=%s preview=1280x720 quality=%u preview_limiter=none exposure_mode=%s\n",mode,quality,exposure_mode);
    printf("RECORD80_BLC_CONFIG mode=%s offsets=%u,%u,%u,%u\n",blc_mode,blc_r,blc_gr,blc_gb,blc_b);
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
    printf("DIAG configured_compress=%d\n",cfg.astViInfo[0].stChnInfo.enCompressMode);
    printf("STEP sys_init begin\n");fflush(stdout);
    SIZE_S size = mode_720p ? SIZE_S{1280,720} : SIZE_S{2560,1440};
    printf("SENSOR_MODE %s size=%ux%u detect=%s display=%s\n",
           mode_720p?"720P_60FPS":"4M_30FPS", size.u32Width, size.u32Height,
           g_detect_on?"on":"off", g_display_on?"on":"off");
    /* VB pool follows the VI channel size: production 720P mode is 1280x720. */
    int sys_rc = SAMPLE_PLAT_SYS_INIT(size);
    printf("POOL_SIZE %ux%u sys_init_rc=%d\n", size.u32Width, size.u32Height, sys_rc);
    fflush(stdout);
    if(sys_rc){close(server);return 4;}
    int vi_rc=SAMPLE_PLAT_VI_INIT(&cfg);
    printf("STEP vi_init rc=%d\n",vi_rc);fflush(stdout);
    if(vi_rc){SAMPLE_COMM_SYS_Exit();close(server);return 5;}
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    VI_CHN_ATTR_S vi{};int rc=CVI_VI_GetChnAttr(0,0,&vi);vi.u32Depth=2;if(!rc)rc=CVI_VI_SetChnAttr(0,0,&vi);
    VI_PIPE_ATTR_S pipe{};int prc=CVI_VI_GetPipeAttr(0,&pipe);
    printf("DIAG channel_rc=%x compress=%d video=%d depth=%u pipe_rc=%x pipe_compress=%d\n",rc,vi.enCompressMode,vi.enVideoFormat,vi.u32Depth,prc,pipe.enCompressMode);
    VENC_CHN_ATTR_S attr{};
    // JPEG single-frame mode has no MJPEG frame-rate-control field.
    attr.stVencAttr.enType=PT_JPEG;
    attr.stVencAttr.u32MaxPicWidth=attr.stVencAttr.u32PicWidth=size.u32Width;
    attr.stVencAttr.u32MaxPicHeight=attr.stVencAttr.u32PicHeight=size.u32Height;
    attr.stVencAttr.u32BufSize=2*1024*1024;attr.stVencAttr.bByFrame=CVI_TRUE;
    attr.stVencAttr.stAttrJpege.bSupportDCF=CVI_FALSE;
    attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum=0;
    attr.stVencAttr.stAttrJpege.enReceiveMode=VENC_PIC_RECEIVE_SINGLE;
    attr.stGopAttr.enGopMode=VENC_GOPMODE_NORMALP;
    bool created=false;
    if(g_display_on){
        if(!rc){rc=CVI_VENC_CreateChn(0,&attr);created=rc==0;printf("VENC CreateChn rc=0x%x\n",rc);}
        if(!rc){VENC_JPEG_PARAM_S jpeg{};rc=CVI_VENC_GetJpegParam(0,&jpeg);if(!rc){jpeg.u32Qfactor=quality;rc=CVI_VENC_SetJpegParam(0,&jpeg);}printf("VENC JPEG quality=%u rc=0x%x\n",quality,rc);}
        if(rc)running=0;
    }else{
        printf("VENC skipped (display_enable=false)\n");
    }
    std::thread server_thread;
    std::thread encoder_thread;
    if(g_display_on){
        server_thread=std::thread(http_server,server);
        encoder_thread=std::thread(encoder_worker);
    }
    printf("CAMERA_READY display=%s preview=%s recorder=%s\n",
           g_display_on?"on":"off", g_display_on?argv[1]:"none",
           recording_enabled?"on":"off");
    unsigned long count=0,dups=0,timeouts=0;uint64_t firstpts=0,lastpts=0;
    double start=now(),first=0,last=0,report=start;
    bool diag_applied=false;
    bool tuning_failed=false;bool blc_check_done=false;double last_ae_report=0;
    unsigned long last_encoded=0,last_sent=0,last_bytes=0,last_frame_count=0;uint64_t last_window_pts=0;
    while(running){
        VIDEO_FRAME_INFO_S frame{};
        if(CVI_VI_GetChnFrame(0,0,&frame,1000)){if(running)++timeouts;continue;}
        // 取帧成功立刻建立帧元数据：帧号 + 原始 PTS + 采集时刻（不是曝光时刻）。
        // 后面所有异步环节（串口线程）只拿这份 POD 拷贝，绝不持有 VIDEO_FRAME_INFO_S。
        gt::FrameTiming timing{};
        timing.frame_id=++g_capture_frame_id;
        timing.vi_pts_raw=frame.stVFrame.u64PTS;
        timing.capture_mono_us=gt::monotonic_us();
        double t=now();
        if(!diag_applied && t-start>2){
            diag_applied=true;
            ISP_EXPOSURE_ATTR_S ae{};int ae_rc=CVI_ISP_GetExposureAttr(0,&ae);
            printf("RECORD80_AE_BEFORE rc=0x%x mode=%d exp_min=%u exp_max=%u\n",ae_rc,ae.enOpType,ae.stAuto.stExpTimeRange.u32Min,ae.stAuto.stExpTimeRange.u32Max);
            if(!ae_rc && !strcmp(exposure_mode,"manual")){
                ae.enOpType=OP_TYPE_MANUAL;ae.stManual.enGainType=AE_TYPE_GAIN;
                ae.stManual.enExpTimeOpType=ae.stManual.enAGainOpType=ae.stManual.enDGainOpType=ae.stManual.enISPDGainOpType=OP_TYPE_MANUAL;
                ae.stManual.u32ExpTime=manual_exp;ae.stManual.u32AGain=manual_again;
                ae.stManual.u32DGain=manual_dgain;ae.stManual.u32ISPDGain=manual_ispgain;
                ae_rc=CVI_ISP_SetExposureAttr(0,&ae);
                printf("EXPOSURE_SET us=%u again=%u dgain=%u isp_gain=%u rc=%x\n",manual_exp,manual_again,manual_dgain,manual_ispgain,ae_rc);
             }else if(!ae_rc && !strcmp(exposure_mode,"auto")){
                ae.enOpType=OP_TYPE_AUTO;
                if(auto_exp){ae.stAuto.stExpTimeRange.u32Max=auto_exp;if(ae.stAuto.stExpTimeRange.u32Min>auto_exp)ae.stAuto.stExpTimeRange.u32Min=auto_exp;}
                if(auto_again){ae.stAuto.stAGainRange.u32Max=auto_again;if(ae.stAuto.stAGainRange.u32Min>auto_again)ae.stAuto.stAGainRange.u32Min=auto_again;}
                if(auto_dgain){ae.stAuto.stDGainRange.u32Max=auto_dgain;if(ae.stAuto.stDGainRange.u32Min>auto_dgain)ae.stAuto.stDGainRange.u32Min=auto_dgain;}
                if(auto_ispgain){ae.stAuto.stISPDGainRange.u32Max=auto_ispgain;if(ae.stAuto.stISPDGainRange.u32Min>auto_ispgain)ae.stAuto.stISPDGainRange.u32Min=auto_ispgain;}
                ae_rc=CVI_ISP_SetExposureAttr(0,&ae);
                printf("RECORD80_AE_AUTO_LIMIT exp_max=%u again_max=%u dgain_max=%u isp_gain_max=%u rc=0x%x\n",auto_exp,auto_again,auto_dgain,auto_ispgain,ae_rc);
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
                printf("RECORD80_BLC_SET offsets=%u,%u,%u,%u rc=0x%x readback_match=%d\n",blc_r,blc_gr,blc_gb,blc_b,be,match);
                if(be || !match){tuning_failed=true;running=0;}
            }
            ISP_PUB_ATTR_S pub{};int pe=CVI_ISP_GetPubAttr(0,&pub);printf("DIAG ISP pub fps=%f get=%x\n",pub.f32FrameRate,pe);
            if(!pe && getenv("PREVIEW_DIAG_PUBFPS")){pub.f32FrameRate=atof(getenv("PREVIEW_DIAG_PUBFPS"));printf("DIAG set ISP pub fps=%f rc=%x\n",pub.f32FrameRate,CVI_ISP_SetPubAttr(0,&pub));}
            if(getenv("PREVIEW_DIAG_NOTNR")){ISP_TNR_ATTR_S a{};int e=CVI_ISP_GetTNRAttr(0,&a);printf("DIAG TNR enabled=%d get=%x\n",a.Enable,e);if(!e){a.Enable=CVI_FALSE;printf("DIAG disable TNR=%x\n",CVI_ISP_SetTNRAttr(0,&a));}}
            if(getenv("PREVIEW_DIAG_NODPC")){ISP_DP_DYNAMIC_ATTR_S a{};ISP_DP_STATIC_ATTR_S b{};int e=CVI_ISP_GetDPDynamicAttr(0,&a);printf("DIAG DPC dynamic=%d get=%x\n",a.Enable,e);if(!e){a.Enable=CVI_FALSE;printf("DIAG disable dynamicDPC=%x\n",CVI_ISP_SetDPDynamicAttr(0,&a));}e=CVI_ISP_GetDPStaticAttr(0,&b);printf("DIAG DPC static=%d get=%x\n",b.Enable,e);if(!e){b.Enable=CVI_FALSE;printf("DIAG disable staticDPC=%x\n",CVI_ISP_SetDPStaticAttr(0,&b));}}
        }
        if(t-last_ae_report>=5){
            last_ae_report=t;ISP_EXP_INFO_S exp{};int e=CVI_ISP_QueryExposureInfo(0,&exp);
            printf("RECORD80_AE_INFO elapsed_s=%.2f rc=0x%x exp_us=%u again=%u dgain=%u isp_gain=%u luma=%u\n",t-start,e,exp.u32ExpTime,exp.u32AGain,exp.u32DGain,exp.u32ISPDGain,exp.u8AveLum);
        }
        if(!blc_check_done && !strcmp(blc_mode,"manual") && t-start>10){
            blc_check_done=true;ISP_INNER_STATE_INFO_S inner{};
            int be=CVI_ISP_QueryInnerStateInfo(0,&inner);
            bool match=!be && inner.blcOffsetR==blc_r && inner.blcOffsetGr==blc_gr &&
                inner.blcOffsetGb==blc_gb && inner.blcOffsetB==blc_b;
            printf("RECORD80_BLC_VERIFY elapsed_s=%.2f rc=0x%x offsets=%u,%u,%u,%u match=%d\n",t-start,be,inner.blcOffsetR,inner.blcOffsetGr,inner.blcOffsetGb,inner.blcOffsetB,match);
            if(!match){tuning_failed=true;running=0;}
        }
        if(t-start<4){CVI_VI_ReleaseChnFrame(0,0,&frame);continue;}
        if(!count){first=t;firstpts=frame.stVFrame.u64PTS;printf("VI geometry=%ux%u stride=%u,%u format=%d compress=%d video=%d lengths=%u,%u,%u\n",frame.stVFrame.u32Width,frame.stVFrame.u32Height,frame.stVFrame.u32Stride[0],frame.stVFrame.u32Stride[1],frame.stVFrame.enPixelFormat,frame.stVFrame.enCompressMode,frame.stVFrame.enVideoFormat,frame.stVFrame.u32Length[0],frame.stVFrame.u32Length[1],frame.stVFrame.u32Length[2]);}
        else if(frame.stVFrame.u64PTS<=lastpts)++dups;
        last=t;lastpts=frame.stVFrame.u64PTS;++count;
        {
            // ---- 绿色圆灯检测（每帧，抽点 4 → 320x180 网格）----
            // 说明：VI 帧为 NV12（UV 交织）。抽点 Y 与 UV，步长 4；
            //       U/V 顺序对绿色判据无影响（两者都要求 < 128）。
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
                track_total_ms+=(now()-td1)*1000.0;
                // ---- ④ 把选中目标的框叠加到 Y 平面（编码前，网页与录像都能看到）----
                // 纯亮度写入，不涉及色彩转换；框宽 3px，最小半径 12px 保证远处可见。
                if(g_display_on && tgt.has_box){
                    int cxp=(int)(tgt.gx*S), cyp=(int)(tgt.gy*S);
                    int rp=(int)(tgt.gr*S)+6; if(rp<12)rp=12;
                    int x0=cxp-rp, x1=cxp+rp, y0=cyp-rp, y1=cyp+rp;
                    if(x0<0)x0=0; if(y0<0)y0=0;
                    if(x1>(int)W-2)x1=(int)W-2; if(y1>(int)H-2)y1=(int)H-2;
                    unsigned char*Yw=(unsigned char*)Yp;
                    for(int t=0;t<3;++t){
                        int ya=y0+t, yb=y1-t, xa=x0+t, xb=x1-t;
                        if(ya>=0&&ya<(int)H)for(int x=x0;x<=x1;++x)Yw[(size_t)ya*ystride+x]=255;
                        if(yb>=0&&yb<(int)H)for(int x=x0;x<=x1;++x)Yw[(size_t)yb*ystride+x]=255;
                        if(xa>=0&&xa<(int)W)for(int y=y0;y<=y1;++y)Yw[(size_t)y*ystride+xa]=255;
                        if(xb>=0&&xb<(int)W)for(int y=y0;y<=y1;++y)Yw[(size_t)y*ystride+xb]=255;
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
                            if(yb>=0)Yw[(size_t)yb*ystride+x]=255;}
                        for(int y=by0;y<=by1;++y){
                            if(xa<(int)W)Yw[(size_t)y*ystride+xa]=255;
                            if(xb>=0)Yw[(size_t)y*ystride+xb]=255;}
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
                        printf("GREEN_TARGET state=%s yaw=%.2f pitch=%.2f vyaw=%.2f vpitch=%.2f "
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
                const double det_ms=(now()-td0)*1000.0;
                det_total_ms+=det_ms; if(det_ms>g_det_max_ms)g_det_max_ms=det_ms;
                const double proc_ms=timing.detect_duration_us()/1000.0;
                g_proc_total_ms+=proc_ms; if(proc_ms>g_proc_max_ms)g_proc_max_ms=proc_ms;
                const double lat_ms=timing.capture_latency_us()/1000.0;
                g_lat_total_ms+=lat_ms; if(lat_ms>g_lat_max_ms)g_lat_max_ms=lat_ms;
                ++det_count;
                if(green_frames%300==0){
                    printf("GREEN_STAT frames=%lu hits=%lu rate=%.1f%%\n",
                           green_frames,green_hits,100.0*green_hits/(green_frames?green_frames:1));
                }
            }
            if(g_display_on){
                std::lock_guard<std::mutex> guard(input_mutex);
                if(has_pending){CVI_VI_ReleaseChnFrame(0,0,&pending);++preview_replaced;}
                pending=frame;has_pending=true;
            }else{
                CVI_VI_ReleaseChnFrame(0,0,&frame);   // 无消费者，直接归还
            }
        }
        if(t-report>=5 && count>1){
            unsigned long enc=encoded.load(),sent=network_sent.load(),bytes=network_bytes.load();
            printf("capture_frames=%lu host_fps=%.4f pts_fps=%.4f duplicate_pts=%lu timeouts=%lu encoded=%lu encode_errors=%lu encode_fps=%.3f sent_fps=%.3f jpeg_mbit_s=%.3f preview_replaced=%lu\n",count,(count-1)/(last-first),lastpts>firstpts?(count-1)*1e6/(lastpts-firstpts):0,dups,timeouts,enc,errors.load(),(enc-last_encoded)/(t-report),(sent-last_sent)/(t-report),(bytes-last_bytes)*8e-6/(t-report),preview_replaced.load());
            if(det_count){printf("DETECT avg_ms=%.3f max_ms=%.3f proc_avg_ms=%.3f proc_max_ms=%.3f lat_avg_ms=%.3f lat_max_ms=%.3f calls=%lu stride=%d map_miss=%lu sample_ms=%.3f track_ms=%.3f ns=%d cand_max=%d\n",det_total_ms/det_count,g_det_max_ms,g_proc_total_ms/det_count,g_proc_max_ms,g_lat_total_ms/det_count,g_lat_max_ms,det_count,g_detect_stride,g_map_miss,samp_total_ms/det_count,track_total_ms/det_count,g_sample_steps,g_max_candidates);det_total_ms=0;det_count=0;samp_total_ms=0;track_total_ms=0;g_det_max_ms=0;g_proc_total_ms=0;g_proc_max_ms=0;g_lat_total_ms=0;g_lat_max_ms=0;}
            if(g_uart.running()){
                const unsigned long us=g_uart.sent_frames();
                printf("UART sent=%lu rate=%.2fHz errors=%lu | rx_frames=%lu rx_crc=%lu rx_other=%lu rx_age=%dms\n",
                       us,(us-g_uart_sent_prev)/(t-report),g_uart.send_errors(),
                       g_uart.rx_frames(),g_uart.rx_crc_errors(),g_uart.rx_other_frames(),g_uart.mcu_age_ms());
                g_uart_sent_prev=us;
            }
            {
                // 帧时间戳：相机帧号、最近发布帧的年龄、串口「取帧→开始写」年龄
                // （后者只做诊断，不写进 ASCII 的 age 字段）
                const gt::WriteStartAgeStats as=g_uart.take_write_start_age_stats();
                const double cap_age_ms=g_last_pub_capture_us?static_cast<double>(gt::monotonic_us()-g_last_pub_capture_us)/1000.0:-1.0;
                printf("TIMING last_frame_id=%u last_pub_frame_id=%u last_capture_age_ms=%.1f "
                       "uart_write_start_age_last=%d uart_write_start_age_avg=%.2f "
                       "uart_write_start_age_max=%d uart_write_start_age_n=%lu\n",
                       g_capture_frame_id,g_last_pub_frame_id,cap_age_ms,as.last_ms,as.avg_ms,as.max_ms,as.samples);
                // 原始 PTS 只记录：板端实测单位是微秒（80fps 时每帧差≈12500），但代码不做换算，
                // 只用于观察单调性/重复；换传感器或驱动后需重新确认。
                const uint64_t win_delta=last_window_pts?(lastpts-last_window_pts):0;
                printf("PTS_STAT last_raw=%llu window_delta_raw=%llu per_frame_raw=%.1f duplicate_pts=%lu\n",
                       static_cast<unsigned long long>(lastpts),static_cast<unsigned long long>(win_delta),
                       (count>last_frame_count)?static_cast<double>(win_delta)/static_cast<double>(count-last_frame_count):0.0,dups);
            }
            double wfps=(count-last_frame_count)/(t-report);double wpts=(last_window_pts&&lastpts>last_window_pts)?(count-last_frame_count)*1000000.0/(lastpts-last_window_pts):0;printf("CLOCK36_WINDOW elapsed_s=%.2f frames=%lu host_fps=%.4f pts_fps=%.4f\n",t-start,count-last_frame_count,wfps,wpts);last_frame_count=count;last_window_pts=lastpts;last_encoded=enc;last_sent=sent;last_bytes=bytes;report=t;
        }
    }
    if(encoder_thread.joinable())encoder_thread.join();
    g_uart.stop();   // 先停串口，避免退出过程中继续往 tty 写
    if(recording_enabled && !avi.finish())recording_failed=1;
    {std::lock_guard<std::mutex> guard(input_mutex);if(has_pending){CVI_VI_ReleaseChnFrame(0,0,&pending);has_pending=false;}}
    if(server_thread.joinable())server_thread.join();
    if(created){CVI_VENC_StopRecvFrame(0);CVI_VENC_DestroyChn(0);}
    SAMPLE_COMM_VI_DestroyIsp(&cfg);SAMPLE_COMM_VI_DestroyVi(&cfg);SAMPLE_COMM_SYS_Exit();
    printf("stopped frames=%lu encoded=%lu errors=%lu tuning_failed=%d recording_failed=%d\n",
           count,encoded.load(),errors.load(),tuning_failed,recording_failed.load());
    return rc||errors||tuning_failed||recording_failed?1:0;
}
