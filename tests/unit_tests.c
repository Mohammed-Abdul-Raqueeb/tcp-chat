/*
 * unit_tests.c - tests for the parts that can be tested without a socket:
 * the byte buffer and the nickname rules.
 *
 * These are the pieces where a bug is a memory-safety bug, so they are worth
 * exercising directly under ASan and valgrind rather than only through the
 * server.
 */
#include <stdio.h>
#include <string.h>

#include "../src/buffer.h"
#include "../src/client.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            passed++;                                                          \
        } else {                                                               \
            failed++;                                                          \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

#define TEST(name) static void name(void)
#define RUN(name)                                                              \
    do {                                                                       \
        int before = failed;                                                   \
        name();                                                                \
        printf("  %s %s\n", failed == before ? "ok  " : "FAIL", #name);        \
    } while (0)

/* ---------------------------------------------------------------- buffer */

TEST(a_fresh_buffer_is_empty_and_allocates_nothing)
{
    buffer b;
    buffer_init(&b);
    CHECK(b.len == 0, "len should be 0, got %zu", b.len);
    CHECK(b.data == NULL, "a fresh buffer should not allocate");
    buffer_free(&b); /* must be safe on an empty buffer */
}

TEST(append_accumulates_bytes)
{
    buffer b;
    buffer_init(&b);
    buffer_append_str(&b, "hello ");
    buffer_append_str(&b, "world");
    CHECK(b.len == 11, "expected 11 bytes, got %zu", b.len);
    CHECK(memcmp(b.data, "hello world", 11) == 0, "content mismatch");
    buffer_free(&b);
}

TEST(append_survives_growth_past_the_initial_capacity)
{
    buffer b;
    buffer_init(&b);
    for (int i = 0; i < 10000; i++)
        buffer_append_str(&b, "0123456789");

    CHECK(b.len == 100000, "expected 100000 bytes, got %zu", b.len);
    CHECK(b.cap >= b.len, "capacity %zu must cover length %zu", b.cap, b.len);
    /* Spot-check the far end: a bad realloc would corrupt it. */
    CHECK(memcmp(b.data + 99990, "0123456789", 10) == 0, "tail corrupted");
    buffer_free(&b);
}

TEST(append_of_zero_bytes_is_a_no_op)
{
    buffer b;
    buffer_init(&b);
    CHECK(buffer_append(&b, "ignored", 0) == 0, "should succeed");
    CHECK(b.len == 0, "len should still be 0, got %zu", b.len);
    buffer_free(&b);
}

TEST(consume_removes_a_prefix_and_keeps_the_rest)
{
    buffer b;
    buffer_init(&b);
    buffer_append_str(&b, "abcdef");
    buffer_consume(&b, 2);
    CHECK(b.len == 4, "expected 4 bytes left, got %zu", b.len);
    CHECK(memcmp(b.data, "cdef", 4) == 0, "remaining content wrong");
    buffer_free(&b);
}

TEST(consuming_more_than_is_present_empties_the_buffer)
{
    buffer b;
    buffer_init(&b);
    buffer_append_str(&b, "abc");
    buffer_consume(&b, 99);
    CHECK(b.len == 0, "expected empty, got %zu", b.len);
    buffer_free(&b);
}

TEST(take_line_returns_nothing_until_a_newline_arrives)
{
    buffer b;
    char line[64];
    buffer_init(&b);

    buffer_append_str(&b, "partial mess");
    CHECK(buffer_take_line(&b, line, sizeof line) == 0, "should not yield a line yet");
    CHECK(b.len == 12, "bytes must be retained, got %zu", b.len);

    buffer_append_str(&b, "age\n");
    CHECK(buffer_take_line(&b, line, sizeof line) == 1, "should yield the line now");
    CHECK(strcmp(line, "partial message") == 0, "got \"%s\"", line);
    CHECK(b.len == 0, "buffer should be drained, %zu left", b.len);
    buffer_free(&b);
}

/* The core TCP framing case: several messages arriving in one read. */
TEST(take_line_splits_multiple_messages_from_one_chunk)
{
    buffer b;
    char line[64];
    buffer_init(&b);
    buffer_append_str(&b, "one\ntwo\nthree\n");

    CHECK(buffer_take_line(&b, line, sizeof line) == 1 && strcmp(line, "one") == 0, "first: \"%s\"", line);
    CHECK(buffer_take_line(&b, line, sizeof line) == 1 && strcmp(line, "two") == 0, "second: \"%s\"", line);
    CHECK(buffer_take_line(&b, line, sizeof line) == 1 && strcmp(line, "three") == 0, "third: \"%s\"", line);
    CHECK(buffer_take_line(&b, line, sizeof line) == 0, "should be exhausted");
    buffer_free(&b);
}

/* And the mirror case: two full messages plus half of a third. */
TEST(take_line_keeps_a_trailing_partial_message)
{
    buffer b;
    char line[64];
    buffer_init(&b);
    buffer_append_str(&b, "alpha\nbeta\ngam");

    CHECK(buffer_take_line(&b, line, sizeof line) == 1 && strcmp(line, "alpha") == 0, "got \"%s\"", line);
    CHECK(buffer_take_line(&b, line, sizeof line) == 1 && strcmp(line, "beta") == 0, "got \"%s\"", line);
    CHECK(buffer_take_line(&b, line, sizeof line) == 0, "the fragment is not a line");
    CHECK(b.len == 3, "the fragment must be kept, %zu bytes held", b.len);
    buffer_free(&b);
}

TEST(take_line_strips_a_carriage_return)
{
    buffer b;
    char line[64];
    buffer_init(&b);
    buffer_append_str(&b, "from telnet\r\n");
    CHECK(buffer_take_line(&b, line, sizeof line) == 1, "should yield a line");
    CHECK(strcmp(line, "from telnet") == 0, "CR not stripped: \"%s\"", line);
    buffer_free(&b);
}

TEST(take_line_handles_an_empty_line)
{
    buffer b;
    char line[64];
    buffer_init(&b);
    buffer_append_str(&b, "\nafter\n");
    CHECK(buffer_take_line(&b, line, sizeof line) == 1, "should yield the empty line");
    CHECK(line[0] == '\0', "expected an empty string, got \"%s\"", line);
    CHECK(buffer_take_line(&b, line, sizeof line) == 1 && strcmp(line, "after") == 0, "got \"%s\"", line);
    buffer_free(&b);
}

/* A line longer than the destination must truncate, not overflow. ASan turns
 * a failure here into a crash rather than a silent corruption. */
TEST(take_line_truncates_instead_of_overflowing)
{
    buffer b;
    char line[8];
    buffer_init(&b);
    buffer_append_str(&b, "abcdefghijklmnop\nnext\n");

    CHECK(buffer_take_line(&b, line, sizeof line) == 1, "should yield a line");
    CHECK(strlen(line) == 7, "expected 7 chars, got %zu", strlen(line));
    CHECK(strcmp(line, "abcdefg") == 0, "got \"%s\"", line);
    /* The whole over-long line must still be consumed, or the parser would
     * loop on the same bytes forever. */
    CHECK(buffer_take_line(&b, line, sizeof line) == 1 && strcmp(line, "next") == 0,
          "the rest of the long line was not consumed: \"%s\"", line);
    buffer_free(&b);
}

TEST(find_newline_reports_the_first_position)
{
    buffer b;
    buffer_init(&b);
    CHECK(buffer_find_newline(&b) == -1, "empty buffer has no newline");
    buffer_append_str(&b, "abc\ndef\n");
    CHECK(buffer_find_newline(&b) == 3, "expected 3, got %ld", buffer_find_newline(&b));
    buffer_free(&b);
}

/* The buffer holds bytes, not C strings, so a NUL in the middle must not
 * truncate anything. */
TEST(buffer_is_binary_safe)
{
    buffer b;
    buffer_init(&b);
    buffer_append(&b, "ab\0cd\n", 6);
    CHECK(b.len == 6, "expected 6 bytes, got %zu", b.len);
    CHECK(buffer_find_newline(&b) == 5, "newline should be at 5, got %ld", buffer_find_newline(&b));
    buffer_free(&b);
}

/* ------------------------------------------------------------- nicknames */

TEST(valid_nicknames_are_accepted)
{
    const char *ok[] = { "a", "alice", "bob_99", "x-y-z", "ABC123", NULL };
    for (int i = 0; ok[i]; i++)
        CHECK(nick_validate(ok[i]) == NULL, "\"%s\" should be valid but got: %s",
              ok[i], nick_validate(ok[i]));
}

TEST(invalid_nicknames_are_rejected_with_a_reason)
{
    CHECK(nick_validate("") != NULL, "empty should be rejected");
    CHECK(nick_validate("has space") != NULL, "spaces should be rejected");
    CHECK(nick_validate("semi;colon") != NULL, "punctuation should be rejected");
    CHECK(nick_validate("new\nline") != NULL, "a newline would forge a protocol message");
    CHECK(nick_validate("bell\x07") != NULL, "control characters should be rejected");
    CHECK(nick_validate("abcdefghijklmnopqrstuvwxyz") != NULL, "over-long should be rejected");
}

TEST(a_nickname_of_exactly_the_limit_is_accepted)
{
    char nick[MAX_NICK_LEN + 1];
    memset(nick, 'a', MAX_NICK_LEN);
    nick[MAX_NICK_LEN] = '\0';
    CHECK(nick_validate(nick) == NULL, "%d characters should be allowed", MAX_NICK_LEN);
}

/* ---------------------------------------------------------------- roster */

TEST(roster_assigns_distinct_default_nicknames)
{
    roster r;
    roster_init(&r);

    /* fd values are never used by these functions; -2 just marks the slot
     * as occupied without owning a real descriptor. */
    client *a = roster_add(&r, -2, "1.1.1.1:1");
    client *b = roster_add(&r, -3, "1.1.1.2:2");

    CHECK(a != NULL && b != NULL, "both adds should succeed");
    CHECK(strcmp(a->nick, b->nick) != 0, "nicknames collided: %s", a->nick);
    CHECK(r.count == 2, "count should be 2, got %d", r.count);

    a->fd = -1; /* release without close(), since these are not real fds */
    b->fd = -1;
    buffer_free(&a->in); buffer_free(&a->out);
    buffer_free(&b->in); buffer_free(&b->out);
}

TEST(roster_lookup_is_case_insensitive_and_can_exclude_self)
{
    roster r;
    roster_init(&r);
    client *a = roster_add(&r, -2, "1.1.1.1:1");
    snprintf(a->nick, sizeof a->nick, "Alice");

    CHECK(roster_find_nick(&r, "alice") == a, "lookup should ignore case");
    CHECK(roster_find_nick(&r, "ALICE") == a, "lookup should ignore case");
    CHECK(roster_find_nick(&r, "bob") == NULL, "unknown nick should not be found");
    CHECK(roster_find_nick_excluding(&r, "alice", a) == NULL,
          "excluding self should report no collision");

    a->fd = -1;
    buffer_free(&a->in);
    buffer_free(&a->out);
}

TEST(roster_refuses_to_exceed_its_capacity)
{
    roster r;
    roster_init(&r);

    for (int i = 0; i < MAX_CLIENTS; i++)
        CHECK(roster_add(&r, -2 - i, "x") != NULL, "add %d should succeed", i);

    CHECK(roster_add(&r, -999, "x") == NULL, "the extra client should be refused");
    CHECK(r.count == MAX_CLIENTS, "count should be %d, got %d", MAX_CLIENTS, r.count);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        r.clients[i].fd = -1;
        buffer_free(&r.clients[i].in);
        buffer_free(&r.clients[i].out);
    }
}

int main(void)
{
    printf("buffer\n");
    RUN(a_fresh_buffer_is_empty_and_allocates_nothing);
    RUN(append_accumulates_bytes);
    RUN(append_survives_growth_past_the_initial_capacity);
    RUN(append_of_zero_bytes_is_a_no_op);
    RUN(consume_removes_a_prefix_and_keeps_the_rest);
    RUN(consuming_more_than_is_present_empties_the_buffer);

    printf("framing\n");
    RUN(take_line_returns_nothing_until_a_newline_arrives);
    RUN(take_line_splits_multiple_messages_from_one_chunk);
    RUN(take_line_keeps_a_trailing_partial_message);
    RUN(take_line_strips_a_carriage_return);
    RUN(take_line_handles_an_empty_line);
    RUN(take_line_truncates_instead_of_overflowing);
    RUN(find_newline_reports_the_first_position);
    RUN(buffer_is_binary_safe);

    printf("nicknames\n");
    RUN(valid_nicknames_are_accepted);
    RUN(invalid_nicknames_are_rejected_with_a_reason);
    RUN(a_nickname_of_exactly_the_limit_is_accepted);

    printf("roster\n");
    RUN(roster_assigns_distinct_default_nicknames);
    RUN(roster_lookup_is_case_insensitive_and_can_exclude_self);
    RUN(roster_refuses_to_exceed_its_capacity);

    printf("\n%d checks passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
