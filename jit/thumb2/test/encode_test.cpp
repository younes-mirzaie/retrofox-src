// Prints "<asm text>|<hex bytes>" for each case in cases.inc (see gen_cases.py),
// plus a brute-force check of EncodeModifiedImm/DecodeModifiedImm.
#include "Thumb2Assembler.h"
#include <stdio.h>
#include <set>

using namespace rfx::thumb2;

static void Print(const char* text, const Assembler& A)
{
    printf("%s|", text);
    for (size_t i = 0; i < A.code.size(); i++)
        printf("%02x", A.code[i]);
    printf("%s\n", A.ok ? "" : "|NOT-OK");
}

#define CASE(text, ...) do { Assembler A; __VA_ARGS__; Print(text, A); } while (0)

int main()
{
#include "cases.inc"
    // Modified immediates: every encoding decodes to a value that encodes back to it,
    // and values outside that set are rejected.
    std::set<uint32_t> all;
    int bad = 0;
    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t v = DecodeModifiedImm(i), back;
        all.insert(v);
        if (!EncodeModifiedImm(v, &back) || DecodeModifiedImm(back) != v) bad++;
    }
    uint32_t x = 12345;
    for (int i = 0; i < 1000000; i++) {
        x = x * 1103515245u + 12345u;
        uint32_t v = x ^ (x >> 7), imm;
        bool enc = EncodeModifiedImm(v, &imm);
        if (enc != (all.count(v) != 0)) bad++;
    }
    fprintf(stderr, "modified-immediate check: %s (%zu distinct values)\n", bad ? "FAILED" : "ok", all.size());
    return bad ? 1 : 0;
}
