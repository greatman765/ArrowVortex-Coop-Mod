/* =========================================================================
 * ArrowVortex - PIU Co-op & Biomechanical Kinematic Solver Mod (x64)
 * Modded and maintained by GreatMan
 * Architecture: x86_64 Native (64-bit)
 * ========================================================================= */
static const char* g_mod_info = "ArrowVortex PIU Co-op & Kinematics Mod - Modded by GreatMan (x64)";

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <wchar.h>

/* =========================================================================
 * Dynamic byte buffer
 * ========================================================================= */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf* b, size_t init_cap) {
    b->cap = init_cap ? init_cap : 256;
    b->len = 0;
    b->data = (char*)malloc(b->cap);
}

static void buf_free(Buf* b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void buf_reserve(Buf* b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap * 2;
        while (nc < b->len + extra + 1) nc *= 2;
        b->data = (char*)realloc(b->data, nc);
        b->cap = nc;
    }
}

static void buf_append(Buf* b, const char* s, size_t n) {
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_append_cstr(Buf* b, const char* s) {
    buf_append(b, s, strlen(s));
}

static void buf_push(Buf* b, char c) {
    buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

/* =========================================================================
 * String / SSC helpers
 * ========================================================================= */
static int ci_strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (!ca || !cb) return (int)ca - (int)cb;
        int la = tolower(ca), lb = tolower(cb);
        if (la != lb) return la - lb;
    }
    return 0;
}

static const char* ci_strstr_n(const char* hay, size_t hay_len, const char* needle) {
    size_t nlen = strlen(needle);
    if (!nlen || hay_len < nlen) return NULL;
    for (size_t i = 0; i <= hay_len - nlen; ++i) {
        if (ci_strncmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

static int extract_tag_value(const char* blk, size_t blk_len, const char* tag, char* out, size_t out_sz) {
    const char* p = ci_strstr_n(blk, blk_len, tag);
    if (!p) return 0;
    p += strlen(tag);
    size_t rem = blk_len - (size_t)(p - blk);
    const char* semi = (const char*)memchr(p, ';', rem);
    if (!semi) return 0;
    size_t vlen = (size_t)(semi - p);
    while (vlen && isspace((unsigned char)*p)) { ++p; --vlen; }
    while (vlen && isspace((unsigned char)p[vlen - 1])) { --vlen; }
    if (vlen >= out_sz) vlen = out_sz - 1;
    memcpy(out, p, vlen);
    out[vlen] = '\0';
    return 1;
}

static int find_notes_tag(const char* blk, size_t blk_len, size_t* tag_off, size_t* body_off, size_t* semi_off) {
    const char* p = ci_strstr_n(blk, blk_len, "#NOTES2:");
    size_t tlen = 8;
    if (!p) {
        p = ci_strstr_n(blk, blk_len, "#NOTES:");
        tlen = 7;
    }
    if (!p) return 0;
    size_t bstart = (size_t)(p - blk) + tlen;
    int in_comment = 0;
    for (size_t i = bstart; i < blk_len; ++i) {
        char c = blk[i];
        if (c == '\n') in_comment = 0;
        else if (!in_comment && c == '/' && i + 1 < blk_len && blk[i + 1] == '/') in_comment = 1;
        else if (!in_comment && c == ';') {
            *tag_off = (size_t)(p - blk);
            *body_off = bstart;
            *semi_off = i;
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * Co-op / PumpSanity / StepF2 Note Converter
 * ========================================================================= */
typedef struct {
    char ch;
    int8_t player;
} NoteCell;

static char map_brace_type(char t) {
    switch (t) {
        case '0': return '0';
        case '1': return '1';
        case '2': return '2';
        case '3': return '3';
        case '4': return '4';
        case 'M': case 'm': return 'M';
        case 'L': case 'l': return 'L';
        case 'F': case 'f': return 'F';
        default: return 'F';
    }
}

static int8_t decode_pchar(char pc, int max_players, int allow_alpha_players) {
    if (max_players <= 1) return -1;
    if (pc >= '1' && pc <= '9') {
        int p = pc - '1';
        return (p < max_players) ? (int8_t)p : 0;
    }
    if (allow_alpha_players && pc >= 'A' && pc <= 'Z') {
        int p = pc - 'A';
        return (p < max_players) ? (int8_t)p : 0;
    }
    return -1;
}

static int tokenize_row(const char* line, size_t len, int default_player,
                        int max_players, int allow_alpha_players,
                        NoteCell* out, int max_out) {
    int count = 0;
    size_t i = 0;
    while (i < len && count < max_out) {
        char ch = line[i];
        if (ch == '{') {
            size_t start = i + 1;
            int depth = 1;
            size_t j = start;
            while (j < len && depth > 0) {
                if (line[j] == '{') ++depth;
                else if (line[j] == '}') --depth;
                ++j;
            }
            size_t inner_end = (depth == 0) ? (j - 1) : j;
            const char* inner = line + start;
            size_t ilen = (inner_end > start) ? (inner_end - start) : 0;
            i = j;

            while (ilen > 0 && inner[0] == '{') {
                ++inner;
                --ilen;
            }

            char ntype = (ilen >= 1) ? inner[0] : '0';
            int has_pipe = (memchr(inner, '|', ilen) != NULL);
            char pchar = (!has_pipe && ilen >= 2) ? inner[1] : '0';
            char av_ch = map_brace_type(ntype);
            int8_t p_idx = decode_pchar(pchar, max_players, allow_alpha_players);
            if (p_idx < 0 && av_ch != '3') p_idx = (int8_t)default_player;
            out[count++] = (NoteCell){av_ch, p_idx};
        } else if (ch == '[') {
            size_t j = i + 1;
            while (j < len && line[j] != ']') ++j;
            i = (j < len) ? (j + 1) : len;
        } else if (ch == 'X' || ch == 'Y' || ch == 'Z') {
            int p = ch - 'X';
            out[count++] = (NoteCell){'1', (int8_t)((p < max_players) ? p : 0)};
            ++i;
        } else if (ch == 'x' || ch == 'y' || ch == 'z') {
            int p = ch - 'x';
            out[count++] = (NoteCell){'2', (int8_t)((p < max_players) ? p : 0)};
            ++i;
        } else if (ch == '0' || ch == '1' || ch == '2' || ch == '3' ||
                   ch == '4' || ch == 'M' || ch == 'L' || ch == 'F') {
            int8_t p_idx = (ch == '3' && default_player < 0) ? -1 : (int8_t)default_player;
            out[count++] = (NoteCell){ch, p_idx};
            ++i;
        } else if (isspace((unsigned char)ch)) {
            ++i;
        } else {
            out[count++] = (NoteCell){'0', (int8_t)default_player};
            ++i;
        }
    }
    return count;
}

typedef struct {
    char* lines_buf;
    int num_rows;
    int num_cols;
} MeasureGrid;

typedef struct {
    MeasureGrid* meas;
    int count;
    int cap;
} PlayerTrack;

static void track_ensure_meas(PlayerTrack* tr, int m_idx, int num_rows, int num_cols) {
    if (m_idx >= tr->cap) {
        int nc = tr->cap ? tr->cap * 2 : 64;
        while (nc <= m_idx) nc *= 2;
        tr->meas = (MeasureGrid*)realloc(tr->meas, nc * sizeof(MeasureGrid));
        for (int k = tr->cap; k < nc; ++k) {
            tr->meas[k].lines_buf = NULL;
            tr->meas[k].num_rows = 0;
            tr->meas[k].num_cols = num_cols;
        }
        tr->cap = nc;
    }
    while (tr->count <= m_idx) {
        int idx = tr->count++;
        int r = (idx == m_idx) ? (num_rows > 0 ? num_rows : 4) : 4;
        tr->meas[idx].num_rows = r;
        tr->meas[idx].num_cols = num_cols;
        tr->meas[idx].lines_buf = (char*)malloc((size_t)r * (size_t)num_cols);
        memset(tr->meas[idx].lines_buf, '0', (size_t)r * (size_t)num_cols);
    }
    if (num_rows > tr->meas[m_idx].num_rows) {
        int old_r = tr->meas[m_idx].num_rows;
        tr->meas[m_idx].lines_buf = (char*)realloc(tr->meas[m_idx].lines_buf, (size_t)num_rows * (size_t)num_cols);
        memset(tr->meas[m_idx].lines_buf + (size_t)old_r * (size_t)num_cols, '0', (size_t)(num_rows - old_r) * (size_t)num_cols);
        tr->meas[m_idx].num_rows = num_rows;
    }
}

typedef struct {
    const char* ptr;
    size_t len;
} Slice;

static size_t trim_note_line(const char* s, size_t len, const char** out_start) {
    for (size_t i = 0; i + 1 < len; ++i) {
        if (s[i] == '/' && s[i + 1] == '/') {
            len = i;
            break;
        }
    }
    while (len && isspace((unsigned char)*s)) { ++s; --len; }
    while (len && isspace((unsigned char)s[len - 1])) { --len; }
    *out_start = s;
    return len;
}

static int notes_need_conversion(const char* notes, size_t len, const char* stepstype, int* out_cols) {
    if (memchr(notes, '{', len) != NULL) return 1;
    int max_rows_in_m = 0;
    int cur_rows = 0;
    int first_cols = 0;
    int has_xyz = 0;

    const char* p = notes;
    const char* end = notes + len;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char* st;
        size_t slen = trim_note_line(p, llen, &st);
        if (slen > 0) {
            if (slen == 1 && (st[0] == ',' || st[0] == '&')) {
                if (cur_rows > max_rows_in_m) max_rows_in_m = cur_rows;
                cur_rows = 0;
            } else {
                const char* cpos = (const char*)memchr(st, ',', slen);
                if (cpos) {
                    if (cur_rows > max_rows_in_m) max_rows_in_m = cur_rows;
                    cur_rows = 0;
                } else {
                    ++cur_rows;
                    int in_ks = 0;
                    int cols = 0;
                    for (size_t k = 0; k < slen; ++k) {
                        char c = st[k];
                        if (c == '[') in_ks = 1;
                        else if (c == ']') in_ks = 0;
                        else if (!in_ks && !isspace((unsigned char)c)) {
                            ++cols;
                            if (c == 'X' || c == 'Y' || c == 'Z' || c == 'x' || c == 'y' || c == 'z')
                                has_xyz = 1;
                        }
                    }
                    if (!first_cols && cols > 0) first_cols = cols;
                }
            }
        }
        p = nl ? (nl + 1) : end;
    }
    if (cur_rows > max_rows_in_m) max_rows_in_m = cur_rows;
    *out_cols = first_cols;
    if (has_xyz || max_rows_in_m > 192) return 1;
    if (ci_strncmp(stepstype, "pump-double-p", 14) == 0 && first_cols == 5) return 1;
    return 0;
}

static void count_pipe_modifiers(const char* notes, size_t len, int* out_v, int* out_h, int* out_s) {
    *out_v = *out_h = *out_s = 0;
    for (size_t i = 0; i < len; ++i) {
        if (notes[i] != '{') continue;
        size_t j = i + 1;
        while (j < len && notes[j] != '}') ++j;
        if (j >= len) break;
        const char* inner = notes + i + 1;
        size_t ilen = j - i - 1;
        const char* pipe = (const char*)memchr(inner, '|', ilen);
        if (pipe && pipe + 1 < inner + ilen) {
            char mod = pipe[1];
            if (mod == 'v' || mod == 'V') ++(*out_v);
            else if (mod == 'h' || mod == 'H') ++(*out_h);
            else if (mod == 's' || mod == 'S') ++(*out_s);
        }
        i = j;
    }
}

static void convert_notes_body(const char* notes, size_t len, int declared_players,
                               int max_players_allowed, int allow_alpha_players,
                               Buf* out_notes, int* out_num_cols, int* out_num_players) {
    Slice raw_parts[16];
    int num_parts = 0;
    const char* cur = notes;
    const char* end = notes + len;
    while (cur <= end && num_parts < 16) {
        const char* amp = (const char*)memchr(cur, '&', (size_t)(end - cur));
        raw_parts[num_parts].ptr = cur;
        raw_parts[num_parts].len = amp ? (size_t)(amp - cur) : (size_t)(end - cur);
        ++num_parts;
        if (!amp) break;
        cur = amp + 1;
    }
    if (num_parts > max_players_allowed) max_players_allowed = num_parts;
    if (max_players_allowed > 5) max_players_allowed = 5;
    int is_single_part = (num_parts == 1);

    int num_cols = 0;
    NoteCell tmp_cells[64];
    for (int pi = 0; pi < num_parts && !num_cols; ++pi) {
        const char* p = raw_parts[pi].ptr;
        const char* pe = p + raw_parts[pi].len;
        while (p < pe && !num_cols) {
            const char* nl = (const char*)memchr(p, '\n', (size_t)(pe - p));
            size_t llen = nl ? (size_t)(nl - p) : (size_t)(pe - p);
            const char* st;
            size_t slen = trim_note_line(p, llen, &st);
            if (slen > 0 && !(slen == 1 && st[0] == ',')) {
                while (slen && *st == ',') { ++st; --slen; }
                while (slen && st[slen - 1] == ',') { --slen; }
                if (slen > 0) {
                    int tc = tokenize_row(st, slen, 0, max_players_allowed, allow_alpha_players, tmp_cells, 64);
                    if (tc > 0) num_cols = tc;
                }
            }
            p = nl ? (nl + 1) : pe;
        }
    }

    if (num_cols <= 0 || num_cols > 32) {
        buf_append(out_notes, notes, len);
        *out_num_cols = num_cols;
        *out_num_players = declared_players > 0 ? declared_players : 1;
        return;
    }

    PlayerTrack tracks[5];
    memset(tracks, 0, sizeof(tracks));
    int max_player_seen = (declared_players > 1) ? (declared_players - 1) : 0;
    if (max_player_seen > 4) max_player_seen = 4;
    int max_meas_count = 0;

    for (int pi = 0; pi < num_parts; ++pi) {
        const char* p = raw_parts[pi].ptr;
        const char* pe = p + raw_parts[pi].len;
        int m_idx = 0;

        int8_t hold_stack[32][16];
        int hold_top[32];
        memset(hold_top, 0, sizeof(hold_top));

        const char* mstart = p;
        while (mstart <= pe) {
            const char* comma = (const char*)memchr(mstart, ',', (size_t)(pe - mstart));
            const char* mend = comma ? comma : pe;

            Slice rows[1024];
            int rcount = 0;
            const char* lp = mstart;
            while (lp < mend && rcount < 1024) {
                const char* nl = (const char*)memchr(lp, '\n', (size_t)(mend - lp));
                size_t llen = nl ? (size_t)(nl - lp) : (size_t)(mend - lp);
                const char* st;
                size_t slen = trim_note_line(lp, llen, &st);
                if (slen > 0) {
                    rows[rcount].ptr = st;
                    rows[rcount].len = slen;
                    ++rcount;
                }
                lp = nl ? (nl + 1) : mend;
            }

            int out_rcount = (rcount > 192) ? 192 : (rcount > 0 ? rcount : 4);
            for (int tp = 0; tp < 5; ++tp) {
                if (tracks[tp].count > 0) {
                    track_ensure_meas(&tracks[tp], m_idx, out_rcount, num_cols);
                }
            }

            for (int r = 0; r < rcount; ++r) {
                int def_p = is_single_part ? -1 : pi;
                int tc = tokenize_row(rows[r].ptr, rows[r].len, def_p,
                                      max_players_allowed, allow_alpha_players,
                                      tmp_cells, 64);
                int dst_r = (rcount > 192) ? ((r * 192) / rcount) : r;
                if (dst_r >= out_rcount) dst_r = out_rcount - 1;

                int cols_lim = (tc < num_cols) ? tc : num_cols;
                for (int c = 0; c < cols_lim; ++c) {
                    char av_ch = tmp_cells[c].ch;
                    int8_t p_idx = tmp_cells[c].player;
                    if (av_ch == '0') continue;

                    int target_p = 0;
                    if (av_ch == '2' || av_ch == '4') {
                        target_p = (p_idx >= 0) ? p_idx : (is_single_part ? 0 : pi);
                        if (target_p > 4) target_p = 4;
                        if (hold_top[c] < 16) hold_stack[c][hold_top[c]++] = (int8_t)target_p;
                    } else if (av_ch == '3') {
                        if (p_idx >= 0) {
                            target_p = p_idx;
                            if (target_p > 4) target_p = 4;
                            for (int h = 0; h < hold_top[c]; ++h) {
                                if (hold_stack[c][h] == target_p) {
                                    for (int k = h; k + 1 < hold_top[c]; ++k)
                                        hold_stack[c][k] = hold_stack[c][k + 1];
                                    --hold_top[c];
                                    break;
                                }
                            }
                        } else if (hold_top[c] > 0) {
                            target_p = hold_stack[c][0];
                            for (int k = 0; k + 1 < hold_top[c]; ++k)
                                hold_stack[c][k] = hold_stack[c][k + 1];
                            --hold_top[c];
                        } else {
                            target_p = is_single_part ? 0 : pi;
                        }
                    } else {
                        target_p = (p_idx >= 0) ? p_idx : (is_single_part ? 0 : pi);
                    }

                    if (target_p < 0) target_p = 0;
                    if (target_p > 4) target_p = 4;
                    if (target_p > max_player_seen) max_player_seen = target_p;

                    track_ensure_meas(&tracks[target_p], m_idx, out_rcount, num_cols);
                    tracks[target_p].meas[m_idx].lines_buf[dst_r * num_cols + c] = av_ch;
                }
            }

            ++m_idx;
            if (!comma) break;
            mstart = comma + 1;
        }
        if (m_idx > max_meas_count) max_meas_count = m_idx;
    }

    int num_players = max_player_seen + 1;
    if (declared_players > num_players) num_players = declared_players;
    if (num_players < 1) num_players = 1;
    if (num_players > 5) num_players = 5;

    char empty_row[33];
    memset(empty_row, '0', (size_t)num_cols);
    empty_row[num_cols] = '\0';

    for (int p = 0; p < num_players; ++p) {
        if (p > 0) buf_append_cstr(out_notes, "\n&\n");
        for (int m = 0; m < max_meas_count; ++m) {
            if (m > 0) buf_append_cstr(out_notes, "\n,\n");
            if (m < tracks[p].count && tracks[p].meas[m].num_rows > 0) {
                MeasureGrid* mg = &tracks[p].meas[m];
                for (int r = 0; r < mg->num_rows; ++r) {
                    if (r > 0) buf_push(out_notes, '\n');
                    buf_append(out_notes, mg->lines_buf + (size_t)r * (size_t)num_cols, (size_t)num_cols);
                }
            } else {
                for (int r = 0; r < 4; ++r) {
                    if (r > 0) buf_push(out_notes, '\n');
                    buf_append(out_notes, empty_row, (size_t)num_cols);
                }
            }
        }
    }
    buf_push(out_notes, '\n');

    for (int p = 0; p < 5; ++p) {
        for (int m = 0; m < tracks[p].count; ++m) free(tracks[p].meas[m].lines_buf);
        free(tracks[p].meas);
    }

    *out_num_cols = num_cols;
    *out_num_players = num_players;
}

static int convert_ssc_to_av(const char* src, size_t src_len, Buf* out) {
    if (src_len >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        src += 3;
        src_len -= 3;
    }

    const char* first_nd = ci_strstr_n(src, src_len, "#NOTEDATA:");
    if (!first_nd) return 0;

    buf_init(out, src_len + 4096);
    buf_append(out, src, (size_t)(first_nd - src));

    int any_modified = 0;
    const char* cur = first_nd;
    const char* end = src + src_len;

    while (cur < end) {
        const char* after_nd = cur + 10;
        const char* next_nd = ci_strstr_n(after_nd, (size_t)(end - after_nd), "#NOTEDATA:");
        const char* blk_end = next_nd ? next_nd : end;
        size_t blk_len = (size_t)(blk_end - cur);

        size_t tag_off = 0, body_off = 0, semi_off = 0;
        if (!find_notes_tag(cur, blk_len, &tag_off, &body_off, &semi_off)) {
            buf_append(out, cur, blk_len);
            cur = blk_end;
            continue;
        }

        char stepstype[64] = {0};
        char players_str[32] = {0};
        extract_tag_value(cur, tag_off, "#STEPSTYPE:", stepstype, sizeof(stepstype));
        int has_players_tag = extract_tag_value(cur, tag_off, "#PLAYERS:", players_str, sizeof(players_str));
        int declared_players = (has_players_tag && players_str[0]) ? atoi(players_str) : 1;
        if (declared_players < 1) declared_players = 1;
        if (declared_players > 5) declared_players = 5;

        int detected_cols = 0;
        if (!notes_need_conversion(cur + body_off, semi_off - body_off, stepstype, &detected_cols)) {
            buf_append(out, cur, blk_len);
            cur = blk_end;
            continue;
        }

        int is_routine_style = (ci_strncmp(stepstype, "pump-routine", 13) == 0);
        int is_coop_style = (declared_players >= 2) ||
                            (ci_strncmp(stepstype, "pump-double-p", 14) == 0) ||
                            is_routine_style;
        int max_players_allowed = 1;
        if (has_players_tag) {
            max_players_allowed = declared_players;
        } else if (is_routine_style || ci_strncmp(stepstype, "pump-double-p", 14) == 0) {
            max_players_allowed = 5;
        }
        int allow_alpha = (max_players_allowed >= 2) && is_coop_style;

        int mod_v = 0, mod_h = 0, mod_s = 0;
        count_pipe_modifiers(cur + body_off, semi_off - body_off, &mod_v, &mod_h, &mod_s);

        Buf conv_notes;
        buf_init(&conv_notes, (semi_off - body_off) + 1024);
        int num_cols = 0, num_players = 1;
        convert_notes_body(cur + body_off, semi_off - body_off, declared_players,
                           max_players_allowed, allow_alpha, &conv_notes,
                           &num_cols, &num_players);

        const char* new_style = NULL;
        if (num_cols == 5 && ci_strncmp(stepstype, "pump-double-p", 14) == 0) {
            new_style = "pump-single-p";
        } else if (num_cols == 10 && num_players > 1) {
            if (ci_strncmp(stepstype, "pump-double", 12) == 0 ||
                ci_strncmp(stepstype, "pump-couple", 12) == 0 ||
                stepstype[0] == '\0') {
                new_style = "pump-double-p";
            } else if (ci_strncmp(stepstype, "pump-routine", 13) == 0 && num_players > 2) {
                new_style = "pump-double-p";
            }
        }

        size_t out_hdr_start = out->len;
        if (new_style) {
            const char* st_pos = ci_strstr_n(cur, tag_off, "#STEPSTYPE:");
            if (st_pos) {
                const char* st_semi = (const char*)memchr(st_pos, ';', tag_off - (size_t)(st_pos - cur));
                if (st_semi) {
                    buf_append(out, cur, (size_t)(st_pos - cur));
                    buf_append_cstr(out, "#STEPSTYPE:");
                    buf_append_cstr(out, new_style);
                    buf_push(out, ';');
                    buf_append(out, st_semi + 1, tag_off - (size_t)(st_semi + 1 - cur));
                } else {
                    buf_append(out, cur, tag_off);
                }
            } else {
                buf_append(out, cur, tag_off);
            }
        } else {
            buf_append(out, cur, tag_off);
        }

        /* Annotate #DESCRIPTION with per-note visibility modifier counts (V=Vanish H=Hidden S=Sudden) */
        if (mod_v || mod_h || mod_s) {
            size_t hdr_written = out->len - out_hdr_start;
            const char* dp = ci_strstr_n(out->data + out_hdr_start, hdr_written, "#DESCRIPTION:");
            if (dp) {
                const char* ds = (const char*)memchr(dp, ';', hdr_written - (size_t)(dp - (out->data + out_hdr_start)));
                if (ds) {
                    char suffix[128];
                    int sp = 0;
                    sp += snprintf(suffix + sp, sizeof(suffix) - sp, " [GFX:");
                    if (mod_v) sp += snprintf(suffix + sp, sizeof(suffix) - sp, " V%d", mod_v);
                    if (mod_h) sp += snprintf(suffix + sp, sizeof(suffix) - sp, " H%d", mod_h);
                    if (mod_s) sp += snprintf(suffix + sp, sizeof(suffix) - sp, " S%d", mod_s);
                    snprintf(suffix + sp, sizeof(suffix) - sp, "]");
                    size_t suf_len = strlen(suffix);
                    size_t ins = (size_t)(ds - out->data);
                    buf_reserve(out, suf_len);
                    memmove(out->data + ins + suf_len, out->data + ins, out->len - ins);
                    memcpy(out->data + ins, suffix, suf_len);
                    out->len += suf_len;
                    out->data[out->len] = '\0';
                }
            }
        }

        buf_append(out, cur + tag_off, body_off - tag_off);
        buf_push(out, '\n');
        buf_append(out, conv_notes.data, conv_notes.len);
        buf_append(out, cur + semi_off, blk_len - semi_off);
        buf_free(&conv_notes);
        any_modified = 1;
        cur = blk_end;
    }

    if (!any_modified) {
        buf_free(out);
        return 0;
    }
    return 1;
}

/* =========================================================================
 * Temp-file registry + MSVCP140 _Fiopen / UCRT fopen / fclose hooks
 * ========================================================================= */
typedef FILE* (*fiopen_w_fn)(const wchar_t*, int, int);
typedef FILE* (*fiopen_a_fn)(const char*, int, int);
typedef FILE* (*fopen_fn)(const char*, const char*);
typedef int   (*fopen_s_fn)(FILE**, const char*, const char*);
typedef int   (*fclose_fn)(FILE*);

static fiopen_w_fn orig_fiopen_w = NULL;
static fiopen_a_fn orig_fiopen_a = NULL;
static fopen_fn    orig_fopen    = NULL;
static fopen_s_fn  orig_fopen_s  = NULL;
static fclose_fn   orig_fclose   = NULL;

typedef struct {
    FILE* fp;
    wchar_t path[MAX_PATH];
} TempEntry;

static TempEntry g_temps[64];
static CRITICAL_SECTION g_temp_cs;

static void register_temp(FILE* fp, const wchar_t* path) {
    if (!fp) return;
    EnterCriticalSection(&g_temp_cs);
    for (int i = 0; i < 64; ++i) {
        if (!g_temps[i].fp) {
            g_temps[i].fp = fp;
            wcsncpy(g_temps[i].path, path, MAX_PATH - 1);
            g_temps[i].path[MAX_PATH - 1] = L'\0';
            break;
        }
    }
    LeaveCriticalSection(&g_temp_cs);
}

static int ends_with_ssc_w(const wchar_t* p) {
    if (!p) return 0;
    size_t len = wcslen(p);
    return (len >= 4 && _wcsicmp(p + len - 4, L".ssc") == 0);
}

static int make_converted_temp_ssc(const wchar_t* orig_path, wchar_t* out_tfile) {
    if (!orig_path || !ends_with_ssc_w(orig_path)) return 0;

    HANDLE hf = CreateFileW(orig_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hf, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(hf);
        return 0;
    }

    size_t fsz = (size_t)sz.QuadPart;
    char* raw = (char*)malloc(fsz + 1);
    if (!raw) {
        CloseHandle(hf);
        return 0;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(hf, raw, (DWORD)fsz, &got, NULL);
    CloseHandle(hf);
    if (!ok || got == 0) {
        free(raw);
        return 0;
    }
    raw[got] = '\0';

    Buf out;
    int modified = convert_ssc_to_av(raw, got, &out);
    free(raw);
    if (!modified) return 0;

    wchar_t tdir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tdir) || !GetTempFileNameW(tdir, L"avc", 0, out_tfile)) {
        buf_free(&out);
        return 0;
    }

    HANDLE ht = CreateFileW(out_tfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (ht == INVALID_HANDLE_VALUE) {
        buf_free(&out);
        DeleteFileW(out_tfile);
        return 0;
    }
    DWORD written = 0;
    WriteFile(ht, out.data, (DWORD)out.len, &written, NULL);
    CloseHandle(ht);
    buf_free(&out);
    return 1;
}

static FILE* hook_fiopen_w(const wchar_t* path, int mode, int prot) {
    /* std::ios_base::in = 1, out = 2 */
    if ((mode & 0x03) == 0x01) {
        wchar_t tfile[MAX_PATH];
        if (make_converted_temp_ssc(path, tfile)) {
            FILE* fp = orig_fiopen_w(tfile, mode, prot);
            if (fp) { register_temp(fp, tfile); return fp; }
            DeleteFileW(tfile);
        }
    }
    return orig_fiopen_w(path, mode, prot);
}

static FILE* hook_fiopen_a(const char* path, int mode, int prot) {
    if (path && (mode & 0x03) == 0x01) {
        size_t len = strlen(path);
        if (len >= 4 && _stricmp(path + len - 4, ".ssc") == 0) {
            wchar_t wpath[MAX_PATH * 2], tfile[MAX_PATH];
            if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2) > 0 ||
                MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2) > 0) {
                if (make_converted_temp_ssc(wpath, tfile)) {
                    FILE* fp = orig_fiopen_w ? orig_fiopen_w(tfile, mode, prot) : NULL;
                    if (fp) { register_temp(fp, tfile); return fp; }
                    DeleteFileW(tfile);
                }
            }
        }
    }
    return orig_fiopen_a(path, mode, prot);
}

static FILE* hook_fopen(const char* path, const char* mode) {
    if (path && mode && mode[0] == 'r') {
        size_t len = strlen(path);
        if (len >= 4 && _stricmp(path + len - 4, ".ssc") == 0) {
            wchar_t wpath[MAX_PATH * 2], tfile[MAX_PATH];
            if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2) > 0 ||
                MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2) > 0) {
                if (make_converted_temp_ssc(wpath, tfile)) {
                    char apath[MAX_PATH * 2];
                    if (WideCharToMultiByte(CP_ACP, 0, tfile, -1, apath, sizeof(apath), NULL, NULL) > 0) {
                        FILE* fp = orig_fopen(apath, mode);
                        if (fp) { register_temp(fp, tfile); return fp; }
                    }
                    DeleteFileW(tfile);
                }
            }
        }
    }
    return orig_fopen(path, mode);
}

static int hook_fopen_s(FILE** pfp, const char* path, const char* mode) {
    if (path && mode && mode[0] == 'r') {
        size_t len = strlen(path);
        if (len >= 4 && _stricmp(path + len - 4, ".ssc") == 0) {
            wchar_t wpath[MAX_PATH * 2], tfile[MAX_PATH];
            if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2) > 0 ||
                MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2) > 0) {
                if (make_converted_temp_ssc(wpath, tfile)) {
                    char apath[MAX_PATH * 2];
                    if (WideCharToMultiByte(CP_ACP, 0, tfile, -1, apath, sizeof(apath), NULL, NULL) > 0) {
                        int err = orig_fopen_s(pfp, apath, mode);
                        if (err == 0 && pfp && *pfp) { register_temp(*pfp, tfile); return 0; }
                    }
                    DeleteFileW(tfile);
                }
            }
        }
    }
    return orig_fopen_s(pfp, path, mode);
}

static int hook_fclose(FILE* fp) {
    wchar_t del_path[MAX_PATH] = {0};
    if (fp) {
        EnterCriticalSection(&g_temp_cs);
        for (int i = 0; i < 64; ++i) {
            if (g_temps[i].fp == fp) {
                wcsncpy(del_path, g_temps[i].path, MAX_PATH - 1);
                g_temps[i].fp = NULL;
                g_temps[i].path[0] = L'\0';
                break;
            }
        }
        LeaveCriticalSection(&g_temp_cs);
    }
    int rc = orig_fclose(fp);
    if (del_path[0]) DeleteFileW(del_path);
    return rc;
}

/* =========================================================================
 * Improved Co-op Dancing Bot hooks (ArrowVortex v1.1.0 x64)
 * ========================================================================= */
static uintptr_t g_base = 0;

typedef void* (*GetDrawPosFn)(void* dlg, int* out_xy, uint64_t col_row_packed);
typedef void  (*BatchDrawFn)(void* spr, void* batch, float x, float y, float rot, float scale, uint32_t color);

static double call_get_cursor_time(void* gView) {
    void** vt = *(void***)gView;
    double (*fn)(void*) = (double (*)(void*))vt[0x168 / 8];
    return fn(gView);
}

static const char* call_notes_begin(void* gNotes) {
    void** vt = *(void***)gNotes;
    const char* (*fn)(void*) = (const char* (*)(void*))vt[0xd0 / 8];
    return fn(gNotes);
}

static const char* call_notes_end(void* gNotes) {
    void** vt = *(void***)gNotes;
    const char* (*fn)(void*) = (const char* (*)(void*))vt[0xd8 / 8];
    return fn(gNotes);
}

static void* call_style_get(void* gStyle) {
    void** vt = *(void***)gStyle;
    void* (*fn)(void*) = (void* (*)(void*))vt[0x40 / 8];
    return fn(gStyle);
}

static int call_style_num_players(void* gStyle) {
    void** vt = *(void***)gStyle;
    int (*fn)(void*) = (int (*)(void*))vt[0x38 / 8];
    return fn(gStyle);
}

static uint32_t pack_rgba(float r, float g, float b, float a) {
    uint32_t ur = (uint32_t)(r * 255.0f + 0.5f); if (ur > 255) ur = 255;
    uint32_t ug = (uint32_t)(g * 255.0f + 0.5f); if (ug > 255) ug = 255;
    uint32_t ub = (uint32_t)(b * 255.0f + 0.5f); if (ub > 255) ub = 255;
    uint32_t ua = (uint32_t)(a * 255.0f + 0.5f); if (ua > 255) ua = 255;
    return (ua << 24) | (ub << 16) | (ug << 8) | ur;
}

/* ExpandedNote in v1.1.0 x64 (stride = 0x28):
 * +0x04: int col
 * +0x10: double time
 * +0x18: double endtime
 * +0x20: uint32_t flags0 (bit 0 = isMine, bit 3 = isFake)
 * +0x24: uint32_t flags1 (& 0xffffff = player)
 */

static uint8_t s_coop_feet[5][16384];
static const char* s_last_nbeg = NULL;
static int s_last_ncount = 0;

static void update_coop_solver_if_needed(const char* nbeg, int ncount, void* style) {
    if (nbeg == s_last_nbeg && ncount == s_last_ncount) return;
    s_last_nbeg = nbeg;
    s_last_ncount = ncount;

    if (!nbeg || ncount <= 0 || !style) return;
    int max_notes = (ncount > 16384) ? 16384 : ncount;

    const int* pad_cols  = *(const int**)((char*)style + 0xa8);
    const int* init_feet = *(const int**)((char*)style + 0xc0);

    for (int p = 0; p < 5; ++p) {
        int l_col = init_feet[p * 2 + 0];
        int r_col = init_feet[p * 2 + 1];
        double l_end = -1.0;
        double r_end = -1.0;
        int last_foot = 1;

        for (int n = 0; n < max_notes; ++n) {
            const char* np = nbeg + (size_t)n * 0x28;
            uint32_t f0 = *(const uint32_t*)(np + 0x20);
            int player = (int)(*(const uint32_t*)(np + 0x24) & 0xffffff);
            if ((f0 & 9) || player != p) continue;

            int col = *(const int*)(np + 0x04);
            double t = *(const double*)(np + 0x10);
            double endt = *(const double*)(np + 0x18);
            if (endt < t) endt = t;

            int target_x = pad_cols[col * 2 + 0];
            int target_y = pad_cols[col * 2 + 1];

            int l_locked = (t <= l_end);
            int r_locked = (t <= r_end);
            int foot = 0;

            if (l_locked && !r_locked) {
                foot = 1;
            } else if (r_locked && !l_locked) {
                foot = 0;
            } else {
                int lx = pad_cols[l_col * 2 + 0];
                int ly = pad_cols[l_col * 2 + 1];
                int rx = pad_cols[r_col * 2 + 0];
                int ry = pad_cols[r_col * 2 + 1];

                float dist_l = (float)((lx - target_x) * (lx - target_x) + (ly - target_y) * (ly - target_y));
                float dist_r = (float)((rx - target_x) * (rx - target_x) + (ry - target_y) * (ry - target_y));

                float score_l = dist_l;
                float score_r = dist_r;

                /* Alternation bonus */
                if (last_foot == 1) score_l -= 1.5f;
                else score_r -= 1.5f;

                /* Natural stance preference */
                if (target_x < rx) score_l -= 1.0f;
                if (target_x > lx) score_r -= 1.0f;

                /* Crossover penalty */
                if (target_x > rx + 1) score_l += 3.5f;
                if (target_x < lx - 1) score_r += 3.5f;

                foot = (score_l <= score_r) ? 0 : 1;
            }

            s_coop_feet[p][n] = (uint8_t)foot;
            if (foot == 0) {
                l_col = col;
                l_end = endt;
                last_foot = 0;
            } else {
                r_col = col;
                r_end = endt;
                last_foot = 1;
            }
        }
    }
}

static void Hook_GetFeetPositions(void* dlg, float* out, int pn) {
    void* gStyle = *(void**)(g_base + 0x146daf8);
    void* gNotes = *(void**)(g_base + 0x146daf0);
    void* gView  = *(void**)(g_base + 0x146da48);
    GetDrawPosFn get_draw_pos = (GetDrawPosFn)(g_base + 0x32a030);

    void* style = call_style_get(gStyle);
    const int* pad_cols  = *(const int**)((char*)style + 0xa8);
    const int* init_feet = *(const int**)((char*)style + 0xc0);

    double time = call_get_cursor_time(gView);
    const char* nbeg = call_notes_begin(gNotes);
    const char* nend = call_notes_end(gNotes);
    int ncount = (int)((nend - nbeg) / 0x28);

    update_coop_solver_if_needed(nbeg, ncount, style);

    int prevNote[2] = {-1, -1};
    int nextNote[2] = {-1, -1};

    for (int n = 0; n < ncount; ++n) {
        const char* np = nbeg + (size_t)n * 0x28;
        double nt = *(const double*)(np + 0x10);
        if (nt > time) break;
        uint32_t f0 = *(const uint32_t*)(np + 0x20);
        int player = (int)(*(const uint32_t*)(np + 0x24) & 0xffffff);
        if ((f0 & 9) || player != pn) continue;
        int foot = (int)s_coop_feet[pn][n];
        prevNote[foot] = n;
    }

    for (int n = ncount - 1; n >= 0; --n) {
        const char* np = nbeg + (size_t)n * 0x28;
        double nt = *(const double*)(np + 0x10);
        if (nt <= time) break;
        uint32_t f0 = *(const uint32_t*)(np + 0x20);
        int player = (int)(*(const uint32_t*)(np + 0x24) & 0xffffff);
        if ((f0 & 9) || player != pn) continue;
        int foot = (int)s_coop_feet[pn][n];
        nextNote[foot] = n;
    }

    int init_c[2] = { init_feet[pn * 2], init_feet[pn * 2 + 1] };

    for (int f = 0; f < 2; ++f) {
        int gx, gy, ex, ey;
        double curTime = 0.0, holdEnd = 0.0, endTime = 0.0;

        if (prevNote[f] >= 0) {
            const char* np = nbeg + (size_t)prevNote[f] * 0x28;
            int col = *(const int*)(np + 0x04);
            gx = pad_cols[col * 2];
            gy = pad_cols[col * 2 + 1];
            curTime = *(const double*)(np + 0x10);
            holdEnd = *(const double*)(np + 0x18);
            if (holdEnd < curTime) holdEnd = curTime;
        } else {
            gx = pad_cols[init_c[f] * 2];
            gy = pad_cols[init_c[f] * 2 + 1];
        }

        ex = gx;
        ey = gy;
        endTime = holdEnd;
        if (nextNote[f] >= 0) {
            const char* np = nbeg + (size_t)nextNote[f] * 0x28;
            int col = *(const int*)(np + 0x04);
            ex = pad_cols[col * 2];
            ey = pad_cols[col * 2 + 1];
            endTime = *(const double*)(np + 0x10);
        }

        int p1[2], p2[2];
        uint64_t pk1 = (uint64_t)(uint32_t)gx | ((uint64_t)(uint32_t)gy << 32);
        uint64_t pk2 = (uint64_t)(uint32_t)ex | ((uint64_t)(uint32_t)ey << 32);
        get_draw_pos(dlg, p1, pk1);
        get_draw_pos(dlg, p2, pk2);
        float fx = (float)p1[0], fy = (float)p1[1];

        if (time > holdEnd && endTime > holdEnd) {
            double startMove = endTime - 0.35;
            if (startMove < holdEnd) startMove = holdEnd;
            double d = (time - startMove) / (endTime - startMove);
            if (d < 0.0) d = 0.0;
            if (d > 1.0) d = 1.0;
            float s = (float)(d * d * (3.0 - 2.0 * d));
            fx += ((float)p2[0] - fx) * s;
            fy += ((float)p2[1] - fy) * s;
        }

        float scale;
        if (prevNote[f] >= 0 && time >= curTime && time <= holdEnd) {
            scale = 0.84f;
        } else {
            double dt1 = fabs(curTime - time);
            double dt2 = (nextNote[f] >= 0) ? fabs(endTime - time) : 10.0;
            double dt = dt1 < dt2 ? dt1 : dt2;
            scale = (float)(0.80 + dt * 5.0);
            if (scale > 1.0f) scale = 1.0f;
        }

        out[f * 3 + 0] = fx;
        out[f * 3 + 1] = fy;
        out[f * 3 + 2] = scale;
    }
}

static void Hook_DrawFeet(void* dlg, void* batch) {
    void* gStyle = *(void**)(g_base + 0x146daf8);
    void* gNotes = *(void**)(g_base + 0x146daf0);
    void* gView  = *(void**)(g_base + 0x146da48);
    BatchDrawFn draw_spr = (BatchDrawFn)(g_base + 0xe2fd10);

    int numPlayers = call_style_num_players(gStyle);
    if (numPlayers <= 0) return;
    if (numPlayers > 16) numPlayers = 16;

    double time = call_get_cursor_time(gView);
    const char* nbeg = call_notes_begin(gNotes);
    const char* nend = call_notes_end(gNotes);
    int ncount = (int)((nend - nbeg) / 0x28);

    uint8_t hasPlayer[16] = {0};
    double minDist[16];
    for (int i = 0; i < 16; ++i) minDist[i] = 9999.0;

    for (int n = 0; n < ncount; ++n) {
        const char* np = nbeg + (size_t)n * 0x28;
        uint32_t f0 = *(const uint32_t*)(np + 0x20);
        if (f0 & 9) continue; /* skip mine(1) and fake(8) */
        int p = (int)(*(const uint32_t*)(np + 0x24) & 0xffffff);
        if (p < 0 || p >= numPlayers) continue;
        hasPlayer[p] = 1;
        double nt = *(const double*)(np + 0x10);
        double ne = *(const double*)(np + 0x18);
        if (ne < nt) ne = nt;
        double d = 0.0;
        if (time < nt) d = nt - time;
        else if (time > ne) d = time - ne;
        if (d < minDist[p]) minDist[p] = d;
    }

    static const float player_rgb[5][3] = {
        {1.00f, 0.36f, 0.36f}, /* P1 Red */
        {0.32f, 0.66f, 1.00f}, /* P2 Blue */
        {1.00f, 0.83f, 0.25f}, /* P3 Gold/Yellow */
        {0.30f, 0.88f, 0.45f}, /* P4 Green */
        {0.82f, 0.42f, 1.00f}, /* P5 Purple */
    };

    void* sprL = (char*)dlg + 0x1b8;
    void* sprR = (char*)dlg + 0x1e0;

    for (int p = 0; p < numPlayers; ++p) {
        if (!hasPlayer[p] && !(p == 0 && ncount == 0)) continue;
        float alpha = 1.0f;
        if (ncount > 0) {
            if (minDist[p] > 2.5) continue;
            if (minDist[p] > 1.5) alpha = (float)(1.0 - (minDist[p] - 1.5));
        }

        int ci = (p < 5) ? p : (p % 5);
        uint32_t col = pack_rgba(player_rgb[ci][0], player_rgb[ci][1], player_rgb[ci][2], alpha);

        float feet[6];
        Hook_GetFeetPositions(dlg, feet, p);
        float lx = feet[0], ly = feet[1], lscale = feet[2];
        float rx = feet[3], ry = feet[4], rscale = feet[5];

        float rot = 0.0f;
        if (fabsf(lx - rx) + fabsf(ly - ry) > 0.1f) {
            if (rx >= lx) {
                rot = atan2f(ry - ly, rx - lx);
                if (rot > 0.45f) rot = 0.45f;
                if (rot < -0.45f) rot = -0.45f;
            } else {
                rot = atan2f(ly - ry, lx - rx);
                if (rot > 0.55f) rot = 0.55f;
                if (rot < -0.55f) rot = -0.55f;
            }
        }

        draw_spr(sprL, batch, lx, ly, rot, lscale, col);
        draw_spr(sprR, batch, rx, ry, rot, rscale, col);
    }
}

/* =========================================================================
 * x64 Patching + IAT Hooks
 * ========================================================================= */
static void write_jmp64(uintptr_t addr, void* target) {
    DWORD old;
    VirtualProtect((void*)addr, 16, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* p = (uint8_t*)addr;
    p[0] = 0x48; p[1] = 0xB8; /* mov rax, imm64 */
    *(uint64_t*)(p + 2) = (uint64_t)(uintptr_t)target;
    p[10] = 0xFF; p[11] = 0xE0; /* jmp rax */
    VirtualProtect((void*)addr, 16, old, &old);
}

static void patch_dancing_bot_draw(void) {
    uintptr_t patch_at = g_base + 0x32ac8a;
    uintptr_t resume_at = g_base + 0x32ade6;
    DWORD old;
    VirtualProtect((void*)patch_at, 32, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* p = (uint8_t*)patch_at;
    /* mov rcx, r13           (4C 89 E9)
     * lea rdx, [rsp + 0x48]  (48 8D 54 24 48)
     * mov rax, Hook_DrawFeet (48 B8 <8B>)
     * call rax               (FF D0)
     * jmp resume_at          (E9 <4B>)
     */
    p[0] = 0x4C; p[1] = 0x89; p[2] = 0xE9;
    p[3] = 0x48; p[4] = 0x8D; p[5] = 0x54; p[6] = 0x24; p[7] = 0x48;
    p[8] = 0x48; p[9] = 0xB8;
    *(uint64_t*)(p + 10) = (uint64_t)(uintptr_t)&Hook_DrawFeet;
    p[18] = 0xFF; p[19] = 0xD0;
    p[20] = 0xE9;
    *(int32_t*)(p + 21) = (int32_t)((intptr_t)resume_at - (intptr_t)(patch_at + 25));
    VirtualProtect((void*)patch_at, 32, old, &old);
}

static void hook_iat_v110(void) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return;

    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(g_base + imp_rva);
    for (; desc->Name; ++desc) {
        const char* dll = (const char*)(g_base + desc->Name);
        int is_msvcp = (_stricmp(dll, "MSVCP140.dll") == 0);
        int is_stdio = (_stricmp(dll, "api-ms-win-crt-stdio-l1-1-0.dll") == 0);
        if (!is_msvcp && !is_stdio) continue;

        IMAGE_THUNK_DATA64* oft = (IMAGE_THUNK_DATA64*)(g_base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA64* ft  = (IMAGE_THUNK_DATA64*)(g_base + desc->FirstThunk);

        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(g_base + oft->u1.AddressOfData);
            const char* name = (const char*)ibn->Name;
            void* replacement = NULL;
            void** save_orig = NULL;

            if (is_msvcp) {
                if (strcmp(name, "?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z") == 0) {
                    save_orig = (void**)&orig_fiopen_w;
                    replacement = (void*)&hook_fiopen_w;
                } else if (strcmp(name, "?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z") == 0) {
                    save_orig = (void**)&orig_fiopen_a;
                    replacement = (void*)&hook_fiopen_a;
                }
            } else if (is_stdio) {
                if (strcmp(name, "fopen") == 0) {
                    save_orig = (void**)&orig_fopen;
                    replacement = (void*)&hook_fopen;
                } else if (strcmp(name, "fopen_s") == 0) {
                    save_orig = (void**)&orig_fopen_s;
                    replacement = (void*)&hook_fopen_s;
                } else if (strcmp(name, "fclose") == 0) {
                    save_orig = (void**)&orig_fclose;
                    replacement = (void*)&hook_fclose;
                }
            }

            if (replacement && save_orig) {
                DWORD old;
                VirtualProtect(&ft->u1.Function, sizeof(uint64_t), PAGE_READWRITE, &old);
                *save_orig = (void*)(uintptr_t)ft->u1.Function;
                ft->u1.Function = (uint64_t)(uintptr_t)replacement;
                VirtualProtect(&ft->u1.Function, sizeof(uint64_t), old, &old);
            }
        }
    }
}

__declspec(dllexport) void AvCoopInit(void) {
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_temp_cs);
        memset(g_temps, 0, sizeof(g_temps));
        g_base = (uintptr_t)GetModuleHandleA(NULL);
        hook_iat_v110();
        write_jmp64(g_base + 0x32a0f0, (void*)&Hook_GetFeetPositions);
        patch_dancing_bot_draw();
    } else if (reason == DLL_PROCESS_DETACH) {
        for (int i = 0; i < 64; ++i) {
            if (g_temps[i].path[0]) DeleteFileW(g_temps[i].path);
        }
        DeleteCriticalSection(&g_temp_cs);
    }
    return TRUE;
}
