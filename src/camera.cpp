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
// 绿色圆灯检测：主循环里抽点下采样后调 greendet::Tracker（已板端验证 2.25ms/帧）
static greendet::Tracker g_tracker;
static unsigned long green_frames=0,green_hits=0,green_reports=0;
// NPU is intentionally not linked or implemented in this version.
extern "C" {
#include "sample_comm.h"
#include <cvi_sys.h>
#include "cvi_venc.h"
#include "cvi_isp.h"
#include "cvi_ae.h"
}
static bool env_u32(const char* name,unsigned fallback,unsigned low,unsigned high,unsigned& out){
    const char* s=getenv(name);if(!s){out=fallback;return true;}
    if(!*s)return false;
    errno=0;char* end=nullptr;unsigned long v=strtoul(s,&end,10);
    if(errno || *end || v<low || v>high)return false;
    out=static_cast<unsigned>(v);return true;
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
    // 传感器模式由板端 /mnt/data/sensor_cfg.ini 决定，本工程要求 720P@60(10bit)。
    // 说明：该模式的枚举常量来自板端镜像的 SDK，交叉编译所用的 SDK 没有同名符号，
    // 所以这里不硬编码覆盖，只校验"不是 4M@30 模式"。
    if(SAMPLE_COMM_VI_ParseIni(&ini)){close(server);return 3;}
    bool mode_4m30 = (ini.enSnsType[0]==GCORE_GC4653_MIPI_4M_30FPS_10BIT);
    bool mode_720p = (ini.enSnsType[0]==GCORE_GC4653_MIPI_720P_60FPS_10BIT);
    if(!mode_4m30 && !mode_720p){
        fprintf(stderr,"ERROR: sensor_cfg.ini 需为 GC4653 4M_30FPS 或 720P_60FPS\n");
        close(server);return 3;
    }
    CVI_VI_SetDevNum(ini.devNum);
    if(SAMPLE_COMM_VI_IniToViCfg(&ini,&cfg)){close(server);return 3;}
    if(getenv("PREVIEW_DIAG_LINEAR"))cfg.astViInfo[0].stChnInfo.enCompressMode=COMPRESS_MODE_NONE;
    printf("DIAG configured_compress=%d\n",cfg.astViInfo[0].stChnInfo.enCompressMode);
    printf("STEP sys_init begin\n");fflush(stdout);
    SIZE_S size = mode_4m30 ? SIZE_S{2560,1440} : SIZE_S{1280,720};
    printf("SENSOR_MODE %s size=%ux%u\n", mode_4m30?"4M_30FPS":"720P_60FPS", size.u32Width, size.u32Height);
    int sys_rc=SAMPLE_PLAT_SYS_INIT(size);
    printf("STEP sys_init rc=%d\n",sys_rc);fflush(stdout);
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
    if(!rc){rc=CVI_VENC_CreateChn(0,&attr);created=rc==0;printf("VENC CreateChn rc=0x%x\n",rc);}
    if(!rc){VENC_JPEG_PARAM_S jpeg{};rc=CVI_VENC_GetJpegParam(0,&jpeg);if(!rc){jpeg.u32Qfactor=quality;rc=CVI_VENC_SetJpegParam(0,&jpeg);}printf("VENC JPEG quality=%u rc=0x%x\n",quality,rc);}
    if(rc)running=0;
    std::thread server_thread(http_server,server);
    std::thread encoder_thread(encoder_worker);
    printf("CAMERA_READY preview=http://%s:8081/ inference=off recorder=%s\n",
           argv[1],recording_enabled?"on":"off");
    unsigned long count=0,dups=0,timeouts=0;uint64_t firstpts=0,lastpts=0;
    double start=now(),first=0,last=0,report=start;
    bool diag_applied=false;
    bool tuning_failed=false;bool blc_check_done=false;double last_ae_report=0;
    unsigned long last_encoded=0,last_sent=0,last_bytes=0,last_frame_count=0;uint64_t last_window_pts=0;
    while(running){
        VIDEO_FRAME_INFO_S frame{};
        if(CVI_VI_GetChnFrame(0,0,&frame,1000)){if(running)++timeouts;continue;}
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
                // 映射 Y + UV 平面（只需一次的物理地址→虚拟地址）
                unsigned sy=frame.stVFrame.u32Stride[0]*H;
                unsigned su=frame.stVFrame.u32Stride[1]*(H/2);
                frame.stVFrame.pu8VirAddr[0]=(unsigned char*)CVI_SYS_Mmap(frame.stVFrame.u64PhyAddr[0],sy);
                frame.stVFrame.pu8VirAddr[1]=(unsigned char*)CVI_SYS_Mmap(frame.stVFrame.u64PhyAddr[1],su);
            }
            static unsigned char gy[320*180],gu[160*90],gv[160*90];
            const int S=(W>=2000)?8:4,w=W/S,h=H/S,cw=w/2,ch=h/2;
            if(frame.stVFrame.pu8VirAddr[0] && w<=320 && h<=180){
                const unsigned char*Yp=frame.stVFrame.pu8VirAddr[0];
                const unsigned char*UVp=frame.stVFrame.pu8VirAddr[1];
                unsigned ystride=frame.stVFrame.u32Stride[0],uvstride=frame.stVFrame.u32Stride[1];
                for(int yy=0;yy<h;++yy){const unsigned char*r=Yp+(size_t)(yy*S)*ystride;
                    for(int xx=0;xx<w;++xx)gy[(size_t)yy*w+xx]=r[xx*S];}
                for(int yy=0;yy<ch;++yy){const unsigned char*r=UVp+(size_t)(yy*S)*uvstride;
                    for(int xx=0;xx<cw;++xx){gu[(size_t)yy*cw+xx]=r[(xx*S)*2];gv[(size_t)yy*cw+xx]=r[(xx*S)*2+1];}}
                ++green_frames;
                const greendet::TrackState& st=g_tracker.process(gy,gu,gv,w,h);
                if(st.status!=greendet::Status::kLost){
                    ++green_hits;
                    if(green_frames%60==0 || st.misses==0){
                        printf("GREEN_TARGET status=%s cx=%.1f cy=%.1f r=%.1f hits=%lu frames=%lu\n",
                               st.status==greendet::Status::kDetected?"detected":"coast",
                               st.cx*S,st.cy*S,st.blob.radius*S,green_hits,green_frames);
                    }
                }
                if(green_frames%300==0){
                    printf("GREEN_STAT frames=%lu hits=%lu rate=%.1f%%\n",
                           green_frames,green_hits,100.0*green_hits/(green_frames?green_frames:1));
                }
            }
            std::lock_guard<std::mutex> guard(input_mutex);
            if(has_pending){CVI_VI_ReleaseChnFrame(0,0,&pending);++preview_replaced;}
            pending=frame;has_pending=true;
        }
        if(t-report>=5 && count>1){
            unsigned long enc=encoded.load(),sent=network_sent.load(),bytes=network_bytes.load();
            printf("capture_frames=%lu host_fps=%.4f pts_fps=%.4f duplicate_pts=%lu timeouts=%lu encoded=%lu encode_errors=%lu encode_fps=%.3f sent_fps=%.3f jpeg_mbit_s=%.3f preview_replaced=%lu\n",count,(count-1)/(last-first),lastpts>firstpts?(count-1)*1e6/(lastpts-firstpts):0,dups,timeouts,enc,errors.load(),(enc-last_encoded)/(t-report),(sent-last_sent)/(t-report),(bytes-last_bytes)*8e-6/(t-report),preview_replaced.load());
            double wfps=(count-last_frame_count)/(t-report);double wpts=(last_window_pts&&lastpts>last_window_pts)?(count-last_frame_count)*1000000.0/(lastpts-last_window_pts):0;printf("CLOCK36_WINDOW elapsed_s=%.2f frames=%lu host_fps=%.4f pts_fps=%.4f\n",t-start,count-last_frame_count,wfps,wpts);last_frame_count=count;last_window_pts=lastpts;last_encoded=enc;last_sent=sent;last_bytes=bytes;report=t;
        }
    }
    encoder_thread.join();
    if(recording_enabled && !avi.finish())recording_failed=1;
    {std::lock_guard<std::mutex> guard(input_mutex);if(has_pending){CVI_VI_ReleaseChnFrame(0,0,&pending);has_pending=false;}}
    server_thread.join();
    if(created){CVI_VENC_StopRecvFrame(0);CVI_VENC_DestroyChn(0);}
    SAMPLE_COMM_VI_DestroyIsp(&cfg);SAMPLE_COMM_VI_DestroyVi(&cfg);SAMPLE_COMM_SYS_Exit();
    printf("stopped frames=%lu encoded=%lu errors=%lu tuning_failed=%d recording_failed=%d\n",
           count,encoded.load(),errors.load(),tuning_failed,recording_failed.load());
    return rc||errors||tuning_failed||recording_failed?1:0;
}
