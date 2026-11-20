#!/usr/bin/env python3
"""重构守门员：判断一次「只改空白/注释」的重排是否真的没有动逻辑。

做法：把两份源码都归一化成"去掉注释 + 去掉字符串外的所有空白"的一条长串，
      再逐字符比较。若完全相同 → 逻辑一字未改；若有差异 → 打印差异片段，
      人工确认每一处是不是有意为之（例如把重复代码抽成函数）。

用法：
    python tools/check_refactor.py <before.cpp> <after.cpp>
"""
import difflib
import sys


def normalize(src: str) -> str:
    """去掉注释与字符串外的空白；字符串/字符字面量原样保留。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        # 字符串 / 字符字面量：整体保留（含内部空格）
        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == '\\' and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        # 行注释
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            continue
        # 块注释
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
            continue
        if c.isspace():
            i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a = normalize(open(sys.argv[1], encoding='utf-8').read())
    b = normalize(open(sys.argv[2], encoding='utf-8').read())
    if a == b:
        print("IDENTICAL：去掉注释与空白后完全一致 → 这次重排没有改动任何逻辑")
        return 0

    print("DIFF：归一化后仍有差异，逐处确认（每处都是有意为之才算通过）")
    sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
    count = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        count += 1
        print("\n#%d [%s] 前 %d 字符 / 后 %d 字符" % (count, tag, i2 - i1, j2 - j1))
        print("  - " + a[max(0, i1 - 40):i2 + 40].replace('\n', ' ')[:300])
        print("  + " + b[max(0, j1 - 40):j2 + 40].replace('\n', ' ')[:300])
    print("\n共 %d 处差异（均为空白/注释之外的改动）" % count)
    return 1


if __name__ == '__main__':
    sys.exit(main())
