"""FH3 (Xbox One / Durango) UI icon .swatchbin <-> PNG.

File = 87-byte header (constant per size) + BC7 blocks, 1D-tiled (8x8-block micro tiles, Morton inside).
  small icon: 200x200 px, in a 224x224 surface  (56x56 blocks, file 50 263 bytes)
  large icon: 916x458 px, in a 928x480 surface  (232x120 blocks, file 445 527 bytes)
"""
import sys
import numpy as np
from PIL import Image
from bc import decode
from bc7enc import encode

HDR_SMALL = bytes.fromhex(
    '62757247010100005700000057c400000100000042435854020001002c0000005700000000c4000000c40000484358543002080000000000000000000000000000000001c800c8000100c800c80001000101002d010048')
HDR_LARGE = bytes.fromhex(
    '62757247010100005700000057cc06000100000042435854020001002c0000005700000000cc060000cc06004843585430020800000000000000000000000000000000019403ca0101009403ca0101000101002d010048')
HDR_SIZE = 87
assert len(HDR_SMALL) == HDR_SIZE and len(HDR_LARGE) == HDR_SIZE


def _index_map(kind):
    """FH3: ARRAY_1D_TILED_THIN1, 128 bit/block. The surface is cut into 8x8-block micro tiles stored row by row;
    inside a tile the block index is the Morton order x0 y0 x1 y1 x2 y2."""
    bw, bh = (56, 56) if kind == 'small' else (232, 120)
    ys, xs = np.mgrid[0:bh, 0:bw]
    lx, ly = xs & 7, ys & 7
    m = (lx & 1) | ((ly & 1) << 1) | (((lx >> 1) & 1) << 2) | (((ly >> 1) & 1) << 3) | (((lx >> 2) & 1) << 4) | (((ly >> 2) & 1) << 5)
    return ((ys >> 3) * (bw >> 3) + (xs >> 3)) * 64 + m, bw, bh


SIZES = {'small': (200, 200), 'large': (916, 458)}


def _premultiply(arr):
    a = arr[..., 3:4].astype(np.float32) / 255.0
    out = arr.copy()
    out[..., :3] = np.clip(np.round(arr[..., :3].astype(np.float32) * a), 0, 255).astype(np.uint8)
    return out


def _unpremultiply(arr):
    a = arr[..., 3:4].astype(np.float32)
    out = arr.copy()
    with np.errstate(divide='ignore', invalid='ignore'):
        rgb = np.where(a > 0, arr[..., :3].astype(np.float32) * 255.0 / np.maximum(a, 1), 0)
    out[..., :3] = np.clip(np.round(rgb), 0, 255).astype(np.uint8)
    return out


def png_to_swatchbin(img, kind='small'):
    """img: PIL image (straight alpha, as every PNG). The game's icons store premultiplied alpha, so the colour is
    multiplied by alpha here. A picture of another size is scaled (premultiplied) to the icon size."""
    w, h = SIZES[kind]
    img = img.convert('RGBA')
    arr = np.array(img)
    if img.size != (w, h):
        pre = Image.fromarray(_premultiply(arr), 'RGBA').resize((w, h), Image.LANCZOS)
        arr = np.array(pre)
    else:
        arr = _premultiply(arr)
    idx, bw, bh = _index_map(kind)
    surf = np.zeros((bh * 4, bw * 4, 4), np.uint8)
    surf[:h, :w] = arr
    blocks = np.frombuffer(encode(surf), np.uint8).reshape(bh, bw, 16)   # row-major (by, bx)
    # padding blocks (entirely outside the picture) are stored as 16 zero bytes in the original files
    nbx, nby = (w + 3) // 4, (h + 3) // 4
    out = np.zeros((bw * bh, 16), np.uint8)
    for by in range(nby):
        for bx in range(nbx):
            out[idx[by, bx]] = blocks[by, bx]
    return (HDR_SMALL if kind == 'small' else HDR_LARGE) + out.tobytes()


def swatchbin_to_png(data, kind='small'):
    """Returns a straight-alpha RGBA PIL image."""
    idx, bw, bh = _index_map(kind)
    body = np.frombuffer(data[HDR_SIZE:], np.uint8).reshape(-1, 16)
    lin = body[idx.reshape(-1)]           # row-major block order
    img = decode(lin.tobytes(), bw * 4, bh * 4, 98)
    w, h = SIZES[kind]
    arr = _unpremultiply(np.array(img.crop((0, 0, w, h))))
    return Image.fromarray(arr, 'RGBA')


def kind_of(data):
    return {len(HDR_SMALL) + 56 * 56 * 16: 'small', len(HDR_LARGE) + 232 * 120 * 16: 'large'}[len(data)]


if __name__ == '__main__':
    print('library module; use png2swatchbin.py')
