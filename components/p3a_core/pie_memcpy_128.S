    .text
    .align 4
    .global pie_memcpy_128
    .type   pie_memcpy_128, @function

/*
 * void pie_memcpy_128(void *dst, const void *src, size_t len);
 *
 * a0 = dst
 * a1 = src
 * a2 = len (bytes)
 *
 * Strategy:
 *   - Copy len / 128 blocks of 128 bytes (8x 16B vectors) using PIE + HWLP
 *   - Then copy (len % 128) / 16 blocks of 16 bytes using PIE + HWLP
 *   - Finally copy len % 16 bytes with scalar loads/stores
 *
 * Uses ESP32-P4 PIE 128-bit load/store instructions:
 *   esp.vld.128.ip qN, rs1, 16
 *   esp.vst.128.ip qN, rs1, 16
 *
 * Uses ESP32-P4 HW loop extension:
 *   esp.lp.setup <loop_id>, <count_reg>, <label>
 *
 * Works for any len >= 0. For best performance, keep dst/src 16-byte aligned
 * and len a multiple of 16, but this function does not require it.
 */

pie_memcpy_128:
    # If length == 0, return immediately
    beqz    a2, .Lret

    ################################################################
    # 1) Copy 128-byte blocks (8 * 16B vectors) with HWLP
    ################################################################
    srli    t3, a2, 7          # t3 = len / 128
    beqz    t3, .Ltail16       # if no 128B blocks, skip

    # HWLP on ESP32-P4 is sensitive to alignment of the setup instruction.
    # Align to 4 bytes; fill with 0x0001 which is the 16-bit c.nop encoding.
    .balignw 4, 0x0001
    esp.lp.setup 0, t3, .Lblock_loop

.Lblock_loop:
    # Load 128 bytes into Q registers, 16 bytes at a time
    esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a1, 16
    esp.vld.128.ip q2, a1, 16
    esp.vld.128.ip q3, a1, 16
    esp.vld.128.ip q4, a1, 16
    esp.vld.128.ip q5, a1, 16
    esp.vld.128.ip q6, a1, 16
    esp.vld.128.ip q7, a1, 16

    # Store 128 bytes from Q registers to destination
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vst.128.ip q2, a0, 16
    esp.vst.128.ip q3, a0, 16
    esp.vst.128.ip q4, a0, 16
    esp.vst.128.ip q5, a0, 16
    esp.vst.128.ip q6, a0, 16
    esp.vst.128.ip q7, a0, 16
    # No explicit branch here – HWLP repeats the body t3 times.

    ################################################################
    # 2) Copy remaining 16-byte chunks with HWLP
    #    tail128 = len & 0x7F
    #    n16 = tail128 / 16
    ################################################################
.Ltail16:
    andi    t3, a2, 127        # t3 = len % 128 (0..127)
    srli    t3, t3, 4          # t3 = (len % 128) / 16
    beqz    t3, .Ltail_scalar  # if no 16B blocks, skip

    .balignw 4, 0x0001
    esp.lp.setup 1, t3, .Ltail16_loop

.Ltail16_loop:
    esp.vld.128.ip q0, a1, 16
    esp.vst.128.ip q0, a0, 16
    # Again, HWLP handles the loop count and branch.

    ################################################################
    # 3) Copy remaining bytes (0..15) with scalar loads/stores
    #    scalarTail = len & 0xF
    ################################################################
.Ltail_scalar:
    andi    t3, a2, 15         # t3 = len % 16 (0..15)
    beqz    t3, .Lret          # if no leftover, we're done

    mv      t0, t3             # t0 = remaining bytes counter

.Lscalar_loop:
    lb      t1, 0(a1)
    sb      t1, 0(a0)
    addi    a1, a1, 1
    addi    a0, a0, 1
    addi    t0, t0, -1
    bnez    t0, .Lscalar_loop

.Lret:
    ret

    .size   pie_memcpy_128, .-pie_memcpy_128
