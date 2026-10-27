# nvmetal design

## Decision log

| # | Decision | Why |
|---|----------|-----|
| 1 | Cite the NVM Express Base Specification **revision 1.4c** for every register and command. | 1.4c is the last revision with the NVM command set inside the base document, so one document covers registers, admin and I/O commands. Section titles are given next to numbers so 2.x readers can find them. |
| 2 | C11 + CMake/Ninja, one flag set (`-Wall -Wextra -Werror -O2 -g`) for every build type, sanitizers via `-DNVMETAL_SANITIZE=`. | Sanitizer builds run the same optimised code paths as release builds. |
| 3 | Own ~60-line test harness (`tests/test.h`) instead of a framework. | No third-party code; tests need only libc. |
| 4 | CI pins clang-format 21.1.8 from PyPI. | Ubuntu 24.04 ships clang-format 18, which formats differently from the 21.x used for development. |
