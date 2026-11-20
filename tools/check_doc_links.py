#!/usr/bin/env python3
"""检查 Markdown 里的相对链接是否都指向真实存在的文件（整理文档后跑一遍）。

用法：python tools/check_doc_links.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK = re.compile(r'\[[^\]]*\]\(([^)#]+)(?:#[^)]*)?\)')


def main() -> int:
    bad = 0
    for dirpath, _dirnames, filenames in os.walk(ROOT):
        if 'build' in dirpath.split(os.sep) or 'third_party' in dirpath.split(os.sep):
            continue
        for name in filenames:
            if not name.endswith('.md'):
                continue
            path = os.path.join(dirpath, name)
            text = open(path, encoding='utf-8', errors='replace').read()
            for target in LINK.findall(text):
                if target.startswith(('http://', 'https://', 'mailto:')):
                    continue
                full = os.path.normpath(os.path.join(dirpath, target))
                if not os.path.exists(full):
                    bad += 1
                    print("%s -> %s  (不存在)" % (os.path.relpath(path, ROOT), target))
    print("OK：所有相对链接都存在" if bad == 0 else "共 %d 个坏链接" % bad)
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
