"""不开解码器也能验 AVI 结构是否自洽：RIFF 链、avih 帧数、movi 里 00dc 块数、idx1 条目数。

用法：python tools/check_avi.py <file.avi>
"""
import struct
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    assert data[0:4] == b"RIFF", "不是 RIFF"
    riff_size = u32(data, 4)
    assert data[8:12] == b"AVI ", "不是 AVI"
    print("RIFF size = %d，实际文件 %d（差 %d = 尾部索引/对齐）"
          % (riff_size, len(data), len(data) - riff_size - 8))

    # 找 avih / movi / idx1
    avih = data.find(b"avih")
    frames_in_avih = u32(data, avih + 8 + 16)      # avih 里 dwTotalFrames 在 +16
    micro_per_frame = u32(data, avih + 8)
    w = u32(data, avih + 8 + 32)
    h = u32(data, avih + 8 + 36)
    print("avih: 帧数=%d  每帧微秒=%d (≈%.1f fps)  尺寸=%dx%d"
          % (frames_in_avih, micro_per_frame, 1e6 / micro_per_frame, w, h))

    movi = data.find(b"movi")
    movi_end = data.find(b"idx1")
    chunks_00dc = data.count(b"00dc", movi, movi_end)
    print("movi 里 00dc 块数 = %d" % chunks_00dc)

    if movi_end > 0:
        idx_size = u32(data, movi_end + 4)
        entries = idx_size // 16
        print("idx1 条目数 = %d（每帧一条索引）" % entries)
    else:
        entries = -1
        print("没有 idx1")

    ok = (frames_in_avih == chunks_00dc == entries)
    print("\n结论：%s（三处帧数 %d / %d / %d）"
          % ("一致 OK" if ok else "不一致 FAIL", frames_in_avih, chunks_00dc, entries))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
