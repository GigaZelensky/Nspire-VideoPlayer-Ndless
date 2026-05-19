#include "player_internal.h"

void strip_filename(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash) {
        slash = strrchr(path, '\\');
    }
    if (slash) {
        *slash = '\0';
    } else {
        strcpy(path, ".");
    }
}

void history_path_for_directory(const char *directory, char *history_path, size_t history_path_size)
{
    if (!history_path || history_path_size == 0) {
        return;
    }
    snprintf(history_path, history_path_size, "%s/%s", directory && directory[0] != '\0' ? directory : ".", HISTORY_FILE_NAME);
}

void history_path_for_movie(const char *movie_path, char *history_path, size_t history_path_size)
{
    char directory[MAX_PATH_LEN];

    if (!history_path || history_path_size == 0) {
        return;
    }
    if (movie_path && movie_path[0] != '\0') {
        snprintf(directory, sizeof(directory), "%s", movie_path);
        strip_filename(directory);
    } else {
        snprintf(directory, sizeof(directory), ".");
    }
    history_path_for_directory(directory, history_path, history_path_size);
}

void history_entry_init_defaults(HistoryEntry *entry)
{
    if (!entry) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->scale_mode = (uint8_t) SCALE_FIT;
    entry->playback_rate_index = (uint8_t) PLAYBACK_RATE_DEFAULT_INDEX;
    entry->playback_mode = (uint8_t) PLAYBACK_MODE_ONCE;
    entry->subtitle_font_index = (uint8_t) SUBTITLE_FONT_DEFAULT_INDEX;
    entry->subtitle_size = 0;
    entry->subtitle_placement = (uint8_t) SUBTITLE_POS_BAR_BOTTOM;
    entry->video_align_x = (int8_t) VIDEO_ALIGN_CENTER;
    entry->video_align_y = (int8_t) VIDEO_ALIGN_CENTER;
}

void apply_history_entry_settings(
    const HistoryEntry *entry,
    Movie *movie,
    ScaleMode *scale_mode,
    size_t *playback_rate_index,
    PlaybackMode *playback_mode,
    bool *realtime_frame_skip,
    size_t *subtitle_font_index,
    int *subtitle_size,
    SubtitlePlacement *subtitle_placement,
    VideoAlign *video_align_x,
    VideoAlign *video_align_y
)
{
    if (!entry || !movie || !scale_mode || !playback_rate_index || !playback_mode ||
        !realtime_frame_skip || !subtitle_font_index || !subtitle_size || !subtitle_placement ||
        !video_align_x || !video_align_y) {
        return;
    }

    *scale_mode = (ScaleMode) clamp_int((int) entry->scale_mode, SCALE_FIT, SCALE_NATIVE);
    *playback_rate_index = (size_t) clamp_int((int) entry->playback_rate_index, 0, (int) (PLAYBACK_RATE_COUNT - 1U));
    *playback_mode = (PlaybackMode) clamp_int((int) entry->playback_mode, PLAYBACK_MODE_ONCE, PLAYBACK_MODE_COUNT - 1);
    *realtime_frame_skip = entry->realtime_frame_skip;
    *subtitle_font_index = (size_t) clamp_int((int) entry->subtitle_font_index, 0, (int) (SUBTITLE_FONT_CHOICE_COUNT - 1U));
    *subtitle_size = clamp_int((int) entry->subtitle_size, -1, 3);
    *video_align_x = clamp_video_align((int) entry->video_align_x);
    *video_align_y = clamp_video_align((int) entry->video_align_y);

    if (movie->subtitle_track_count == 0) {
        movie->selected_subtitle_track = 0;
    } else {
        uint16_t max_track = (uint16_t) (movie->subtitle_track_count - 1U);
        movie->selected_subtitle_track = (uint16_t) clamp_int((int) entry->selected_subtitle_track, 0, (int) max_track);
    }
    *subtitle_placement = subtitle_normalize_placement(
        (SubtitlePlacement) clamp_int((int) entry->subtitle_placement, SUBTITLE_POS_BAR_BOTTOM, SUBTITLE_POS_COUNT - 1),
        selected_subtitle_track_supports_auto_positioning(movie)
    );
}

bool load_history_store_from_path(const char *history_path, HistoryStore *history)
{
    FILE *file;
    char line[MAX_PATH_LEN + 128];
    int version = 1;

    memset(history, 0, sizeof(*history));
    history->theme_id = UI_THEME_DORFIC;
    file = fopen(history_path, "rb");
    if (!file) {
        return true;
    }
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return false;
    }
    if (strncmp(line, HISTORY_MAGIC_V6, 5) == 0) {
        version = 6;
    } else if (strncmp(line, HISTORY_MAGIC_V5, 5) == 0) {
        version = 5;
    } else if (strncmp(line, HISTORY_MAGIC_V4, 5) == 0) {
        version = 4;
    } else if (strncmp(line, HISTORY_MAGIC_V3, 5) == 0) {
        version = 3;
    } else if (strncmp(line, HISTORY_MAGIC_V2, 5) == 0) {
        version = 2;
    } else if (strncmp(line, HISTORY_MAGIC_V1, 5) != 0) {
        fclose(file);
        return false;
    }
    while (history->count < HISTORY_MAX_ENTRIES && fgets(line, sizeof(line), file)) {
        char *path = NULL;
        HistoryEntry entry;

        history_entry_init_defaults(&entry);
        if (version >= 5 && strncmp(line, "@theme\t", 7) == 0) {
            history->theme_id = ui_theme_clamp((int) strtol(line + 7, NULL, 10));
            continue;
        }
        if (version >= 2) {
            char *fields[13];
            size_t expected_field_count = version >= 6 ? 13U : (version >= 4 ? 12U : (version >= 3 ? 10U : 9U));
            size_t field_index;

            fields[0] = line;
            for (field_index = 1; field_index < expected_field_count; ++field_index) {
                char *separator = strchr(fields[field_index - 1], '\t');
                if (!separator) {
                    break;
                }
                *separator = '\0';
                fields[field_index] = separator + 1;
            }
            if (field_index < expected_field_count) {
                continue;
            }

            path = fields[expected_field_count - 1U];
            entry.has_resume = strtoul(fields[0], NULL, 10) != 0;
            entry.frame = (uint32_t) strtoul(fields[1], NULL, 10);
            entry.scale_mode = (uint8_t) strtoul(fields[2], NULL, 10);
            entry.playback_rate_index = (uint8_t) strtoul(fields[3], NULL, 10);
            if (version >= 4) {
                entry.playback_mode = (uint8_t) strtoul(fields[4], NULL, 10);
                entry.subtitle_font_index = (uint8_t) strtoul(fields[5], NULL, 10);
                entry.subtitle_size = (int8_t) strtol(fields[6], NULL, 10);
                entry.subtitle_placement = (uint8_t) strtoul(fields[7], NULL, 10);
                entry.selected_subtitle_track = (uint16_t) strtoul(fields[8], NULL, 10);
                entry.video_align_x = (int8_t) clamp_video_align((int) strtol(fields[9], NULL, 10));
                entry.video_align_y = (int8_t) clamp_video_align((int) strtol(fields[10], NULL, 10));
                if (version >= 6) {
                    entry.realtime_frame_skip = strtoul(fields[11], NULL, 10) != 0;
                }
            } else if (version >= 3) {
                entry.playback_mode = (uint8_t) strtoul(fields[4], NULL, 10);
                entry.subtitle_font_index = (uint8_t) strtoul(fields[5], NULL, 10);
                entry.subtitle_size = (int8_t) strtol(fields[6], NULL, 10);
                entry.subtitle_placement = (uint8_t) strtoul(fields[7], NULL, 10);
                entry.selected_subtitle_track = (uint16_t) strtoul(fields[8], NULL, 10);
            } else {
                entry.subtitle_font_index = (uint8_t) strtoul(fields[4], NULL, 10);
                entry.subtitle_size = (int8_t) strtol(fields[5], NULL, 10);
                entry.subtitle_placement = (uint8_t) strtoul(fields[6], NULL, 10);
                entry.selected_subtitle_track = (uint16_t) strtoul(fields[7], NULL, 10);
            }
            if (version >= 6) {
                path = fields[12];
            } else if (version >= 4) {
                path = fields[11];
            } else if (version >= 3) {
                path = fields[9];
            } else {
                path = fields[8];
            }
        } else {
            char *separator = strchr(line, '\t');
            if (!separator) {
                continue;
            }
            *separator = '\0';
            path = separator + 1;
            entry.has_resume = true;
            entry.frame = (uint32_t) strtoul(line, NULL, 10);
        }

        path[strcspn(path, "\r\n")] = '\0';
        if (path[0] == '\0') {
            continue;
        }
        entry.path = dup_string(path);
        history->entries[history->count] = entry;
        if (!history->entries[history->count].path) {
            fclose(file);
            free_history_store(history);
            return false;
        }
        history->count++;
    }
    fclose(file);
    return true;
}

bool load_history_store(const char *movie_path, HistoryStore *history)
{
    char history_path[MAX_PATH_LEN];
    history_path_for_movie(movie_path, history_path, sizeof(history_path));
    return load_history_store_from_path(history_path, history);
}

bool save_history_store_to_path(const char *history_path, const HistoryStore *history)
{
    FILE *file;
    size_t index;

    file = fopen(history_path, "wb");
    if (!file) {
        return false;
    }
    fputs(HISTORY_MAGIC_V6 "\n", file);
    fprintf(file, "@theme\t%u\n", (unsigned) ui_theme_clamp((int) history->theme_id));
    for (index = 0; index < history->count && index < HISTORY_MAX_ENTRIES; ++index) {
        fprintf(
            file,
            "%u\t%lu\t%u\t%u\t%u\t%u\t%d\t%u\t%u\t%d\t%d\t%u\t%s\n",
            history->entries[index].has_resume ? 1U : 0U,
            (unsigned long) history->entries[index].frame,
            (unsigned) history->entries[index].scale_mode,
            (unsigned) history->entries[index].playback_rate_index,
            (unsigned) history->entries[index].playback_mode,
            (unsigned) history->entries[index].subtitle_font_index,
            (int) history->entries[index].subtitle_size,
            (unsigned) history->entries[index].subtitle_placement,
            (unsigned) history->entries[index].selected_subtitle_track,
            (int) history->entries[index].video_align_x,
            (int) history->entries[index].video_align_y,
            history->entries[index].realtime_frame_skip ? 1U : 0U,
            history->entries[index].path
        );
    }
    fclose(file);
    return true;
}

bool save_history_store(const char *movie_path, const HistoryStore *history)
{
    char history_path[MAX_PATH_LEN];

    history_path_for_movie(movie_path, history_path, sizeof(history_path));
    return save_history_store_to_path(history_path, history);
}

void ui_load_theme_for_directory(const char *directory)
{
    char history_path[MAX_PATH_LEN];
    HistoryStore history;

    history_path_for_directory(directory, history_path, sizeof(history_path));
    if (load_history_store_from_path(history_path, &history)) {
        ui_set_theme(history.theme_id);
        free_history_store(&history);
    }
}

void ui_write_theme_for_directory(const char *directory)
{
    char history_path[MAX_PATH_LEN];
    HistoryStore history;

    history_path_for_directory(directory, history_path, sizeof(history_path));
    memset(&history, 0, sizeof(history));
    history.theme_id = g_ui_theme_id;
    if (load_history_store_from_path(history_path, &history)) {
        history.theme_id = g_ui_theme_id;
        save_history_store_to_path(history_path, &history);
        free_history_store(&history);
        return;
    }
    history.theme_id = g_ui_theme_id;
    save_history_store_to_path(history_path, &history);
}

void ui_save_theme_for_directory(const char *directory)
{
    snprintf(
        g_pending_theme_directory,
        sizeof(g_pending_theme_directory),
        "%s",
        directory && directory[0] != '\0' ? directory : "."
    );
    g_pending_theme_save = true;
}

void ui_save_theme_for_movie(const char *movie_path)
{
    char directory[MAX_PATH_LEN];

    if (movie_path && movie_path[0] != '\0') {
        snprintf(directory, sizeof(directory), "%s", movie_path);
        strip_filename(directory);
    } else {
        snprintf(directory, sizeof(directory), ".");
    }
    ui_save_theme_for_directory(directory);
}

void flush_queued_theme_save(const char *reason)
{
    uint32_t start_ms;
    uint32_t elapsed_ms;

    if (!g_pending_theme_save) {
        return;
    }

    start_ms = monotonic_clock_now_ms();
    ui_write_theme_for_directory(g_pending_theme_directory);
    elapsed_ms = monotonic_clock_now_ms() - start_ms;
    debug_tracef(
        "theme queued flush reason=%s ms=%lu",
        reason ? reason : "unknown",
        (unsigned long) elapsed_ms
    );
    g_pending_theme_directory[0] = '\0';
    g_pending_theme_save = false;
}

int history_find_entry_index(const HistoryStore *history, const char *movie_path)
{
    size_t index;
    for (index = 0; index < history->count; ++index) {
        if (history->entries[index].path && strcmp(history->entries[index].path, movie_path) == 0) {
            return (int) index;
        }
    }
    return -1;
}

void history_remove_entry(HistoryStore *history, const char *movie_path)
{
    int found_index = history_find_entry_index(history, movie_path);
    size_t index;
    if (found_index < 0) {
        return;
    }
    free(history->entries[found_index].path);
    for (index = (size_t) found_index; index + 1 < history->count; ++index) {
        history->entries[index] = history->entries[index + 1];
    }
    memset(&history->entries[history->count - 1], 0, sizeof(history->entries[history->count - 1]));
    history->count--;
}

void history_upsert_entry(
    HistoryStore *history,
    const char *movie_path,
    const Movie *movie,
    bool has_resume,
    uint32_t frame,
    ScaleMode scale_mode,
    size_t playback_rate_index,
    PlaybackMode playback_mode,
    bool realtime_frame_skip,
    size_t subtitle_font_index,
    int subtitle_size,
    SubtitlePlacement subtitle_placement,
    VideoAlign video_align_x,
    VideoAlign video_align_y
)
{
    HistoryEntry entry;
    size_t index;

    history_entry_init_defaults(&entry);
    history_remove_entry(history, movie_path);
    entry.path = dup_string(movie_path);
    entry.has_resume = has_resume;
    entry.frame = frame;
    entry.scale_mode = (uint8_t) clamp_int((int) scale_mode, SCALE_FIT, SCALE_NATIVE);
    entry.playback_rate_index = (uint8_t) clamp_int((int) playback_rate_index, 0, (int) (PLAYBACK_RATE_COUNT - 1U));
    entry.playback_mode = (uint8_t) clamp_int((int) playback_mode, PLAYBACK_MODE_ONCE, PLAYBACK_MODE_COUNT - 1);
    entry.realtime_frame_skip = realtime_frame_skip;
    entry.subtitle_font_index = (uint8_t) clamp_int((int) subtitle_font_index, 0, (int) (SUBTITLE_FONT_CHOICE_COUNT - 1U));
    entry.subtitle_size = (int8_t) clamp_int(subtitle_size, -1, 3);
    entry.subtitle_placement = (uint8_t) clamp_int((int) subtitle_placement, SUBTITLE_POS_BAR_BOTTOM, SUBTITLE_POS_COUNT - 1);
    entry.video_align_x = (int8_t) clamp_video_align((int) video_align_x);
    entry.video_align_y = (int8_t) clamp_video_align((int) video_align_y);
    entry.selected_subtitle_track = movie ? movie->selected_subtitle_track : 0;
    if (!entry.path) {
        return;
    }
    if (history->count == HISTORY_MAX_ENTRIES) {
        free(history->entries[HISTORY_MAX_ENTRIES - 1].path);
        history->count--;
    }
    for (index = history->count; index > 0; --index) {
        history->entries[index] = history->entries[index - 1];
    }
    history->entries[0] = entry;
    history->count++;
}

bool history_resume_frame_for_movie(const char *movie_path, uint32_t *out_frame) __attribute__((unused));
bool history_resume_frame_for_movie(const char *movie_path, uint32_t *out_frame)
{
    HistoryStore history;
    int found_index;
    if (!load_history_store(movie_path, &history)) {
        return false;
    }
    found_index = history_find_entry_index(&history, movie_path);
    if (found_index >= 0 && history.entries[found_index].has_resume) {
        *out_frame = history.entries[found_index].frame;
        free_history_store(&history);
        return true;
    }
    free_history_store(&history);
    return false;
}

bool should_save_history_snapshot(const MovieHeader *header, uint32_t frame)
{
    uint32_t now_ms;
    uint32_t duration_ms;

    if (!header) {
        return false;
    }
    now_ms = movie_header_frame_time_ms(header, frame);
    duration_ms = movie_header_frame_time_ms(header, header->frame_count);
    return now_ms >= RESUME_MIN_MS && (duration_ms == 0 || now_ms + RESUME_CLEAR_TAIL_MS < duration_ms);
}

void update_movie_file_resume_from_snapshot(MovieFile *file, const DeferredHistorySave *request)
{
    uint32_t frame;

    if (!file || !request) {
        return;
    }

    frame = request->current_frame;
    if (request->header.frame_count > 0 && frame >= request->header.frame_count) {
        frame = request->header.frame_count - 1U;
    }

    file->has_resume = should_save_history_snapshot(&request->header, frame);
    file->resume_frame = file->has_resume ? frame : 0;
    file->resume_time_known = false;
    file->resume_ms = 0;
    file->duration_ms = 0;
    if (file->has_resume) {
        file->resume_ms = movie_header_frame_time_ms(&request->header, frame);
        file->duration_ms = movie_header_frame_time_ms(&request->header, request->header.frame_count);
        file->resume_time_known = true;
    }
}

void update_movie_picker_cache_resume_from_snapshot(
    MoviePickerCache *cache,
    const DeferredHistorySave *request
)
{
    size_t index;

    if (!cache || !cache->valid || !request || request->movie_path[0] == '\0') {
        return;
    }
    for (index = 0; index < cache->count; ++index) {
        if (cache->files[index].path && strcmp(cache->files[index].path, request->movie_path) == 0) {
            update_movie_file_resume_from_snapshot(&cache->files[index], request);
            return;
        }
    }
}

void queue_history_save_from_movie(
    DeferredHistorySave *request,
    MoviePickerCache *cache,
    const char *movie_path,
    const Movie *movie,
    ScaleMode scale_mode,
    size_t playback_rate_index,
    PlaybackMode playback_mode,
    bool realtime_frame_skip,
    size_t subtitle_font_index,
    int subtitle_size,
    SubtitlePlacement subtitle_placement,
    VideoAlign video_align_x,
    VideoAlign video_align_y
)
{
    if (!request || !movie_path || !movie) {
        return;
    }

    memset(request, 0, sizeof(*request));
    snprintf(request->movie_path, sizeof(request->movie_path), "%s", movie_path);
    request->header = movie->header;
    request->current_frame = movie->current_frame;
    request->selected_subtitle_track = movie->selected_subtitle_track;
    request->scale_mode = scale_mode;
    request->playback_rate_index = playback_rate_index;
    request->playback_mode = playback_mode;
    request->realtime_frame_skip = realtime_frame_skip;
    request->subtitle_font_index = subtitle_font_index;
    request->subtitle_size = subtitle_size;
    request->subtitle_placement = subtitle_placement;
    request->video_align_x = video_align_x;
    request->video_align_y = video_align_y;
    request->pending = true;
    update_movie_picker_cache_resume_from_snapshot(cache, request);
}

void flush_queued_history_save(DeferredHistorySave *request, const char *reason)
{
    HistoryStore history;
    Movie snapshot;
    bool has_resume;
    uint32_t frame;
    uint32_t start_ms;
    uint32_t elapsed_ms;

    if (!request || !request->pending || request->movie_path[0] == '\0') {
        return;
    }
    if (!load_history_store(request->movie_path, &history)) {
        return;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.header = request->header;
    snapshot.current_frame = request->current_frame;
    snapshot.selected_subtitle_track = request->selected_subtitle_track;
    frame = request->current_frame;
    if (request->header.frame_count > 0 && frame >= request->header.frame_count) {
        frame = request->header.frame_count - 1U;
    }
    has_resume = should_save_history_snapshot(&request->header, frame);
    history.theme_id = g_ui_theme_id;

    start_ms = monotonic_clock_now_ms();
    history_upsert_entry(
        &history,
        request->movie_path,
        &snapshot,
        has_resume,
        has_resume ? frame : 0,
        request->scale_mode,
        request->playback_rate_index,
        request->playback_mode,
        request->realtime_frame_skip,
        request->subtitle_font_index,
        request->subtitle_size,
        request->subtitle_placement,
        request->video_align_x,
        request->video_align_y
    );
    save_history_store(request->movie_path, &history);
    elapsed_ms = monotonic_clock_now_ms() - start_ms;
    if (g_pending_theme_save) {
        char history_directory[MAX_PATH_LEN];

        snprintf(history_directory, sizeof(history_directory), "%s", request->movie_path);
        strip_filename(history_directory);
        if (strcmp(history_directory, g_pending_theme_directory) == 0) {
            g_pending_theme_directory[0] = '\0';
            g_pending_theme_save = false;
        }
    }
    debug_tracef(
        "history queued flush reason=%s frame=%lu ms=%lu",
        reason ? reason : "unknown",
        (unsigned long) frame,
        (unsigned long) elapsed_ms
    );
    free_history_store(&history);
    memset(request, 0, sizeof(*request));
}

bool save_screenshot_bitmap_in_directory(SDL_Surface *screen, const char *directory, char *saved_path, size_t saved_path_size)
{
    char screenshot_directory[MAX_PATH_LEN];
    int index;

    if (saved_path && saved_path_size > 0) {
        saved_path[0] = '\0';
    }
    if (!screen) {
        return false;
    }

    if (directory && directory[0] != '\0') {
        snprintf(screenshot_directory, sizeof(screenshot_directory), "%s", directory);
    } else {
        snprintf(screenshot_directory, sizeof(screenshot_directory), ".");
    }

    for (index = 1; index <= 9999; ++index) {
        char candidate[MAX_PATH_LEN];
        FILE *existing;
        int candidate_len = snprintf(candidate, sizeof(candidate), "%s/ndvideo-shot-%04d.bmp.tns", screenshot_directory, index);

        if (candidate_len < 0 || (size_t) candidate_len >= sizeof(candidate)) {
            return false;
        }
        existing = fopen(candidate, "rb");
        if (existing) {
            fclose(existing);
            continue;
        }
        if (SDL_SaveBMP(screen, candidate) == 0) {
            if (saved_path && saved_path_size > 0) {
                snprintf(saved_path, saved_path_size, "%s", candidate);
            }
            debug_tracef("screenshot saved path=%s", candidate);
            return true;
        }
        debug_tracef("screenshot save fail path=%s", candidate);
        return false;
    }

    return false;
}

bool save_screenshot_bitmap(SDL_Surface *screen, const char *movie_path, char *saved_path, size_t saved_path_size)
{
    char directory[MAX_PATH_LEN];

    if (movie_path && movie_path[0] != '\0') {
        snprintf(directory, sizeof(directory), "%s", movie_path);
        strip_filename(directory);
    } else {
        snprintf(directory, sizeof(directory), ".");
    }
    return save_screenshot_bitmap_in_directory(screen, directory, saved_path, saved_path_size);
}

void prepare_screenshot_preview(ScreenshotPreviewState *preview, SDL_Surface *screen, const char *saved_path)
{
    SDL_Surface *thumbnail;

    if (!preview) {
        return;
    }

    clear_screenshot_preview(preview);
    if (!screen || !saved_path || saved_path[0] == '\0') {
        return;
    }

    thumbnail = create_scaled_surface_from_surface(screen, SCREENSHOT_PREVIEW_MAX_W, SCREENSHOT_PREVIEW_MAX_H);
    if (!thumbnail) {
        return;
    }

    preview->surface = thumbnail;
    snprintf(preview->label, sizeof(preview->label), "Saved %.72s", filename_from_path(saved_path));
    preview->until_ms = monotonic_clock_now_ms() + SCREENSHOT_PREVIEW_MS;
}

bool update_seek_bar_preview(Movie *movie, SeekBarPreviewState *preview, const PointerState *pointer, bool show_ui, uint32_t now_ms)
{
    SDL_Rect bar = progress_bar_rect();
    int marker_x;
    uint32_t duration_ms;
    uint32_t target_ms;
    uint32_t target_frame;
    int chunk_index;

    if (!preview) {
        return false;
    }

    preview->over_bar = false;
    if (!movie || !pointer || !pointer->visible || !show_ui ||
        pointer->y < SCREEN_H - UI_BAR_H || pointer->y >= SCREEN_H) {
        preview->tracking = false;
        preview->last_pointer_x = -1;
        preview->suppress_until_pointer_moves = false;
        preview->suppress_marker_x = -1;
        preview->suppress_frame_index = UINT32_MAX;
        clear_seek_bar_preview_decode_job(preview);
        return false;
    }

    preview->over_bar = true;
    marker_x = progress_bar_marker_x_from_pointer(&bar, pointer->x);
    preview->marker_x = marker_x;
    if (movie->header.frame_count == 0) {
        preview->hover_ms = 0;
        return true;
    }
    duration_ms = movie_duration_ms(movie);
    if (!progress_bar_target_frame_for_marker(movie, &bar, marker_x, &target_frame, &target_ms)) {
        return false;
    }
    preview->hover_ms = duration_ms > 0 ? target_ms : 0;
    if (!movie->codec_ops ||
        !movie->codec_ops->supports_incremental_seek_preview ||
        !movie->codec_ops->supports_incremental_seek_preview(movie)) {
        clear_seek_bar_preview_decode_job(preview);
        return true;
    }
    chunk_index = movie_chunk_for_frame(movie, target_frame);
    if (chunk_index < 0) {
        return false;
    }
    if (preview->suppress_until_pointer_moves) {
        if (preview->suppress_marker_x == marker_x &&
            preview->suppress_frame_index == target_frame) {
            return true;
        }
        preview->suppress_until_pointer_moves = false;
        preview->suppress_marker_x = -1;
        preview->suppress_frame_index = UINT32_MAX;
    }

    if (!preview->tracking || pointer->moved || preview->last_pointer_x != marker_x) {
        preview->tracking = true;
        preview->last_pointer_x = marker_x;
        preview->last_move_ms = now_ms;
        return true;
    }
    if ((int32_t) (now_ms - preview->last_move_ms) < (int32_t) SEEK_BAR_PREVIEW_DEBOUNCE_MS) {
        return true;
    }
    if (preview->surface && preview->decoded_frame_index == target_frame) {
        return true;
    }
    if (preview->decode_job.complete && preview->decode_job.chunk_index == chunk_index) {
        if (target_frame >= preview->decode_job.next_frame) {
            preview->decode_job.target_frame = target_frame;
            preview->decode_job.active = true;
            preview->decode_job.complete = false;
            return true;
        }
    }
    if (preview->decode_job.active && preview->decode_job.chunk_index == chunk_index) {
        if (preview->decode_job.target_frame == target_frame) {
            return true;
        }
        if (target_frame >= preview->decode_job.next_frame) {
            preview->decode_job.target_frame = target_frame;
            return true;
        }
    }

    if (!begin_seek_bar_preview_decode(movie, preview, chunk_index, target_frame)) {
        return false;
    }
    return true;
}

