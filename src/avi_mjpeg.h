// Standalone MJPEG AVI muxer. It reuses the exact JPEG bitstream sent to the web preview.
// No additional sensor acquisition, encoder channel, or ffmpeg process.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>

class AviMjpegWriter {
    struct Entry { uint32_t offset, size; };
    FILE* file_ = nullptr;
    std::string path_;
    std::vector<Entry> index_;
    long movi_size_pos_ = 0;
    long movi_fourcc_pos_ = 0;
    long avih_us_pos_ = 0;
    long avih_bps_pos_ = 0;
    long avih_frames_pos_ = 0;
    long strh_rate_pos_ = 0;
    long strh_length_pos_ = 0;
    uint64_t first_pts_ = 0, last_pts_ = 0;
    uint32_t max_frame_ = 0;
    bool failed_ = false;
    static bool put(FILE* f, const void* bytes, size_t n) {
        return fwrite(bytes, 1, n, f) == n;
    }
    static bool four(FILE* f, const char* s) { return put(f, s, 4); }
    static bool u16(FILE* f, uint16_t n) {
        unsigned char b[2] = {static_cast<unsigned char>(n),
                              static_cast<unsigned char>(n >> 8)};
        return put(f, b, 2);
    }
    static bool u32(FILE* f, uint32_t n) {
        unsigned char b[4] = {static_cast<unsigned char>(n),
                              static_cast<unsigned char>(n >> 8),
                              static_cast<unsigned char>(n >> 16),
                              static_cast<unsigned char>(n >> 24)};
        return put(f, b, 4);
    }
    bool patch(long offset, uint32_t value) {
        return fseek(file_, offset, SEEK_SET) == 0 && u32(file_, value);
    }
    bool start(const std::string& directory) {
        char stamp[32] = {};
        time_t t = time(nullptr);
        struct tm tmv{};
        localtime_r(&t, &tmv);
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
        path_ = directory + "/gc4653_" + stamp + "_" + std::to_string(static_cast<long>(getpid())) + ".avi";
        file_ = fopen(path_.c_str(), "wb");
        if (!file_) { perror("RECORDING_OPEN_FAILED"); return false; }
        bool ok = true;
        ok &= four(file_, "RIFF") && u32(file_, 0) && four(file_, "AVI ");
        ok &= four(file_, "LIST");
        long hdrl_size = ftell(file_);
        ok &= u32(file_, 0) && four(file_, "hdrl");
        ok &= four(file_, "avih") && u32(file_, 56);
        avih_us_pos_ = ftell(file_);
        ok &= u32(file_, 12500);  // replaced with the measured encoded FPS on close
        avih_bps_pos_ = ftell(file_);
        ok &= u32(file_, 0) && u32(file_, 0) && u32(file_, 0x10);
        avih_frames_pos_ = ftell(file_);
        ok &= u32(file_, 0) && u32(file_, 0) && u32(file_, 1);
        ok &= u32(file_, 0) && u32(file_, 1280) && u32(file_, 720);
        for (int i = 0; i < 4; ++i) ok &= u32(file_, 0);
        ok &= four(file_, "LIST");
        long strl_size = ftell(file_);
        ok &= u32(file_, 0) && four(file_, "strl");
        ok &= four(file_, "strh") && u32(file_, 56);
        ok &= four(file_, "vids") && four(file_, "MJPG");
        ok &= u32(file_, 0) && u16(file_, 0) && u16(file_, 0);
        ok &= u32(file_, 0) && u32(file_, 1000);
        strh_rate_pos_ = ftell(file_);
        ok &= u32(file_, 80000) && u32(file_, 0);
        strh_length_pos_ = ftell(file_);
        ok &= u32(file_, 0) && u32(file_, 0) && u32(file_, 0xffffffff) && u32(file_, 0);
        ok &= u16(file_, 0) && u16(file_, 0) && u16(file_, 1280) && u16(file_, 720);
        ok &= four(file_, "strf") && u32(file_, 40);
        ok &= u32(file_, 40) && u32(file_, 1280) && u32(file_, 720);
        ok &= u16(file_, 1) && u16(file_, 24) && four(file_, "MJPG");
        ok &= u32(file_, 1280 * 720 * 3);
        for (int i = 0; i < 4; ++i) ok &= u32(file_, 0);
        long strl_end = ftell(file_);
        ok &= patch(strl_size, static_cast<uint32_t>(strl_end - strl_size - 4));
        ok &= fseek(file_, strl_end, SEEK_SET) == 0;
        long hdrl_end = ftell(file_);
        ok &= patch(hdrl_size, static_cast<uint32_t>(hdrl_end - hdrl_size - 4));
        ok &= fseek(file_, hdrl_end, SEEK_SET) == 0;
        ok &= four(file_, "LIST");
        movi_size_pos_ = ftell(file_);
        ok &= u32(file_, 0) && four(file_, "movi");
        movi_fourcc_pos_ = ftell(file_) - 4;
        if (!ok) { perror("RECORDING_HEADER_FAILED"); failed_ = true; return false; }
        printf("RECORDING_STARTED file=%s format=MJPEG_AVI size=1280x720\n", path_.c_str());
        return true;
    }
public:
    bool write(const std::string& dir, const unsigned char* jpeg, size_t len, uint64_t pts) {
        if (failed_) return false;
        if (!file_ && !start(dir)) { failed_ = true; return false; }
        // RIFF/AVI 1.0 uses 32-bit offsets. Stop before it can overflow.
        long pos = ftell(file_);
        if (pos < 0 || pos > 3500000000LL || len > 2000000 || index_.size() >= 200000u) {
            fprintf(stderr, "RECORDING_LIMIT_REACHED file=%s\n", path_.c_str());
            failed_ = true;
            return false;
        }
        if (!four(file_, "00dc") || !u32(file_, static_cast<uint32_t>(len)) ||
            !put(file_, jpeg, len) || ((len & 1u) && !u16_padding())) {
            perror("RECORDING_WRITE_FAILED");
            failed_ = true; return false;
        }
        index_.push_back({static_cast<uint32_t>(pos - movi_fourcc_pos_),
                          static_cast<uint32_t>(len)});
        if (index_.size() == 1) first_pts_ = pts;
        last_pts_ = pts;
        if (len > max_frame_) max_frame_ = static_cast<uint32_t>(len);
        return true;
    }
    bool u16_padding() { const unsigned char zero = 0; return put(file_, &zero, 1); }
    bool finish() {
        if (!file_) return !failed_;
        long end_movi = ftell(file_);
        bool ok = !failed_ && end_movi > 0;
        if (ok) {
            ok &= patch(movi_size_pos_, static_cast<uint32_t>(end_movi - movi_size_pos_ - 4));
            ok &= fseek(file_, end_movi, SEEK_SET) == 0;
            ok &= four(file_, "idx1") && u32(file_, static_cast<uint32_t>(index_.size() * 16));
            for (const auto& entry : index_) {
                ok &= four(file_, "00dc") && u32(file_, 0x10) &&
                      u32(file_, entry.offset) && u32(file_, entry.size);
                if (!ok) break;
            }
            long end_file = ftell(file_);
            double fps = 80.0;
            if (index_.size() > 1 && last_pts_ > first_pts_) {
                fps = (index_.size() - 1) * 1000000.0 / (last_pts_ - first_pts_);
                if (fps < 5 || fps > 120) fps = 80.0;
            }
            uint32_t rate = static_cast<uint32_t>(fps * 1000.0 + 0.5);
            ok &= patch(avih_us_pos_, static_cast<uint32_t>(1000000.0 / fps + 0.5));
            ok &= patch(avih_bps_pos_, static_cast<uint32_t>(max_frame_ * fps));
            ok &= patch(avih_frames_pos_, static_cast<uint32_t>(index_.size()));
            ok &= patch(strh_rate_pos_, rate);
            ok &= patch(strh_length_pos_, static_cast<uint32_t>(index_.size()));
            ok &= patch(4, static_cast<uint32_t>(end_file - 8));
            ok &= fflush(file_) == 0;
            if (ok) printf("RECORDING_SAVED file=%s frames=%zu fps=%.3f bytes=%ld\n",
                           path_.c_str(), index_.size(), fps, end_file);
        }
        if (fclose(file_) != 0) ok = false;
        file_ = nullptr;
        if (!ok) fprintf(stderr, "RECORDING_FINALIZE_FAILED file=%s\n", path_.c_str());
        return ok;
    }
    ~AviMjpegWriter() { if (file_) finish(); }
};
