/*
 * Minimal unit test harness: each test binary registers cases with TEST(),
 * and main() from TEST_MAIN() runs them and returns non-zero on any failure.
 * Kept tiny on purpose so the tests need nothing beyond libc.
 */
#ifndef NVMETAL_TEST_H
#define NVMETAL_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct test_case {
    const char *name;
    void (*fn)(void);
};

extern int test_failures;
extern int test_checks;

#define TEST_MAX_CASES 128
extern struct test_case test_cases[TEST_MAX_CASES];
extern int test_ncases;

#define TEST(name)                                                                                 \
    static void name(void);                                                                        \
    __attribute__((constructor)) static void register_##name(void)                                 \
    {                                                                                              \
        if (test_ncases < TEST_MAX_CASES)                                                          \
            test_cases[test_ncases++] = (struct test_case){#name, name};                           \
    }                                                                                              \
    static void name(void)

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        test_checks++;                                                                             \
        if (!(cond)) {                                                                             \
            test_failures++;                                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);           \
        test_checks++;                                                                             \
        if (va_ != vb_) {                                                                          \
            test_failures++;                                                                       \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (0x%llx) != %s (0x%llx)\n", __FILE__,      \
                    __LINE__, #a, va_, #b, vb_);                                                   \
        }                                                                                          \
    } while (0)

#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond);             \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)

#define TEST_MAIN()                                                                                \
    int test_failures, test_checks, test_ncases;                                                   \
    struct test_case test_cases[TEST_MAX_CASES];                                                   \
    int main(int argc, char **argv)                                                                \
    {                                                                                              \
        int ran = 0, failed_cases = 0;                                                             \
        for (int i = 0; i < test_ncases; i++) {                                                    \
            if (argc > 1 && strcmp(argv[1], test_cases[i].name) != 0)                              \
                continue;                                                                          \
            int before = test_failures;                                                            \
            test_cases[i].fn();                                                                    \
            ran++;                                                                                 \
            int ok = test_failures == before;                                                      \
            failed_cases += !ok;                                                                   \
            printf("[%s] %s\n", ok ? "PASS" : "FAIL", test_cases[i].name);                         \
        }                                                                                          \
        printf("%d cases, %d passed, %d failed, %d checks\n", ran, ran - failed_cases,             \
               failed_cases, test_checks);                                                         \
        return failed_cases || ran == 0 ? 1 : 0;                                                   \
    }

#endif
