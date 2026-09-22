#!/usr/bin/env python3
# ===========================================================================
#  tools/mkinitrd.py —— 把 .xzs 程序打包成 initrd
#  ---------------------------------------------------------------------------
#  格式（简单、够用）：
#     Header:  magic('XZS1') + count
#     Entry[]: name[32] + offset + size
#     Data:    各程序内容依次排列
#
#  offset 是相对**文件开头**的偏移。
# ===========================================================================
import struct, sys, os

MAGIC = b'XZS1'

def main():
    if len(sys.argv) < 3:
        print("用法: mkinitrd.py <输出文件> <程序1> [程序2] ...")
        return 1

    out_path = sys.argv[1]
    files = sys.argv[2:]

    entries = []
    blobs = []

    # 头部 8 字节 + 每个目录项 40 字节
    offset = 8 + 40 * len(files)

    for path in files:
        name = os.path.basename(path)
        if len(name) > 31:
            print("文件名太长: %s" % name)
            return 1
        with open(path, 'rb') as f:
            data = f.read()
        entries.append((name, offset, len(data)))
        blobs.append(data)
        offset += len(data)

    with open(out_path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<I', len(entries)))
        for name, off, size in entries:
            nb = name.encode('ascii').ljust(32, b'\0')
            f.write(nb)
            f.write(struct.pack('<II', off, size))
        for b in blobs:
            f.write(b)

    print("[initrd] %s: %d 个程序, %d 字节" % (out_path, len(entries), offset))
    for name, off, size in entries:
        print("         %-20s %6d 字节" % (name, size))
    return 0

if __name__ == '__main__':
    sys.exit(main())
