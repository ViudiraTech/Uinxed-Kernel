/*
 *
 *      seccomp_bpf.c
 *      Classic BPF verifier and interpreter used by seccomp filters.
 *
 *      2026/8/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <libs/std/string.h>
#include <security/seccomp.h>

/* Validate a word-aligned absolute load from a seccomp_data field. */
static bool seccomp_bpf_valid_load(uint16_t code, uint32_t offset)
{
    if (code != (BPF_LD | BPF_W | BPF_ABS)) return false;
    if (offset & (sizeof(uint32_t) - 1U)) return false;
    return offset <= sizeof(struct seccomp_data) - sizeof(uint32_t);
}

/* Validate an ALU instruction, including BPF_K/BPF_X and shift-width rules. */
static bool seccomp_bpf_valid_alu(uint16_t code, uint32_t k)
{
    uint16_t op  = BPF_OP(code);
    uint16_t src = BPF_SRC(code);
    if (code == (BPF_ALU | BPF_NEG)) return true;
    if (src != BPF_K && src != BPF_X) return false;
    switch (op) {
        case BPF_ADD :
        case BPF_SUB :
        case BPF_MUL :
        case BPF_OR :
        case BPF_AND :
        case BPF_LSH :
        case BPF_RSH :
        case BPF_XOR :
            return src == BPF_X || (op != BPF_LSH && op != BPF_RSH) || k < 32U;
        case BPF_DIV :
            return src == BPF_X || k != 0;
        default :
            return false;
    }
}

/* Validate a jump instruction so both targets stay inside the program. */
static bool seccomp_bpf_valid_jump(const struct sock_filter *instruction, size_t index, size_t length)
{
    uint16_t op = BPF_OP(instruction->code);
    if (instruction->code == (BPF_JMP | BPF_JA)) return instruction->k < length - index - 1U;
    if (BPF_SRC(instruction->code) != BPF_K && BPF_SRC(instruction->code) != BPF_X) return false;
    if (op != BPF_JEQ && op != BPF_JGT && op != BPF_JGE && op != BPF_JSET) return false;
    size_t remaining = length - index - 1U;
    return instruction->jt < remaining && instruction->jf < remaining;
}

/* Verify a filter program is well-formed and safe to run. */
int seccomp_bpf_validate(const struct sock_filter *program, size_t length)
{
    if (!program || !length || length > SECCOMP_MAX_INSNS_PER_FILTER) return -EINVAL;
    for (size_t i = 0; i < length; i++) {
        const struct sock_filter *instruction = &program[i];
        uint16_t                  code        = instruction->code;
        switch (BPF_CLASS(code)) {
            case BPF_LD :
                if (seccomp_bpf_valid_load(code, instruction->k)) break;
                if (code == (BPF_LD | BPF_W | BPF_LEN) || code == (BPF_LD | BPF_IMM)) break;
                if (code == (BPF_LD | BPF_MEM) && instruction->k < BPF_MEMWORDS) break;
                return -EINVAL;
            case BPF_LDX :
                if (code == (BPF_LDX | BPF_W | BPF_LEN) || code == (BPF_LDX | BPF_IMM)) break;
                if (code == (BPF_LDX | BPF_MEM) && instruction->k < BPF_MEMWORDS) break;
                return -EINVAL;
            case BPF_ST :
            case BPF_STX :
                if (code != BPF_CLASS(code) || instruction->k >= BPF_MEMWORDS) return -EINVAL;
                break;
            case BPF_ALU :
                if (!seccomp_bpf_valid_alu(code, instruction->k)) return -EINVAL;
                break;
            case BPF_JMP :
                if (!seccomp_bpf_valid_jump(instruction, i, length)) return -EINVAL;
                break;
            case BPF_RET :
                if (code != (BPF_RET | BPF_K) && code != (BPF_RET | BPF_A)) return -EINVAL;
                break;
            case BPF_MISC :
                if (code != (BPF_MISC | BPF_TAX) && code != (BPF_MISC | BPF_TXA)) return -EINVAL;
                break;
            default :
                return -EINVAL;
        }
    }
    if (BPF_CLASS(program[length - 1U].code) != BPF_RET) return -EINVAL;

    /*
     * Linux classic-BPF requires every scratch-memory read to be initialized
     * on every reachable predecessor path.  All jumps are forward, so one
     * ascending data-flow pass reaches a fixed point.
     */
    uint16_t memory_masks[SECCOMP_MAX_INSNS_PER_FILTER] = {0};
    uint8_t  reachable[SECCOMP_MAX_INSNS_PER_FILTER]    = {0};
    reachable[0]                                        = 1;
    for (size_t i = 0; i < length; i++) {
        if (!reachable[i]) continue;
        const struct sock_filter *instruction = &program[i];
        uint16_t                  mask        = memory_masks[i];
        if ((instruction->code == (BPF_LD | BPF_MEM) || instruction->code == (BPF_LDX | BPF_MEM)) && !(mask & (uint16_t)(1U << instruction->k))) return -EINVAL;
        if (instruction->code == BPF_ST || instruction->code == BPF_STX) mask |= (uint16_t)(1U << instruction->k);
        if (BPF_CLASS(instruction->code) == BPF_RET) continue;

        size_t successors[2];
        size_t count = 1;
        if (instruction->code == (BPF_JMP | BPF_JA))
            successors[0] = i + 1U + instruction->k;
        else if (BPF_CLASS(instruction->code) == BPF_JMP) {
            successors[0] = i + 1U + instruction->jt;
            successors[1] = i + 1U + instruction->jf;
            count         = 2;
        } else
            successors[0] = i + 1U;
        for (size_t branch = 0; branch < count; branch++) {
            size_t target = successors[branch];
            if (!reachable[target]) {
                reachable[target]    = 1;
                memory_masks[target] = mask;
            } else {
                memory_masks[target] &= mask;
            }
        }
    }
    return EOK;
}

/* Byte-wise seccomp_data load, avoiding alignment traps for ABS fetches. */
static uint32_t seccomp_bpf_load_word(const struct seccomp_data *data, uint32_t offset)
{
    uint32_t value;
    memcpy(&value, (const uint8_t *)data + offset, sizeof(value));
    return value;
}

/* Interpret a validated classic-BPF filter against a seccomp_data snapshot. */
uint32_t seccomp_bpf_run(const struct sock_filter *program, size_t length, const struct seccomp_data *data)
{
    uint32_t accumulator          = 0;
    uint32_t index                = 0;
    uint32_t memory[BPF_MEMWORDS] = {0};

    for (size_t pc = 0; pc < length; pc++) {
        const struct sock_filter *instruction = &program[pc];
        uint16_t                  code        = instruction->code;
        switch (BPF_CLASS(code)) {
            case BPF_LD :
                if (code == (BPF_LD | BPF_W | BPF_ABS))
                    accumulator = seccomp_bpf_load_word(data, instruction->k);
                else if (code == (BPF_LD | BPF_W | BPF_LEN))
                    accumulator = sizeof(*data);
                else if (code == (BPF_LD | BPF_IMM))
                    accumulator = instruction->k;
                else
                    accumulator = memory[instruction->k];
                break;
            case BPF_LDX :
                if (code == (BPF_LDX | BPF_W | BPF_LEN))
                    index = sizeof(*data);
                else if (code == (BPF_LDX | BPF_IMM))
                    index = instruction->k;
                else
                    index = memory[instruction->k];
                break;
            case BPF_ST :
                memory[instruction->k] = accumulator;
                break;
            case BPF_STX :
                memory[instruction->k] = index;
                break;
            case BPF_ALU : {
                uint32_t operand = BPF_SRC(code) == BPF_X ? index : instruction->k;
                switch (BPF_OP(code)) {
                    case BPF_ADD :
                        accumulator += operand;
                        break;
                    case BPF_SUB :
                        accumulator -= operand;
                        break;
                    case BPF_MUL :
                        accumulator *= operand;
                        break;
                    case BPF_DIV :
                        if (!operand) return SECCOMP_RET_KILL_THREAD;
                        accumulator /= operand;
                        break;
                    case BPF_OR :
                        accumulator |= operand;
                        break;
                    case BPF_AND :
                        accumulator &= operand;
                        break;
                    case BPF_LSH :
                        accumulator = operand < 32U ? accumulator << operand : 0;
                        break;
                    case BPF_RSH :
                        accumulator = operand < 32U ? accumulator >> operand : 0;
                        break;
                    case BPF_NEG :
                        accumulator = (uint32_t)-accumulator;
                        break;
                    case BPF_MOD :
                        if (!operand) return SECCOMP_RET_KILL_THREAD;
                        accumulator %= operand;
                        break;
                    case BPF_XOR :
                        accumulator ^= operand;
                        break;
                    default :
                        return SECCOMP_RET_KILL_THREAD;
                }
                break;
            }
            case BPF_JMP :
                if (code == (BPF_JMP | BPF_JA)) {
                    pc += instruction->k;
                    break;
                }
                {
                    uint32_t operand = BPF_SRC(code) == BPF_X ? index : instruction->k;
                    bool     taken;
                    switch (BPF_OP(code)) {
                        case BPF_JEQ :
                            taken = accumulator == operand;
                            break;
                        case BPF_JGT :
                            taken = accumulator > operand;
                            break;
                        case BPF_JGE :
                            taken = accumulator >= operand;
                            break;
                        case BPF_JSET :
                            taken = (accumulator & operand) != 0;
                            break;
                        default :
                            return SECCOMP_RET_KILL_THREAD;
                    }
                    pc += taken ? instruction->jt : instruction->jf;
                }
                break;
            case BPF_RET :
                return BPF_RVAL(code) == BPF_A ? accumulator : instruction->k;
            case BPF_MISC :
                if (BPF_MISCOP(code) == BPF_TAX)
                    index = accumulator;
                else
                    accumulator = index;
                break;
            default :
                return SECCOMP_RET_KILL_THREAD;
        }
    }
    return SECCOMP_RET_KILL_THREAD;
}
