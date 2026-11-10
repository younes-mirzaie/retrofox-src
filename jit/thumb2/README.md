# Thumb-2 encoder (JIT groundwork)

`Thumb2Assembler.h` encodes the Thumb-2 instructions SpiderMonkey's ARM JIT needs, for the
Windows-on-ARM port (plan: [notes/jit-thumb2-plan.md](../../notes/jit-thumb2-plan.md)).
No dependencies, not even the C++ library.

Tests (Linux with clang, llvm-mc, qemu-user):
- `test/check-encodings.sh`: compares 1,042 generated instructions against llvm-mc
  (`SEED=n` for another random set).
- `test/run-exec.sh`: generates code and runs it under qemu-arm (31 checks).
