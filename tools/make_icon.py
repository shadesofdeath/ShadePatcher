#
# Builds the application icon from the supplied artwork.
#
# The 3A dark variant is used because it carries its own background, so the icon reads the same on a light or a
# dark title bar, and its sage and sand tiles stay distinguishable when scaled down to 16 pixels.
#
# The only change made to the artwork is rounding the corners, which is the shape Windows 11 app icons have.
# The composition itself is untouched.
#
# The .ico is written here rather than by Pillow so that the frame formats can be chosen: 32-bit bitmaps for
# every size up to 128, which every version of Windows and every shell surface reads, and PNG for the 256 frame,
# where a bitmap would waste 256 KB.
#
import os
import struct

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "design", "icon", "3A", "logo-dark.png")
OUT_DIR = os.path.join(ROOT, "src", "common")
OUT_ICO = os.path.join(OUT_DIR, "app.ico")
OUT_PNG = os.path.join(OUT_DIR, "app.png")
PREVIEW = os.path.join(OUT_DIR, "app-preview.png")

# Every size Windows asks for. A missing size is scaled by the shell, usually badly.
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
PNG_FROM = 256


def rounded(image, radius_ratio=0.18):
    """Applies a rounded-square alpha mask, the way Windows 11 app icons are shaped."""
    size = image.size[0]
    radius = int(size * radius_ratio)

    # Drawn at 4x and scaled back down so the curve has no stair-stepping.
    scale = 4
    mask = Image.new("L", (size * scale, size * scale), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size * scale - 1, size * scale - 1), radius=radius * scale, fill=255
    )
    mask = mask.resize((size, size), Image.LANCZOS)

    out = image.copy()
    # Combined with whatever transparency the artwork already carries.
    out.putalpha(Image.composite(out.getchannel("A"), Image.new("L", out.size, 0), mask))
    return out


def bmp_frame(image):
    """One ICO frame as a 32-bit bottom-up bitmap, followed by the AND mask the format still requires."""
    width, height = image.size
    pixels = image.load()

    xor = bytearray()
    for y in range(height - 1, -1, -1):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            xor += bytes((b, g, r, a))

    # The AND mask is ignored for 32-bit frames but must be present and 4-byte aligned per row.
    row_bytes = ((width + 31) // 32) * 4
    and_mask = bytearray()
    for y in range(height - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(width):
            if pixels[x, y][3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        and_mask += row

    header = struct.pack(
        "<IiiHHIIiiII",
        40,             # biSize
        width,
        height * 2,     # colour data and mask stacked
        1,              # biPlanes
        32,             # biBitCount
        0,              # BI_RGB
        len(xor) + len(and_mask),
        0, 0, 0, 0,
    )
    return bytes(header + xor + and_mask)


def png_frame(image):
    from io import BytesIO

    buffer = BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_ico(path, frames):
    """frames: list of (size, payload bytes)."""
    out = bytearray(struct.pack("<HHH", 0, 1, len(frames)))    # reserved, type 1 = icon, count

    offset = 6 + 16 * len(frames)
    entries = bytearray()
    payloads = bytearray()

    for size, payload in frames:
        entries += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,    # 0 means 256
            0 if size >= 256 else size,
            0,      # no colour palette
            0,      # reserved
            1,      # planes
            32,     # bits per pixel
            len(payload),
            offset,
        )
        payloads += payload
        offset += len(payload)

    out += entries + payloads
    with open(path, "wb") as handle:
        handle.write(out)


def main():
    source = Image.open(SOURCE).convert("RGBA")
    print("source:", source.size, "corner pixel:", source.getpixel((2, 2)))

    shaped = rounded(source)

    frames = []
    previews = []
    for size in SIZES:
        # Resized from the 1024 original every time, rather than stepping down, so every size stays sharp.
        image = shaped.resize((size, size), Image.LANCZOS)
        previews.append(image)
        frames.append((size, png_frame(image) if size >= PNG_FROM else bmp_frame(image)))

    write_ico(OUT_ICO, frames)
    shaped.resize((256, 256), Image.LANCZOS).save(OUT_PNG)

    # A strip of the real small sizes on a light and a dark band, to judge how the icon actually reads.
    strip_sizes = [16, 20, 24, 32, 48, 64]
    pad = 12
    strip_w = sum(s + pad for s in strip_sizes) + pad
    strip = Image.new("RGBA", (strip_w, 64 * 2 + pad * 3), (255, 255, 255, 255))
    ImageDraw.Draw(strip).rectangle((0, 64 + pad * 2, strip_w, strip.size[1]), fill=(32, 32, 32, 255))
    x = pad
    for size in strip_sizes:
        image = shaped.resize((size, size), Image.LANCZOS)
        strip.paste(image, (x, pad + (64 - size) // 2), image)
        strip.paste(image, (x, 64 + pad * 2 + (64 - size) // 2), image)
        x += size + pad
    strip.save(PREVIEW)

    print("wrote", OUT_ICO, os.path.getsize(OUT_ICO), "bytes")
    print("wrote", OUT_PNG)
    print("wrote", PREVIEW)


if __name__ == "__main__":
    main()
