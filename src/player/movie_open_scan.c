#include "player_internal.h"

bool load_subtitles(
    Movie *movie,
    FILE *file,
    const MovieHeader *header,
    SubtitleCue **out_cues,
    SubtitleTrack **out_tracks,
    uint16_t *out_track_count,
    LoadingProgress *loading_progress
)
{
    SubtitleCue *cues = NULL;
    SubtitleTrack *tracks = NULL;
    uint32_t cue_index = 0;
    uint32_t track_index = 0;

    *out_cues = NULL;
    *out_tracks = NULL;
    *out_track_count = 0;
    if (!header->subtitle_count) {
        debug_tracef("open subtitles none");
        return true;
    }
    loading_progress_tick(loading_progress, false);

    cues = (SubtitleCue *) calloc(header->subtitle_count, sizeof(SubtitleCue));
    if (!cues) {
        debug_failf("subtitle cue alloc failed count=%lu", (unsigned long) header->subtitle_count);
        return false;
    }
    if (fseek(file, (long) header->subtitle_offset, SEEK_SET) != 0) {
        debug_failf("subtitle seek failed offset=%lu", (unsigned long) header->subtitle_offset);
        free(cues);
        return false;
    }
    if (movie) {
        movie->current_file_pos = -1;
    }

    {
        uint8_t track_count_bytes[2];
        uint32_t cue_cursor = 0;
        if (fread(track_count_bytes, 1, sizeof(track_count_bytes), file) != sizeof(track_count_bytes)) {
            debug_failf("subtitle track count read failed");
            free(cues);
            return false;
        }
        loading_progress_tick(loading_progress, false);
        *out_track_count = read_le16(track_count_bytes);
        if (*out_track_count == 0) {
            debug_tracef("open subtitles zero tracks");
            free(cues);
            return true;
        }
        tracks = (SubtitleTrack *) calloc(*out_track_count, sizeof(SubtitleTrack));
        if (!tracks) {
            debug_failf("subtitle track alloc failed count=%u", (unsigned) *out_track_count);
            free(cues);
            return false;
        }
        for (track_index = 0; track_index < *out_track_count; ++track_index) {
            uint8_t meta[6];
            uint16_t name_len;
            if (fread(meta, 1, sizeof(meta), file) != sizeof(meta)) {
                debug_failf("subtitle track meta read failed track=%lu", (unsigned long) track_index);
                free(cues);
                free(tracks);
                return false;
            }
            name_len = read_le16(meta);
            tracks[track_index].cue_start = cue_cursor;
            tracks[track_index].cue_count = read_le32(meta + 2);
            if (cue_cursor + tracks[track_index].cue_count > header->subtitle_count) {
                debug_failf(
                    "subtitle cue range overflow track=%lu start=%lu count=%lu total=%lu",
                    (unsigned long) track_index,
                    (unsigned long) cue_cursor,
                    (unsigned long) tracks[track_index].cue_count,
                    (unsigned long) header->subtitle_count
                );
                free(cues);
                free(tracks);
                return false;
            }
            tracks[track_index].name = (char *) malloc(name_len + 1);
            if (!tracks[track_index].name) {
                debug_failf("subtitle track name alloc failed track=%lu len=%u", (unsigned long) track_index, (unsigned) name_len);
                free(cues);
                free(tracks);
                return false;
            }
            if (fread(tracks[track_index].name, 1, name_len, file) != name_len) {
                debug_failf("subtitle track name read failed track=%lu len=%u", (unsigned long) track_index, (unsigned) name_len);
                free(cues);
                free(tracks[track_index].name);
                free(tracks);
                return false;
            }
            tracks[track_index].name[name_len] = '\0';
            cue_cursor += tracks[track_index].cue_count;
            loading_progress_tick(loading_progress, false);
        }
        if (cue_cursor != header->subtitle_count) {
            debug_failf(
                "subtitle cue count mismatch tracks=%lu cues=%lu expected=%lu",
                (unsigned long) *out_track_count,
                (unsigned long) cue_cursor,
                (unsigned long) header->subtitle_count
            );
            free(cues);
            for (track_index = 0; track_index < *out_track_count; ++track_index) {
                free(tracks[track_index].name);
            }
            free(tracks);
            return false;
        }
    }

    for (cue_index = 0; cue_index < header->subtitle_count; ++cue_index) {
        uint8_t meta[22];
        size_t meta_size = header->version >= MOVIE_VERSION_POSITIONED_SUBS ? 22U : 10U;
        uint16_t text_len;
        if (fread(meta, 1, meta_size, file) != meta_size) {
            debug_failf("subtitle cue meta read failed cue=%lu", (unsigned long) cue_index);
            goto fail;
        }
        cues[cue_index].start_ms = read_le32(meta);
        cues[cue_index].end_ms = read_le32(meta + 4);
        text_len = read_le16(meta + 8);
        if (meta_size > 10U) {
            cues[cue_index].position_mode = meta[10];
            cues[cue_index].align = meta[11];
            cues[cue_index].pos_x = read_le16(meta + 12);
            cues[cue_index].pos_y = read_le16(meta + 14);
            cues[cue_index].margin_l = read_le16(meta + 16);
            cues[cue_index].margin_r = read_le16(meta + 18);
            cues[cue_index].margin_v = read_le16(meta + 20);
        }
        cues[cue_index].text = (char *) malloc(text_len + 1);
        if (!cues[cue_index].text) {
            debug_failf("subtitle text alloc failed cue=%lu len=%u", (unsigned long) cue_index, (unsigned) text_len);
            goto fail;
        }
        if (fread(cues[cue_index].text, 1, text_len, file) != text_len) {
            debug_failf("subtitle text read failed cue=%lu len=%u", (unsigned long) cue_index, (unsigned) text_len);
            goto fail;
        }
        cues[cue_index].text[text_len] = '\0';
        loading_progress_tick(loading_progress, false);
    }
    for (track_index = 0; track_index < *out_track_count; ++track_index) {
        uint32_t start_index = tracks[track_index].cue_start;
        uint32_t end_index = start_index + tracks[track_index].cue_count;

        if (end_index > header->subtitle_count) {
            end_index = header->subtitle_count;
        }
        tracks[track_index].supports_positioning = 0;
        for (cue_index = start_index; cue_index < end_index; ++cue_index) {
            if ((cues[cue_index].position_mode == SUBTITLE_CUE_POSITION_MARGIN ||
                 cues[cue_index].position_mode == SUBTITLE_CUE_POSITION_ABSOLUTE) &&
                cues[cue_index].align >= 1 &&
                cues[cue_index].align <= 9) {
                tracks[track_index].supports_positioning = 1;
                break;
            }
        }
    }

    debug_tracef(
        "open subtitles loaded tracks=%u cues=%lu",
        (unsigned) *out_track_count,
        (unsigned long) header->subtitle_count
    );
    *out_cues = cues;
    *out_tracks = tracks;
    return true;

fail:
    if (cues) {
        for (cue_index = 0; cue_index < header->subtitle_count; ++cue_index) {
            free(cues[cue_index].text);
        }
    }
    if (tracks) {
        for (track_index = 0; track_index < *out_track_count; ++track_index) {
            free(tracks[track_index].name);
        }
    }
    free(cues);
    free(tracks);
    *out_track_count = 0;
    return false;
}

bool load_movie(const char *path, Movie *movie, LoadingProgress *loading_progress)
{
    size_t framebuffer_words;
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    movie->last_read_bytes = 2048U;
    movie->last_read_time_ms = 1U;
    {
        int prefetch_index;
        int ui_chunk_index;
        for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
            movie->prefetched[prefetch_index].chunk_index = -1;
        }
        for (ui_chunk_index = 0; ui_chunk_index < UI_BUFFER_CHUNK_CACHE_COUNT; ++ui_chunk_index) {
            movie->ui_buffer_chunks[ui_chunk_index] = -1;
        }
    }
    movie->decoded_local_frame = -1;
    debug_tracef("open start path=%s", path ? path : "(null)");
    loading_progress_tick(loading_progress, false);

    movie->file = fopen(path, "rb");
    if (!movie->file) {
        debug_failf("open failed: fopen");
        return false;
    }
    movie->current_file_pos = 0;
    loading_progress_tick(loading_progress, false);
    if (fread(&movie->header, 1, sizeof(movie->header), movie->file) != sizeof(movie->header)) {
        debug_failf("open failed: header read");
        return false;
    }
    movie->current_file_pos = (long) sizeof(movie->header);
    if (memcmp(movie->header.magic, "NVP1", 4) != 0) {
        debug_failf("open failed: bad magic");
        return false;
    }
    movie->codec = movie_codec_from_header(&movie->header);
    movie->codec_ops = movie_codec_ops(movie->codec);
    if (!movie->codec_ops) {
        debug_failf(
            "open failed: unsupported version=%u flags=0x%04x",
            (unsigned) movie->header.version,
            (unsigned) movie->header.flags
        );
        return false;
    }
    debug_tracef(
        "open header version=%u codec=%s flags=0x%04x video=%ux%u frames=%lu chunks=%lu subtitles=%lu",
        (unsigned) movie->header.version,
        movie_codec_name(movie->codec),
        (unsigned) movie->header.flags,
        (unsigned) movie->header.video_width,
        (unsigned) movie->header.video_height,
        (unsigned long) movie->header.frame_count,
        (unsigned long) movie->header.chunk_count,
        (unsigned long) movie->header.subtitle_count
    );
    loading_progress_tick(loading_progress, false);
    movie->chunk_index = (ChunkIndexEntry *) calloc(movie->header.chunk_count, sizeof(ChunkIndexEntry));
    if (!movie->chunk_index) {
        debug_failf("open failed: chunk index alloc count=%lu", (unsigned long) movie->header.chunk_count);
        return false;
    }
    if (fseek(movie->file, (long) movie->header.index_offset, SEEK_SET) != 0) {
        debug_failf("open failed: index seek offset=%lu", (unsigned long) movie->header.index_offset);
        return false;
    }
    movie->current_file_pos = (long) movie->header.index_offset;
    if (fread(movie->chunk_index, sizeof(ChunkIndexEntry), movie->header.chunk_count, movie->file) != movie->header.chunk_count) {
        debug_failf("open failed: chunk index read count=%lu", (unsigned long) movie->header.chunk_count);
        return false;
    }
    movie->current_file_pos += (long) (sizeof(ChunkIndexEntry) * movie->header.chunk_count);
    debug_tracef("open index loaded chunks=%lu", (unsigned long) movie->header.chunk_count);
    loading_progress_tick(loading_progress, false);
    framebuffer_words = (size_t) movie->header.video_width * movie->header.video_height;
    movie->framebuffer = (uint16_t *) calloc(framebuffer_words, sizeof(uint16_t));
    if (!movie->framebuffer) {
        debug_failf("open failed: framebuffer alloc words=%lu", (unsigned long) framebuffer_words);
        return false;
    }
    if (movie->codec_ops->global_init && !movie->codec_ops->global_init()) {
        return false;
    }
    if (!movie->codec_ops->open(movie)) {
        return false;
    }
    loading_progress_tick(loading_progress, false);
    movie->frame_surface = SDL_CreateRGBSurfaceFrom(
        movie->framebuffer,
        movie->header.video_width,
        movie->header.video_height,
        16,
        movie->header.video_width * 2,
        0xF800, 0x07E0, 0x001F, 0
    );
    if (!movie->frame_surface) {
        debug_failf("open failed: SDL surface create");
        return false;
    }
    if (!decode_to_frame(movie, 0)) {
        debug_tracef("open failed during initial frame decode");
        return false;
    }
    debug_tracef("open first frame ok");
    loading_progress_tick(loading_progress, false);
    if (!load_subtitles(movie, movie->file, &movie->header, &movie->subtitles, &movie->subtitle_tracks, &movie->subtitle_track_count, loading_progress)) {
        debug_tracef("open subtitles disabled after alloc/read failure");
    }
    loading_progress_tick(loading_progress, false);
    return true;
}

bool key_pressed_edge(t_key key, bool *previous_state)
{
    bool current_state = isKeyPressed(key) ? true : false;
    bool pressed = current_state && !(*previous_state);
    *previous_state = current_state;
    return pressed;
}

bool on_key_pressed_edge(bool *previous_state)
{
    bool current_state = on_key_pressed() ? true : false;
    bool pressed = current_state && !(*previous_state);
    *previous_state = current_state;
    return pressed;
}

unsigned os_close_document_addr(void)
{
    /* Same OS-specific close_document table Ndless uses after installer launch. */
    static const unsigned close_document_addrs[] = {
        0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
        0x0, 0x0, 0x0, 0x0,
        0x0, 0x0, 0x0, 0x0,
        0x0, 0x0, 0x0, 0x0,
        0x0, 0x0,
        0x0, 0x0,
        0x0, 0x0,
        0x0, 0x0,
        0x0, 0x0,
        0x1000b240, 0x1000b23c,
        0x0, 0x0,
        0x1000b278, 0x1000b2b0,
        0x100265d4, 0x10026614, 0x1002660c,
        0x1000b4e4, 0x1000b524,
        0x10028610, 0x10028770, 0x100287a0,
        0x1000b4e0, 0x1000b514,
        0x10028560, 0x10028664, 0x1002867c
    };

    return nl_osvalue(
        close_document_addrs,
        (unsigned) (sizeof(close_document_addrs) / sizeof(close_document_addrs[0]))
    );
}

void queue_os_home_calculator_shortcut(void)
{
    if (!nl_hassyscall(send_key_event)) {
        return;
    }

    const unsigned short a_shortcut = (unsigned short) (('a' << 8) | 'a');
    struct s_ns_event event;
    memset(&event, 0, sizeof(event));
    event.ascii = 'a';
    event.key = 'a';

    event.type = 0x8;
    send_key_event(&event, a_shortcut, FALSE, FALSE);

    event.type = 0x10;
    send_key_event(&event, a_shortcut, TRUE, FALSE);
}

void return_to_os_home_menu(void)
{
    unsigned close_document_addr = os_close_document_addr();

    if (close_document_addr != 0 && !nl_loaded_by_3rd_party_loader()) {
        ((void (*)(void)) close_document_addr)();
    }

    if (nl_hassyscall(refresh_homescr)) {
        refresh_homescr();
    }
}

void yes_teacher_im_mathing(void)
{
    return_to_os_home_menu();
    queue_os_home_calculator_shortcut();
}

int compare_movie_files(const void *lhs, const void *rhs)
{
    const MovieFile *a = (const MovieFile *) lhs;
    const MovieFile *b = (const MovieFile *) rhs;
    int result = strcmp(a->name ? a->name : "", b->name ? b->name : "");
    if (result != 0) {
        return result;
    }
    return strcmp(a->detail ? a->detail : "", b->detail ? b->detail : "");
}

bool strings_equal_ignore_case(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }
    while (*lhs && *rhs) {
        if (tolower((unsigned char) *lhs) != tolower((unsigned char) *rhs)) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

bool read_movie_file_timing(const char *path, uint32_t resume_frame, uint32_t *resume_ms, uint32_t *duration_ms)
{
    FILE *file;
    MovieHeader header;
    uint32_t clamped_resume_frame;

    if (resume_ms) {
        *resume_ms = 0;
    }
    if (duration_ms) {
        *duration_ms = 0;
    }
    if (!path || !resume_ms || !duration_ms) {
        return false;
    }

    file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return false;
    }
    fclose(file);

    if (memcmp(header.magic, "NVP1", 4) != 0 || header.fps_num == 0 || header.frame_count == 0) {
        return false;
    }

    clamped_resume_frame = resume_frame;
    if (clamped_resume_frame >= header.frame_count) {
        clamped_resume_frame = header.frame_count - 1U;
    }
    *resume_ms = movie_header_frame_time_ms(&header, clamped_resume_frame);
    *duration_ms = movie_header_frame_time_ms(&header, header.frame_count);
    return true;
}

MovieFile *scan_movies(const char *directory, size_t *out_count)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    MovieFile *files;
    char history_path[MAX_PATH_LEN];
    HistoryStore history;
    bool have_history = false;
    size_t count = 0;
    if (!dir) {
        *out_count = 0;
        return NULL;
    }
    files = (MovieFile *) calloc(PICKER_MAX_FILES, sizeof(MovieFile));
    if (!files) {
        closedir(dir);
        *out_count = 0;
        return NULL;
    }
    history_path_for_directory(directory, history_path, sizeof(history_path));
    have_history = load_history_store_from_path(history_path, &history);
    while ((entry = readdir(dir)) && count < PICKER_MAX_FILES) {
        char joined[MAX_PATH_LEN];
        int joined_len;
        size_t history_index;
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!has_suffix(entry->d_name, ".nvp") && !has_suffix(entry->d_name, ".nvp.tns")) {
            continue;
        }
        joined_len = snprintf(joined, sizeof(joined), "%s/%s", directory, entry->d_name);
        if (joined_len < 0 || (size_t) joined_len >= sizeof(joined)) {
            continue;
        }
        if (!movie_display_fields_for_filename(entry->d_name, &files[count].name, &files[count].detail)) {
            closedir(dir);
            if (have_history) {
                free_history_store(&history);
            }
            free_movie_files(files, count + 1);
            *out_count = 0;
            return NULL;
        }
        files[count].path = dup_string(joined);
        if (!files[count].name || !files[count].path) {
            closedir(dir);
            if (have_history) {
                free_history_store(&history);
            }
            free_movie_files(files, count + 1);
            *out_count = 0;
            return NULL;
        }
        if (have_history) {
            for (history_index = 0; history_index < history.count; ++history_index) {
                if (strcmp(history.entries[history_index].path, files[count].path) == 0) {
                    files[count].has_resume = history.entries[history_index].has_resume;
                    files[count].resume_frame = history.entries[history_index].frame;
                    break;
                }
            }
        }
        if (files[count].has_resume) {
            files[count].resume_time_known = read_movie_file_timing(
                files[count].path,
                files[count].resume_frame,
                &files[count].resume_ms,
                &files[count].duration_ms
            );
        }
        count++;
    }
    closedir(dir);
    if (have_history) {
        free_history_store(&history);
    }
    qsort(files, count, sizeof(MovieFile), compare_movie_files);
    *out_count = count;
    return files;
}

void ensure_movie_picker_cache(MoviePickerCache *cache, const char *directory)
{
    if (!cache || !directory) {
        return;
    }
    if (cache->valid && strcmp(cache->directory, directory) == 0) {
        return;
    }

    clear_movie_picker_cache(cache);
    snprintf(cache->directory, sizeof(cache->directory), "%s", directory);
    cache->files = scan_movies(directory, &cache->count);
    cache->valid = true;
}

bool find_next_movie_path(const char *current_path, char *next_path, size_t next_path_size)
{
    char directory[MAX_PATH_LEN];
    const char *current_filename;
    MovieFile *files;
    size_t count = 0;
    size_t index;
    bool found = false;
    bool using_picker_cache = false;

    if (!current_path || current_path[0] == '\0' || !next_path || next_path_size == 0) {
        return false;
    }

    snprintf(directory, sizeof(directory), "%s", current_path);
    strip_filename(directory);
    current_filename = filename_from_path(current_path);
    if (g_picker_cache.valid && strcmp(g_picker_cache.directory, directory) == 0) {
        files = g_picker_cache.files;
        count = g_picker_cache.count;
        using_picker_cache = true;
    } else {
        files = scan_movies(directory, &count);
    }
    if (!files || count == 0) {
        return false;
    }

    for (index = 0; index < count; ++index) {
        if (strings_equal_ignore_case(filename_from_path(files[index].path), current_filename)) {
            if (index + 1 < count) {
                strncpy(next_path, files[index + 1].path, next_path_size - 1);
                next_path[next_path_size - 1] = '\0';
                found = true;
            }
            break;
        }
    }

    if (!using_picker_cache) {
        free_movie_files(files, count);
    }
    return found;
}

const SubtitleCue *active_subtitle_cue(const Movie *movie, uint32_t now_ms)
{
    uint32_t index;
    uint32_t start_index;
    uint32_t end_index;

    if (!movie || !movie->subtitles) {
        return NULL;
    }
    start_index = 0;
    end_index = movie->header.subtitle_count;

    if (movie->subtitle_track_count > 0 && movie->selected_subtitle_track < movie->subtitle_track_count) {
        start_index = movie->subtitle_tracks[movie->selected_subtitle_track].cue_start;
        end_index = start_index + movie->subtitle_tracks[movie->selected_subtitle_track].cue_count;
    }
    for (index = start_index; index < end_index; ++index) {
        if (now_ms >= movie->subtitles[index].start_ms && now_ms <= movie->subtitles[index].end_ms) {
            return &movie->subtitles[index];
        }
    }
    return NULL;
}

uint32_t h264_incremental_total_mbs(const Movie *movie, const storage_t *decoder)
{
    uint32_t width_mbs;
    uint32_t height_mbs;

    if (decoder && decoder->picSizeInMbs > 0U) {
        return decoder->picSizeInMbs;
    }
    if (!movie) {
        return 0U;
    }
    width_mbs = ((uint32_t) movie->header.video_width + 15U) / 16U;
    height_mbs = ((uint32_t) movie->header.video_height + 15U) / 16U;
    return width_mbs * height_mbs;
}

void update_h264_incremental_rate(uint16_t *avg_mbs_per_ms_q8, uint32_t elapsed_ms, uint32_t decoded_mbs)
{
    uint32_t sample_q8;

    if (!avg_mbs_per_ms_q8 || elapsed_ms == 0U || decoded_mbs == 0U) {
        return;
    }

    sample_q8 = (decoded_mbs << 8) / elapsed_ms;
    if (sample_q8 == 0U) {
        sample_q8 = 1U;
    }
    if (*avg_mbs_per_ms_q8 == 0U) {
        *avg_mbs_per_ms_q8 = (uint16_t) sample_q8;
    } else {
        *avg_mbs_per_ms_q8 = (uint16_t) (((uint32_t) *avg_mbs_per_ms_q8 * 3U + sample_q8 + 2U) / 4U);
    }
}

uint32_t h264_incremental_budget(
    const Movie *movie,
    const storage_t *decoder,
    uint16_t avg_mbs_per_ms_q8,
    uint32_t spare_ms
)
{
    uint32_t rate_q8;
    uint32_t usable_ms;
    uint32_t decoded_mbs = 0U;
    uint32_t total_mbs;
    uint32_t remaining_mbs;
    uint32_t budget;

    if (!movie || !decoder || spare_ms < H264_INCREMENTAL_DECODE_MIN_SPARE_MS) {
        return 0U;
    }

    total_mbs = h264_incremental_total_mbs(movie, decoder);
    decoded_mbs = decoder->slice->numDecodedMbs;
    remaining_mbs = total_mbs > decoded_mbs ? (total_mbs - decoded_mbs) : 1U;
    usable_ms = spare_ms > H264_INCREMENTAL_DECODE_BUDGET_GUARD_MS
        ? (spare_ms - H264_INCREMENTAL_DECODE_BUDGET_GUARD_MS)
        : 1U;
    rate_q8 = avg_mbs_per_ms_q8 > 0U
        ? (uint32_t) avg_mbs_per_ms_q8
        : H264_INCREMENTAL_DECODE_DEFAULT_MBS_PER_MS_Q8;
    budget = (usable_ms * rate_q8) >> 8;
    if (budget == 0U) {
        budget = 1U;
    }
    if (budget > remaining_mbs) {
        budget = remaining_mbs;
    }
    return budget;
}

