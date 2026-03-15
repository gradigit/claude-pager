#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGER_TESTING 1
#include "pager.c"

static void failf(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void assert_true(int cond, const char *msg) {
    if (!cond) failf(msg);
}

static void assert_int_eq(int got, int want, const char *msg) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got=%d want=%d)\n", msg, got, want);
        exit(1);
    }
}

static void reset_render_state(int cols) {
    g_cols = cols;
    g_rows = 24;
    g_queue_rows = 0;
    g_crows = 21;
    g_fd = -1;
    g_ol = 0;
    g_quit = 0;
    g_hover_row = 0;
    g_hover_uri[0] = '\0';
    g_mouse_x = 0;
    g_mouse_y = 0;
    link_map_clear();
}

static int current_rows_consumed(const Lines *l) {
    int row = 2;
    link_map_clear();
    for (int i = 0; i < l->n; i++) {
        if (line_is_wrap_placeholder(l->d[i])) {
            row++;
            continue;
        }
        (void)link_map_track_line(l->d[i], row);
        row++;
    }
    return row - 2;
}

static int legacy_rows_consumed(const Lines *l) {
    int row = 2;
    link_map_clear();
    for (int i = 0; i < l->n; i++) {
        int used_rows = link_map_track_line(l->d[i], row);
        row += (used_rows > 0 ? used_rows : 1);
    }
    return row - 2;
}

static void test_wrap_slots_mark_placeholders(void) {
    reset_render_state(10);
    Lines l;
    L_init(&l);
    L_pushw(&l, "1234567890123456789012345");

    assert_int_eq(l.n, 3, "wrapped line should reserve three visual slots");
    assert_true(strcmp(l.d[0], "1234567890123456789012345") == 0, "first slot should be original line");
    assert_true(line_is_wrap_placeholder(l.d[1]), "second slot should be wrap placeholder");
    assert_true(line_is_wrap_placeholder(l.d[2]), "third slot should be wrap placeholder");

    L_free(&l);
}

static void test_normalize_offset_skips_placeholders(void) {
    reset_render_state(10);
    Lines l;
    L_init(&l);
    L_pushw(&l, "1234567890123456789012345");
    L_pushw(&l, "tail");

    assert_int_eq(normalize_off_visual(&l, 1, -1), 0, "backward normalize should land on wrapped line head");
    assert_int_eq(normalize_off_visual(&l, 1, +1), 3, "forward normalize should skip to next real line");
    assert_int_eq(normalize_off_visual(&l, 2, +1), 3, "forward normalize should skip all wrap placeholders");

    L_free(&l);
}

static void test_current_row_accounting_matches_slots(void) {
    reset_render_state(10);
    Lines l;
    L_init(&l);
    L_pushw(&l, "1234567890123456789012345");

    assert_int_eq(current_rows_consumed(&l), l.n, "current draw accounting should match reserved visual slots");

    L_free(&l);
}

static void test_legacy_row_accounting_overcounts_wrapped_lines(void) {
    reset_render_state(10);
    Lines l;
    L_init(&l);
    L_pushw(&l, "1234567890123456789012345");

    assert_int_eq(legacy_rows_consumed(&l), 5, "legacy row accounting should overcount wrapped lines");
    assert_true(legacy_rows_consumed(&l) > l.n, "legacy row accounting should exceed reserved slots");

    L_free(&l);
}

static void test_unwrapped_line_is_stable(void) {
    reset_render_state(80);
    Lines l;
    L_init(&l);
    L_pushw(&l, "short line");

    assert_int_eq(l.n, 1, "short line should reserve one slot");
    assert_int_eq(current_rows_consumed(&l), 1, "current row accounting should be one row for short lines");
    assert_int_eq(legacy_rows_consumed(&l), 1, "legacy row accounting should also be one row for short lines");

    L_free(&l);
}

int main(void) {
    test_wrap_slots_mark_placeholders();
    test_normalize_offset_skips_placeholders();
    test_current_row_accounting_matches_slots();
    test_legacy_row_accounting_overcounts_wrapped_lines();
    test_unwrapped_line_is_stable();
    printf("ok\n");
    return 0;
}
