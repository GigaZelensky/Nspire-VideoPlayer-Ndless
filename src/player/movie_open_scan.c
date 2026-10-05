#include "player_internal.h"
#include "storage_read_stream.h"
#include <errno.h>
#include "movie/nvp_validation.h"
#include "timing_math.h"
#include "raw_player_io.h"
#include "movie_crypto_session.h"

bool load_subtitles(
    Movie *movie,
    FILE *file,
    const MovieHeader *header,
    SubtitleCue **out_cues,
    SubtitleTrack **out_tracks,
    uint16_t *out_track_count,
    uint8_t **out_storage,
    size_t *out_storage_size,
    LoadingProgress *loading_progress
)
{
    uint8_t *serialized = NULL;
    uint8_t *storage = NULL;
    SubtitleCue *cues;
    SubtitleTrack *tracks;
    uint16_t track_count;
    uint32_t cue_index;
    uint32_t track_index;
    uint32_t cue_cursor = 0;
    long file_size;
    size_t subtitle_bytes;
    size_t cue_meta_size;
    size_t cursor;
    size_t string_bytes = 0;
    size_t cue_bytes;
    size_t metadata_bytes;
    size_t storage_bytes;
    char *text;

    if (!out_cues || !out_tracks || !out_track_count || !out_storage || !out_storage_size) {
        return false;
    }
    *out_cues = NULL;
    *out_tracks = NULL;
    *out_track_count = 0;
    *out_storage = NULL;
    *out_storage_size = 0;
    if (movie) {
        movie->subtitle_lookup_valid = false;
    }
    if (!header || !file) {
        return false;
    }
    if (!header->subtitle_count) {
        debug_tracef("open subtitles none");
        return true;
    }
    loading_progress_tick(loading_progress, false);
    if (movie) {
        movie->current_file_pos = -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        (uint64_t) file_size > INT32_MAX || header->subtitle_offset > (uint32_t) file_size ||
        header->subtitle_count > SIZE_MAX / sizeof(SubtitleCue)) {
        debug_failf("subtitle section bounds invalid");
        goto fail;
    }
    subtitle_bytes = (size_t) file_size - header->subtitle_offset;
    cue_meta_size = header->version >= MOVIE_VERSION_POSITIONED_SUBS ? 22U : 10U;
    if (subtitle_bytes < 8U || header->subtitle_count > (subtitle_bytes - 8U) / cue_meta_size) {
        debug_failf("subtitle cue count exceeds section size");
        goto fail;
    }
    serialized = (uint8_t *) malloc(subtitle_bytes);
    if (!serialized || fseek(file, (long) header->subtitle_offset, SEEK_SET) != 0) {
        debug_failf("subtitle section allocation/seek failed");
        goto fail;
    }
    /* Batch file reads instead of issuing two tiny reads for every string.
     * Bound each read so the loading animation can still advance. */
    for (cursor = 0; cursor < subtitle_bytes;) {
        size_t amount = subtitle_bytes - cursor;
        if (amount > PREFETCH_FILE_BLOCK_SIZE) {
            amount = PREFETCH_FILE_BLOCK_SIZE;
        }
        if (fread(serialized + cursor, 1, amount, file) != amount) {
            debug_failf("subtitle section read failed");
            goto fail;
        }
        cursor += amount;
        loading_progress_tick(loading_progress, false);
    }

    track_count = read_le16(serialized);
    if (track_count == 0 || track_count > (subtitle_bytes - 2U) / 6U) {
        debug_failf("subtitle track count invalid");
        goto fail;
    }
    /* Validate all ranges and calculate the exact resident storage before
     * allocating it. Track/event order is preserved, including unsorted ASS. */
    cursor = 2U;
    for (track_index = 0; track_index < track_count; ++track_index) {
        uint16_t name_len;
        uint32_t count;
        if (subtitle_bytes - cursor < 6U) {
            goto malformed;
        }
        name_len = read_le16(serialized + cursor);
        count = read_le32(serialized + cursor + 2U);
        cursor += 6U;
        if (name_len > subtitle_bytes - cursor || count > header->subtitle_count - cue_cursor) {
            goto malformed;
        }
        cue_cursor += count;
        cursor += name_len;
        string_bytes += (size_t) name_len + 1U;
    }
    if (cue_cursor != header->subtitle_count) {
        goto malformed;
    }
    for (cue_index = 0; cue_index < header->subtitle_count; ++cue_index) {
        uint16_t text_len;
        if (subtitle_bytes - cursor < cue_meta_size) {
            goto malformed;
        }
        if (read_le32(serialized + cursor + 4U) < read_le32(serialized + cursor)) {
            goto malformed;
        }
        text_len = read_le16(serialized + cursor + 8U);
        cursor += cue_meta_size;
        if (text_len > subtitle_bytes - cursor) {
            goto malformed;
        }
        cursor += text_len;
        string_bytes += (size_t) text_len + 1U;
    }
    /* Each removed record header exceeds its added NUL byte, so the validated
     * string total cannot exceed subtitle_bytes. Check the metadata sum too. */
    cue_bytes = (size_t) header->subtitle_count * sizeof(SubtitleCue);
    if (track_count > (SIZE_MAX - cue_bytes) / sizeof(SubtitleTrack)) {
        goto malformed;
    }
    metadata_bytes = cue_bytes + (size_t) track_count * sizeof(SubtitleTrack);
    if (string_bytes > SIZE_MAX - metadata_bytes) {
        goto malformed;
    }
    storage_bytes = metadata_bytes + string_bytes;
    storage = (uint8_t *) malloc(storage_bytes);
    if (!storage) {
        debug_failf("subtitle arena allocation failed size=%lu", (unsigned long) storage_bytes);
        goto fail;
    }
    /* Both structs have pointer alignment; an array of cues keeps tracks
     * aligned. Only clear metadata, whose v9 optional fields default to zero. */
    memset(storage, 0, metadata_bytes);
    cues = (SubtitleCue *) storage;
    tracks = (SubtitleTrack *) (storage + cue_bytes);
    text = (char *) storage + metadata_bytes;
    cursor = 2U;
    cue_cursor = 0;
    for (track_index = 0; track_index < track_count; ++track_index) {
        uint16_t name_len = read_le16(serialized + cursor);
        tracks[track_index].cue_start = cue_cursor;
        tracks[track_index].cue_count = read_le32(serialized + cursor + 2U);
        cue_cursor += tracks[track_index].cue_count;
        cursor += 6U;
        tracks[track_index].name = text;
        memcpy(text, serialized + cursor, name_len);
        text[name_len] = '\0';
        text += (size_t) name_len + 1U;
        cursor += name_len;
    }
    for (cue_index = 0; cue_index < header->subtitle_count; ++cue_index) {
        const uint8_t *meta = serialized + cursor;
        uint16_t text_len = read_le16(meta + 8U);
        cues[cue_index].start_ms = read_le32(meta);
        cues[cue_index].end_ms = read_le32(meta + 4U);
        if (cue_meta_size > 10U) {
            cues[cue_index].position_mode = meta[10];
            cues[cue_index].align = meta[11];
            cues[cue_index].pos_x = read_le16(meta + 12);
            cues[cue_index].pos_y = read_le16(meta + 14);
            cues[cue_index].margin_l = read_le16(meta + 16);
            cues[cue_index].margin_r = read_le16(meta + 18);
            cues[cue_index].margin_v = read_le16(meta + 20);
        }
        cursor += cue_meta_size;
        cues[cue_index].text = text;
        memcpy(text, serialized + cursor, text_len);
        text[text_len] = '\0';
        text += (size_t) text_len + 1U;
        cursor += text_len;
        if ((cue_index & 31U) == 0U) {
            loading_progress_tick(loading_progress, false);
        }
    }
    for (track_index = 0; track_index < track_count; ++track_index) {
        uint32_t end_index = tracks[track_index].cue_start + tracks[track_index].cue_count;
        for (cue_index = tracks[track_index].cue_start; cue_index < end_index; ++cue_index) {
            if ((cues[cue_index].position_mode == SUBTITLE_CUE_POSITION_MARGIN ||
                 cues[cue_index].position_mode == SUBTITLE_CUE_POSITION_ABSOLUTE) &&
                cues[cue_index].align >= 1 && cues[cue_index].align <= 9) {
                tracks[track_index].supports_positioning = 1;
                break;
            }
        }
    }
    free(serialized);
    debug_tracef("open subtitles loaded tracks=%u cues=%lu arena=%lu",
                 (unsigned) track_count, (unsigned long) header->subtitle_count, (unsigned long) storage_bytes);
    *out_cues = cues;
    *out_tracks = tracks;
    *out_track_count = track_count;
    *out_storage = storage;
    *out_storage_size = storage_bytes;
    return true;

malformed:
    debug_failf("subtitle metadata/string bounds invalid");
fail:
    free(serialized);
    free(storage);
    return false;
}

bool load_movie(const char *path, Movie *movie, LoadingProgress *loading_progress,
    MovieCodec *missing_codec)
{
    size_t framebuffer_words;
    long file_size;

    if (missing_codec) *missing_codec = MOVIE_CODEC_UNKNOWN;
    if (!movie) {
        return false;
    }
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    movie->last_read_bytes = 2048U;
    movie->last_read_time_ms = 1U;
    {
        int prefetch_index;
        for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
            movie->prefetched[prefetch_index].chunk_index = -1;
        }
    }
    movie->decoded_local_frame = -1;
    debug_tracef("open start path=%s", path ? path : "(null)");
    loading_progress_tick(loading_progress, false);

    movie->file = path ? fopen(path, "rb") : NULL;
    movie->encrypted = movie_crypto_keys(path) != NULL;
    if (!movie->file) {
        debug_failf("open failed: %s (%d), errno=%d", storage_read_stream_open_stage(),
            storage_read_stream_open_status(), errno);
        goto fail;
    }
    movie->current_file_pos = 0;
    loading_progress_tick(loading_progress, false);
    if (fread(&movie->header, 1, sizeof(movie->header), movie->file) != sizeof(movie->header)) {
        debug_failf("open failed: header read");
        goto fail;
    }
    movie->current_file_pos = (long) sizeof(movie->header);
    if (fseek(movie->file, 0, SEEK_END) != 0 || (file_size = ftell(movie->file)) < 0 ||
        (uint64_t) file_size > INT32_MAX ||
        !nvp_header_is_valid(&movie->header, (uint32_t) file_size)) {
        debug_failf("open failed: invalid header or file bounds");
        goto fail;
    }
    movie->current_file_pos = -1;
    movie->timing_fps_num = movie->header.fps_num;
    movie->timing_fps_den = movie->header.fps_den;
    player_reduce_fps(&movie->timing_fps_num, &movie->timing_fps_den);
    movie->codec = movie_codec_from_header(&movie->header);
    movie->codec_ops = movie_codec_ops(movie->codec);
    if (!movie->codec_ops) {
        if (movie->codec != MOVIE_CODEC_UNKNOWN) {
            if (missing_codec) *missing_codec = movie->codec;
            debug_tracef("%s codec not included in this build.",
                movie_codec_name(movie->codec));
        } else
            debug_failf("open failed: unsupported version=%u flags=0x%04x",
                (unsigned)movie->header.version, (unsigned)movie->header.flags);
        goto fail;
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
        goto fail;
    }
    if (fseek(movie->file, (long) movie->header.index_offset, SEEK_SET) != 0) {
        debug_failf("open failed: index seek offset=%lu", (unsigned long) movie->header.index_offset);
        goto fail;
    }
    movie->current_file_pos = (long) movie->header.index_offset;
    if (fread(movie->chunk_index, sizeof(ChunkIndexEntry), movie->header.chunk_count, movie->file) != movie->header.chunk_count) {
        debug_failf("open failed: chunk index read count=%lu", (unsigned long) movie->header.chunk_count);
        goto fail;
    }
    movie->current_file_pos += (long) (sizeof(ChunkIndexEntry) * movie->header.chunk_count);
    if (!nvp_index_is_valid(&movie->header, movie->chunk_index)) {
        debug_failf("open failed: invalid chunk index");
        goto fail;
    }
    movie_configure_prefetch(movie);
    debug_tracef("open index loaded chunks=%lu", (unsigned long) movie->header.chunk_count);
    loading_progress_tick(loading_progress, false);
    framebuffer_words = (size_t) movie->header.video_width * movie->header.video_height;
    movie->framebuffer = (uint16_t *) player_calloc_aligned(
        framebuffer_words,
        sizeof(uint16_t),
        PLAYER_CACHE_LINE_SIZE,
        &movie->framebuffer_allocation
    );
    if (!movie->framebuffer) {
        debug_failf("open failed: framebuffer alloc words=%lu", (unsigned long) framebuffer_words);
        goto fail;
    }
    if (movie->codec_ops->global_init && !movie->codec_ops->global_init()) {
        goto fail;
    }
    if (!movie->codec_ops->open(movie)) {
        goto fail;
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
        goto fail;
    }
    if (!decode_to_frame_loading(movie, 0, loading_progress)) {
        debug_tracef("open failed during initial frame decode");
        goto fail;
    }
    debug_tracef("open first frame ok");
    loading_progress_tick(loading_progress, false);
    if (!load_subtitles(movie, movie->file, &movie->header, &movie->subtitles, &movie->subtitle_tracks, &movie->subtitle_track_count, &movie->subtitle_storage, &movie->subtitle_storage_size, loading_progress)) {
        debug_tracef("open subtitles disabled after alloc/read failure");
    }
    loading_progress_tick(loading_progress, false);
    return true;

fail:
    destroy_movie(movie);
    return false;
}

bool key_pressed_edge(t_key key, bool *previous_state)
{
    bool current_state = player_key_pressed(key);
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

static RawPlayerIo *picker_timing_reader;
static MovieFile *picker_timing_file;

void movie_picker_timing_stop(void)
{
    RawPlayerIo *reader = picker_timing_reader;
    picker_timing_reader = NULL;
    picker_timing_file = NULL;
    raw_player_destroy(reader);
}

void movie_picker_timing_tick(MovieFile *files, size_t count, size_t preferred)
{
    MovieHeader header;
    uint8_t prefix[NVE_HEADER_BYTES];

    /* A cold header can require a filesystem-map scan. Discover metadata
     * during the menu entrance, with the same bounded reader used for playback.
     * The picker owns it until completion or cancellation before leaving. */
    if (!picker_timing_reader) {
        if (!files || !count)
            return;
        if (preferred >= count || files[preferred].timing_checked) {
            for (preferred = 0; preferred < count; ++preferred)
                if (!files[preferred].timing_checked)
                    break;
            if (preferred == count)
                return;
        }
        picker_timing_file = &files[preferred];
        picker_timing_reader = raw_player_create(picker_timing_file->path);
        if (!picker_timing_reader) {
            picker_timing_file->timing_checked = true;
            picker_timing_file->metadata_ready_ms = monotonic_clock_now_ms();
            picker_timing_file = NULL;
            return;
        }
    }

    raw_player_service(8U);
    uint32_t file_bytes = raw_player_file_bytes(picker_timing_reader);
    size_t prefix_bytes = file_bytes < sizeof(prefix) ? file_bytes : sizeof(prefix);
    int result = prefix_bytes >= sizeof(header)
        ? raw_player_read(picker_timing_reader, 0, prefix, prefix_bytes, false) : -1;
    if (result == 0)
        return;

    MovieFile *file = picker_timing_file;
    file->timing_checked = true;
    file->metadata_ready_ms = monotonic_clock_now_ms();
    if (result > 0) {
        file->encrypted = !memcmp(prefix, "NVE1", 4);
        if (file->encrypted) {
            NveHeader protected_header;
            if (prefix_bytes == NVE_HEADER_BYTES && nve_header_parse(&protected_header, prefix, file_bytes)) {
                /* This public header was needed for the lock/duration anyway.
                 * Retain it so opening the password dialog needs no file I/O.
                 * Its authentication tag is still checked after key derivation. */
                file->encrypted_header = malloc(sizeof(protected_header));
                if (file->encrypted_header) *file->encrypted_header = protected_header;
                file->duration_ms = protected_header.duration_ms;
                uint32_t frame = file->resume_frame < protected_header.frame_count
                    ? file->resume_frame : protected_header.frame_count - 1U;
                file->resume_ms = (uint32_t)((uint64_t)frame * protected_header.fps_den * 1000U / protected_header.fps_num);
                file->resume_time_known = file->has_resume;
            }
            movie_picker_timing_stop();
            return;
        }
        memcpy(&header, prefix, sizeof(header));
    }
    if (result > 0 && memcmp(header.magic, "NVP1", 4) == 0 &&
        header.fps_num && header.fps_den && header.frame_count) {
        uint32_t frame = file->resume_frame < header.frame_count
            ? file->resume_frame : header.frame_count - 1U;
        file->resume_ms = movie_header_frame_time_ms(&header, frame);
        file->duration_ms = movie_header_frame_time_ms(&header, header.frame_count);
        file->resume_time_known = file->has_resume;
    }
    movie_picker_timing_stop();
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

static bool find_adjacent_movie_path(const char *current_path, char *target_path, size_t target_path_size, int direction)
{
    char directory[MAX_PATH_LEN];
    const char *current_filename;
    MovieFile *files;
    size_t count = 0;
    size_t index;
    bool found = false;
    bool using_picker_cache = false;

    if (!current_path || current_path[0] == '\0' || !target_path || target_path_size == 0 || direction == 0) {
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
            if (direction > 0 && index + 1 < count) {
                strncpy(target_path, files[index + 1].path, target_path_size - 1);
                target_path[target_path_size - 1] = '\0';
                found = true;
            } else if (direction < 0 && index > 0) {
                strncpy(target_path, files[index - 1].path, target_path_size - 1);
                target_path[target_path_size - 1] = '\0';
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

bool find_next_movie_path(const char *current_path, char *next_path, size_t next_path_size)
{
    return find_adjacent_movie_path(current_path, next_path, next_path_size, 1);
}

bool find_previous_movie_path(const char *current_path, char *previous_path, size_t previous_path_size)
{
    return find_adjacent_movie_path(current_path, previous_path, previous_path_size, -1);
}

const SubtitleCue *active_subtitle_cue(Movie *movie, uint32_t now_ms)
{
    uint32_t index;
    uint32_t start_index;
    uint32_t end_index;

    if (!movie || !movie->subtitles) {
        return NULL;
    }
    if (movie->subtitle_lookup_valid && movie->subtitle_lookup_track == movie->selected_subtitle_track &&
        now_ms >= movie->subtitle_lookup_from_ms && now_ms <= movie->subtitle_lookup_until_ms) {
        return movie->subtitle_lookup_cue;
    }
    movie->subtitle_lookup_cue = NULL;
    movie->subtitle_lookup_from_ms = 0;
    movie->subtitle_lookup_until_ms = UINT32_MAX;
    movie->subtitle_lookup_track = movie->selected_subtitle_track;
    movie->subtitle_lookup_valid = true;
    start_index = 0;
    end_index = movie->header.subtitle_count;

    if (movie->subtitle_track_count > 0 && movie->selected_subtitle_track < movie->subtitle_track_count) {
        start_index = movie->subtitle_tracks[movie->selected_subtitle_track].cue_start;
        end_index = start_index + movie->subtitle_tracks[movie->selected_subtitle_track].cue_count;
    }
    for (index = start_index; index < end_index; ++index) {
        const SubtitleCue *cue = &movie->subtitles[index];

        /* File order is significant for overlapping/unsorted ASS cues. A cached
         * answer remains valid until this cue ends or an earlier cue starts. */
        if (now_ms < cue->start_ms) {
            if (cue->start_ms - 1U < movie->subtitle_lookup_until_ms) {
                movie->subtitle_lookup_until_ms = cue->start_ms - 1U;
            }
        } else if (now_ms > cue->end_ms) {
            if (cue->end_ms + 1U > movie->subtitle_lookup_from_ms) {
                movie->subtitle_lookup_from_ms = cue->end_ms + 1U;
            }
        } else {
            if (cue->start_ms > movie->subtitle_lookup_from_ms) {
                movie->subtitle_lookup_from_ms = cue->start_ms;
            }
            if (cue->end_ms < movie->subtitle_lookup_until_ms) {
                movie->subtitle_lookup_until_ms = cue->end_ms;
            }
            movie->subtitle_lookup_cue = cue;
            break;
        }
    }
    return movie->subtitle_lookup_cue;
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

