// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* bounce-drop: physically-flavored gravity drop with damped bounces.
 * The logo accelerates downward under constant gravity, slams into its
 * settled position, rebounds with energy loss, and after two bounces
 * settles for good. Arc count and timing are picked to fit t ∈ [0, 1].
 *
 * Trajectory (analytical, no integration):
 *   - Falling arc 0: starts at rest at height H, lands at 0 with speed
 *     v0 = sqrt(2*g*H). Position is H * (1 − u^2), so velocity grows
 *     LINEARLY from 0 → v0 across the arc — that's the gravity feel.
 *   - Bounce arcs 1, 2: symmetric parabolic up-then-down peaking at
 *     height H_i = e^(2i) * H, with peak velocity v_i = e^i * v0.
 *     Velocity reverses ABRUPTLY at every landing (sign flip with
 *     magnitude reduced by factor e), so the impact reads as a hard
 *     bounce, not a sinusoidal swing.
 *   - Restitution e = 0.42 ⇒ first bounce reaches ~18% of H, second
 *     reaches ~3%. After arc 2 the logo holds.
 *
 * Arc durations are weighted by physical fall times: T_i ∝ sqrt(H_i).
 * Whatever t-budget remains after the two bounces sits at rest at 0.
 *
 * Y-only motion → one canonical blit per frame at a shifted dest. */

#include "intro_anim.h"
#include "p3a_logo.h"

void ia_bounce_drop_render(uint8_t *buffer,
                           const intro_anim_ctx_t *ctx,
                           float t)
{
    if (t <= 0.0f) {
        intro_anim_fill_bg(buffer, ctx);
        return;
    }
    if (t >= 1.0f) {
        intro_anim_fill_bg(buffer, ctx);
        p3a_logo_blit_pixelwise_bgr888(
            buffer, ctx->width, ctx->height, (int)ctx->stride,
            ctx->logo_x, ctx->logo_y, 255,
            ctx->bg_b, ctx->bg_g, ctx->bg_r,
            ctx->logo_scale, ctx->rotation);
        return;
    }

    intro_anim_fill_bg(buffer, ctx);

    const float drop_h = (float)(ctx->logo_y + ctx->height / 2);

    /* Restitution + arc durations.
     *   T_fall ∝ sqrt(H), so duration ratios: 1, 2*e, 2*e^2  (×2 because
     *   each bounce is up-then-down, twice the half-fall). */
    const float e = 0.42f;
    float D0 = 1.0f;
    float D1 = 2.0f * e;
    float D2 = 2.0f * e * e;
    /* We want all three arcs to fit inside ~0.92 of t so the logo holds
     * still for the last fraction (canonical end state visible briefly
     * before t=1). Normalize. */
    const float HOLD_FRACTION = 0.08f;
    float Dsum = D0 + D1 + D2;
    float scale = (1.0f - HOLD_FRACTION) / Dsum;
    float T1 = D0 * scale;             /* falling arc ends at T1 */
    float T2 = T1 + D1 * scale;        /* bounce 1 ends at T2 */
    float T3 = T2 + D2 * scale;        /* bounce 2 ends at T3 */

    float h0 = drop_h;
    float h1 = e * e * drop_h;
    float h2 = e * e * e * e * drop_h;

    float y_off;
    if (t < T1) {
        /* Gravity fall: y = H * (1 - u^2). Velocity ∝ -u, so SPEED grows
         * linearly into the impact — accelerating descent. */
        float u = t / T1;
        y_off = h0 * (1.0f - u * u);
    } else if (t < T2) {
        /* Bounce 1, symmetric parabola up-then-down: y = h1 * 4u(1-u).
         * dy/du = h1 * 4 * (1 - 2u) — positive (rising) at u=0, negative
         * (falling) at u=1, magnitude h1*4. Glued to arc 0's landing
         * speed by the e^2 height ratio (sqrt → e velocity ratio). */
        float u = (t - T1) / (T2 - T1);
        y_off = h1 * 4.0f * u * (1.0f - u);
    } else if (t < T3) {
        /* Bounce 2, smaller still. */
        float u = (t - T2) / (T3 - T2);
        y_off = h2 * 4.0f * u * (1.0f - u);
    } else {
        /* Held still at 0 for the remaining fraction. */
        y_off = 0.0f;
    }

    int dy_logo = ctx->logo_y - (int)(y_off + 0.5f);

    p3a_logo_blit_pixelwise_bgr888(
        buffer, ctx->width, ctx->height, (int)ctx->stride,
        ctx->logo_x, dy_logo, 255,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        ctx->logo_scale, ctx->rotation);
}
