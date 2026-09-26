#include "player_internal.h"
#include "night_mode_math.h"

static NightModeState g_night = {false, NIGHT_MODE_DEFAULT_PERCENT, false, false, false, 0, 0};
static uint16_t g_night_lut[NIGHT_MODE_LUT_SIZE];
static unsigned g_night_lut_percent = UINT32_MAX;
static SDL_Surface *g_night_surface;
static uint32_t g_night_status_started;
static uint32_t g_night_status_until;
static char g_night_status[40];
static uint32_t g_night_revision;
static uint32_t g_night_fade_started;
static unsigned g_night_display_q8, g_night_from_q8, g_night_target_q8;
static bool g_night_fading;

static void night_mode_format_status(void)
{
    unsigned percent = (g_night_display_q8 + 128U) / 256U;
    if (percent)
        snprintf(g_night_status, sizeof(g_night_status), "NIGHT %u%%", percent);
    else
        snprintf(g_night_status, sizeof(g_night_status), "NIGHT OFF");
}

static void night_mode_tick_fade(uint32_t now_ms)
{
    if (!g_night_fading)
        return;
    uint32_t elapsed = now_ms - g_night_fade_started;
    unsigned next;
    if (elapsed >= LCD_BRIGHTNESS_FADE_MS) {
        next = g_night_target_q8;
        g_night_fading = false;
    } else {
        unsigned mix = ui_ease_smoothstep(elapsed, LCD_BRIGHTNESS_FADE_MS);
        int delta = (int)g_night_target_q8 - (int)g_night_from_q8;
        next = (unsigned)((int)g_night_from_q8 + delta * (int)mix / 255);
    }
    if (next != g_night_display_q8) {
        bool label_changed = (next + 128U) / 256U != (g_night_display_q8 + 128U) / 256U;
        g_night_display_q8 = next;
        ++g_night_revision;
        if (label_changed)
            night_mode_format_status();
    }
}

uint32_t night_mode_revision(void)
{
    return g_night_revision;
}
bool night_mode_is_enabled(void)
{
    return g_night.enabled && g_night.percent != 0U;
}
unsigned night_mode_percent(void)
{
    return night_mode_is_enabled() ? g_night.percent : 0U;
}

void night_mode_hide_status(void)
{
    g_night_status_started = 0;
    g_night_status_until = 0;
}

void night_mode_init(const Fonts *fonts)
{
    (void)fonts;
    g_night.previous_toggle = isKeyPressed(KEY_NSPIRE_N);
    g_night.previous_up = isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_UP);
    g_night.previous_down = isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_DOWN);
}

static bool night_mode_ensure_surface(void)
{
    if (!g_night_surface) {
        g_night_surface =
            SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_W, SCREEN_H, 16, 0xf800, 0x07e0, 0x001f, 0);
    }
    return g_night_surface != NULL;
}

bool night_mode_poll(uint32_t now_ms, bool allow_changes)
{
    night_mode_tick_fade(now_ms);
    bool ctrl = isKeyPressed(KEY_NSPIRE_CTRL);
    bool toggle = isKeyPressed(KEY_NSPIRE_N);
    bool up = ctrl && player_key_pressed(KEY_NSPIRE_UP);
    bool down = ctrl && player_key_pressed(KEY_NSPIRE_DOWN);
    bool activity = toggle || up || down;
    bool status_was_visible = night_mode_status_visible(now_ms);
    if (night_mode_update_keys(&g_night, toggle, up, down, now_ms, allow_changes && has_colors)) {
        ++g_night_revision;
        if (g_night.enabled && !night_mode_ensure_surface()) {
            g_night.enabled = false;
            debug_failf("Night mode: insufficient display buffer memory");
            return activity;
        }
        /* Retarget from the currently displayed tint, including rapid N or
         * held Ctrl+direction changes. OFF still fades through the final frame. */
        g_night_from_q8 = g_night_display_q8;
        g_night_target_q8 = night_mode_percent() * 256U;
        g_night_fade_started = now_ms;
        g_night_fading = g_night_from_q8 != g_night_target_q8;
        night_mode_format_status();
        status_overlay_update_timing(now_ms, !status_was_visible, &g_night_status_started,
                                     &g_night_status_until);
    } else if (allow_changes && has_colors && (up != down) && status_was_visible) {
        /* Holding at 0/100 keeps the existing badge readable without creating
         * another state change or replaying its entrance animation. */
        status_overlay_update_timing(now_ms, false, &g_night_status_started, &g_night_status_until);
    }
    return activity;
}

bool night_mode_status_animating(uint32_t now_ms)
{
    if (g_night_fading)
        return true;
    if (!g_night_status_until)
        return false;
    return (uint32_t)(now_ms - g_night_status_started) < STATUS_BADGE_ANIM_MS ||
           (uint32_t)(now_ms - g_night_status_until) < STATUS_BADGE_EXIT_ANIM_MS;
}

bool night_mode_status_visible(uint32_t now_ms)
{
    return g_night_status_until &&
           (int32_t)(now_ms - (g_night_status_until + STATUS_BADGE_EXIT_ANIM_MS)) < 0;
}

void night_mode_draw_status(SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *video_rect,
                            uint32_t now_ms)
{
    SDL_Rect anchor;
    if (!night_mode_status_visible(now_ms) || g_display_power_state.off)
        return;
    /* Reserve the playback-icon space even with the controls hidden, so the
     * feedback has the identical anchor as the normal brightness badge. */
    anchor = playback_status_badge_anchor(video_rect, true);
    draw_status_overlay_badge(screen, fonts, anchor.x, anchor.y, g_night_status,
                              g_night_status_started, g_night_status_until, now_ms, 255);
}

/* Filter a separate presentation surface so repeated presents never accumulate
 * tint and cached video frames/UI snapshots remain in their original colors. */
void *night_mode_present_pixels(SDL_Surface *screen)
{
    int y;
    if (!has_colors || !screen || screen->format->BitsPerPixel != 16 || !g_night_display_q8) {
        return screen ? screen->pixels : NULL;
    }
    if (!night_mode_ensure_surface())
        return screen->pixels;
    if (g_night_lut_percent != g_night_display_q8) {
        night_mode_build_lut_q8(g_night_lut, g_night_display_q8);
        g_night_lut_percent = g_night_display_q8;
    }
    for (y = 0; y < SCREEN_H; ++y) {
        const uint16_t *src =
            (const uint16_t *)((const uint8_t *)screen->pixels + y * screen->pitch);
        uint16_t *dst =
            (uint16_t *)((uint8_t *)g_night_surface->pixels + y * g_night_surface->pitch);
        night_mode_filter_row(dst, src, SCREEN_W, g_night_lut);
    }
    return g_night_surface->pixels;
}

void night_mode_shutdown(void)
{
    if (g_night_surface)
        SDL_FreeSurface(g_night_surface);
    g_night_surface = NULL;
}
