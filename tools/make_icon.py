#!/usr/bin/env python3
"""把一张方形的 PNG 图标转成多尺寸 .ico（16/32/48/256）。

只依赖 Python 标准库（zlib + struct），不需要 Pillow / ImageMagick：

    python tools/make_icon.py assets/19icon.png assets/19icon.ico

实现要点：
* 自己解码 PNG（支持灰度 / RGB / 调色板 / 带 alpha 的常见组合，8bit，非隔行）；
* 缩小用面积平均（box filter）+ alpha 预乘，避免边缘发黑；
* 256 尺寸按惯例存成内嵌 PNG，其余尺寸存 32bpp BMP，Windows 各尺寸都会取到清晰的那张。
"""

import struct
import sys
import zlib


# ---------------------------------------------------------------------------
# PNG 解码
# ---------------------------------------------------------------------------

UNFILTER_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def load_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("不是有效的 PNG 文件：%s" % path)

    pos = 8
    idat = bytearray()
    palette = None
    info = None
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            info = struct.unpack(">IIBBBBB", payload)
        elif ctype == b"IDAT":
            idat += payload
        elif ctype == b"PLTE":
            palette = payload
        elif ctype == b"IEND":
            break

    if info is None:
        raise SystemExit("PNG 缺少 IHDR。")
    width, height, depth, color, _comp, _filter, interlace = info
    if interlace != 0:
        raise SystemExit("暂不支持隔行（Adam7）PNG，请另存为普通 PNG。")
    if depth != 8:
        raise SystemExit("只支持 8bit 深的 PNG，当前为 %d bit。" % depth)
    if color not in UNFILTER_CHANNELS:
        raise SystemExit("不支持的 PNG 颜色类型：%d" % color)

    channels = UNFILTER_CHANNELS[color]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    lines = bytearray(height * stride)
    previous = bytearray(stride)
    cursor = 0
    for y in range(height):
        method = raw[cursor]
        cursor += 1
        line = bytearray(raw[cursor:cursor + stride])
        cursor += stride
        if method == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif method == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif method == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif method == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                up = previous[i]
                upleft = previous[i - channels] if i >= channels else 0
                pa = abs(up - upleft)
                pb = abs(left - upleft)
                pc = abs(left + up - 2 * upleft)
                if pa <= pb and pa <= pc:
                    predictor = left
                elif pb <= pc:
                    predictor = up
                else:
                    predictor = upleft
                line[i] = (line[i] + predictor) & 0xFF
        lines[y * stride:(y + 1) * stride] = line
        previous = line

    # 统一转成 RGBA
    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        if color == 6:
            r, g, b, a = lines[i * 4:i * 4 + 4]
        elif color == 2:
            r, g, b = lines[i * 3:i * 3 + 3]
            a = 255
        elif color == 0:
            r = g = b = lines[i]
            a = 255
        elif color == 4:
            r = g = b = lines[i * 2]
            a = lines[i * 2 + 1]
        else:  # 调色板
            index = lines[i]
            if palette is None:
                raise SystemExit("调色板 PNG 缺少 PLTE。")
            r, g, b = palette[index * 3:index * 3 + 3]
            a = 255
        rgba[i * 4] = r
        rgba[i * 4 + 1] = g
        rgba[i * 4 + 2] = b
        rgba[i * 4 + 3] = a
    return width, height, rgba


# ---------------------------------------------------------------------------
# 缩放与编码
# ---------------------------------------------------------------------------

def resize_box(rgba, width, height, size):
    """面积平均缩小，alpha 预乘，避免边缘出现黑边。"""
    out = bytearray(size * size * 4)
    for dy in range(size):
        y0 = dy * height // size
        y1 = max((dy + 1) * height // size, y0 + 1)
        for dx in range(size):
            x0 = dx * width // size
            x1 = max((dx + 1) * width // size, x0 + 1)
            sum_r = sum_g = sum_b = sum_a = 0
            count = 0
            for y in range(y0, y1):
                base = y * width
                for x in range(x0, x1):
                    offset = (base + x) * 4
                    alpha = rgba[offset + 3]
                    sum_r += rgba[offset] * alpha
                    sum_g += rgba[offset + 1] * alpha
                    sum_b += rgba[offset + 2] * alpha
                    sum_a += alpha
                    count += 1
            offset = (dy * size + dx) * 4
            if sum_a == 0 or count == 0:
                out[offset:offset + 4] = b"\x00\x00\x00\x00"
                continue
            out[offset] = min(255, (sum_r + sum_a // 2) // sum_a)
            out[offset + 1] = min(255, (sum_g + sum_a // 2) // sum_a)
            out[offset + 2] = min(255, (sum_b + sum_a // 2) // sum_a)
            out[offset + 3] = min(255, (sum_a + count // 2) // count)
    return out


def encode_bmp_entry(rgba, size):
    """32bpp DIB：BITMAPINFOHEADER + 自下而上的 BGRA + 全零 AND 掩码。"""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    pixels = bytearray()
    for y in range(size - 1, -1, -1):
        row = rgba[y * size * 4:(y + 1) * size * 4]
        for x in range(size):
            r, g, b, a = row[x * 4:x * 4 + 4]
            pixels += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytes(mask_stride * size)
    return header + bytes(pixels) + mask


def encode_png_entry(rgba, size):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0
        raw += rgba[y * size * 4:(y + 1) * size * 4]
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def write_ico(path, images):
    count = len(images)
    header = struct.pack("<HHH", 0, 1, count)
    offset = 6 + 16 * count
    directory = bytearray()
    blobs = bytearray()
    for size, blob in images:
        directory += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,
            0 if size >= 256 else size,
            0, 0, 1, 32, len(blob), offset)
        blobs += blob
        offset += len(blob)
    with open(path, "wb") as handle:
        handle.write(header + bytes(directory) + bytes(blobs))


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)

    source, target = argv[1], argv[2]
    width, height, rgba = load_png(source)
    if width != height:
        raise SystemExit("需要正方形源图，当前为 %dx%d。" % (width, height))

    images = []
    for size in (16, 32, 48, 256):
        scaled = resize_box(rgba, width, height, size)
        blob = encode_png_entry(scaled, size) if size >= 256 else encode_bmp_entry(scaled, size)
        images.append((size, blob))
        print("  %3d x %-3d  %6d bytes  (%s)" % (size, size, len(blob), "PNG" if size >= 256 else "BMP"))

    write_ico(target, images)
    print("已写入 %s（源图 %dx%d，共 %d 个尺寸）" % (target, width, height, len(images)))


if __name__ == "__main__":
    main(sys.argv)
