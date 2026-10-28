// blit_arm64.S — hand-written AArch64 inner loop of the glyph compositor.
//
// void blit_span16_asm(uint32_t *dst, const uint8_t *cov, size_t n16, const uint8_t color[4])
//
// Composites n16 blocks of 16 pixels: for each pixel, source = color * coverage
// (premultiplied BGRA, color[0..3] = b, g, r, a at full coverage), then
// dst = src + dst * (255 - src.a) / 255, every channel rounded exactly like the C
// path: round(x*y/255) = ((t = x*y + 128) + (t >> 8)) >> 8.
// Blocks whose 16 coverage bytes are all zero are skipped without touching dst.
//
// Register use (all caller-saved, no stack):
//   x0 dst, x1 cov, x2 blocks left, x3 color
//   v16-v19  color b, g, r, a broadcast      v20 = 255      v21 = 128 (u16)
//   v0       coverage                         v1-v4 dst planes (ld4: b, g, r, a)
//   v5       source alpha                     v6 inverse alpha
//   v22-v31  scratch for the widening multiplies

    .text
    .p2align 4
    .globl _blit_span16_asm
_blit_span16_asm:
    cbz     x2, 9f
    ld1r    {v16.16b}, [x3], #1          // b
    ld1r    {v17.16b}, [x3], #1          // g
    ld1r    {v18.16b}, [x3], #1          // r
    ld1r    {v19.16b}, [x3]              // a
    movi    v20.16b, #255
    movi    v21.8h, #128

// MUL255 dst, a, b: dst.16b = round(a.16b * b.16b / 255), clobbers v22-v25
.macro MUL255 dst, a, b
    umull   v22.8h, \a\().8b, \b\().8b
    umull2  v23.8h, \a\().16b, \b\().16b
    add     v22.8h, v22.8h, v21.8h
    add     v23.8h, v23.8h, v21.8h
    usra    v22.8h, v22.8h, #8           // t + (t >> 8)
    usra    v23.8h, v23.8h, #8
    shrn    \dst\().8b, v22.8h, #8
    shrn2   \dst\().16b, v23.8h, #8
.endm

1:
    ld1     {v0.16b}, [x1], #16          // 16 coverage bytes
    umaxv   b26, v0.16b
    fmov    w9, s26
    cbz     w9, 2f                       // empty run: skip the block
    ld4     {v1.16b, v2.16b, v3.16b, v4.16b}, [x0]
    MUL255  v5, v19, v0                  // source alpha = a * cov
    sub     v6.16b, v20.16b, v5.16b      // 255 - source alpha
    MUL255  v27, v16, v0                 // source b
    MUL255  v28, v1, v6                  // dst b * inv
    add     v1.16b, v27.16b, v28.16b
    MUL255  v27, v17, v0                 // g
    MUL255  v28, v2, v6
    add     v2.16b, v27.16b, v28.16b
    MUL255  v27, v18, v0                 // r
    MUL255  v28, v3, v6
    add     v3.16b, v27.16b, v28.16b
    MUL255  v28, v4, v6                  // a
    add     v4.16b, v5.16b, v28.16b
    st4     {v1.16b, v2.16b, v3.16b, v4.16b}, [x0]
2:
    add     x0, x0, #64                  // 16 pixels
    subs    x2, x2, #1
    b.ne    1b
9:
    ret

// void blit_glyph_asm(uint32_t *dst, size_t dst_stride_bytes, const uint8_t *cov,
//                     size_t cov_stride, size_t blocks, size_t rows, const uint8_t color[4])
// Whole glyph: `rows` rows of `blocks` x 16 pixels (masks are zero-padded to 16).
// A block whose destination is all zero (text drawn on a just-cleared area, the
// common case) stores the source directly: same result, half the multiplies.
    .p2align 4
    .globl _blit_glyph_asm
_blit_glyph_asm:
    cbz     x5, 9f
    cbz     x4, 9f
    ld1r    {v16.16b}, [x6], #1
    ld1r    {v17.16b}, [x6], #1
    ld1r    {v18.16b}, [x6], #1
    ld1r    {v19.16b}, [x6]
    movi    v20.16b, #255
    movi    v21.8h, #128
3:                                       // next row
    mov     x7, x0
    mov     x8, x2
    mov     x9, x4
4:
    ld1     {v0.16b}, [x8], #16
    umaxv   b26, v0.16b
    fmov    w10, s26
    cbz     w10, 5f
    ld4     {v1.16b, v2.16b, v3.16b, v4.16b}, [x7]
    MUL255  v5, v19, v0
    orr     v7.16b, v1.16b, v2.16b       // destination block all zero (freshly cleared)?
    orr     v29.16b, v3.16b, v4.16b
    orr     v7.16b, v7.16b, v29.16b
    umaxv   b29, v7.16b
    fmov    w11, s29
    cbnz    w11, 6f
    MUL255  v1, v16, v0                  // then src over 0 = src: no destination math
    MUL255  v2, v17, v0
    MUL255  v3, v18, v0
    mov     v4.16b, v5.16b
    st4     {v1.16b, v2.16b, v3.16b, v4.16b}, [x7]
    b       5f
6:
    sub     v6.16b, v20.16b, v5.16b
    MUL255  v27, v16, v0
    MUL255  v28, v1, v6
    add     v1.16b, v27.16b, v28.16b
    MUL255  v27, v17, v0
    MUL255  v28, v2, v6
    add     v2.16b, v27.16b, v28.16b
    MUL255  v27, v18, v0
    MUL255  v28, v3, v6
    add     v3.16b, v27.16b, v28.16b
    MUL255  v28, v4, v6
    add     v4.16b, v5.16b, v28.16b
    st4     {v1.16b, v2.16b, v3.16b, v4.16b}, [x7]
5:
    add     x7, x7, #64
    subs    x9, x9, #1
    b.ne    4b
    add     x0, x0, x1                   // next destination row
    add     x2, x2, x3                   // next mask row
    subs    x5, x5, #1
    b.ne    3b
9:
    ret
