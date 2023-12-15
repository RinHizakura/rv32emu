/* RV32I Base Instruction Set */
/* conforming to the instructions specified in chapter 2 of the unprivileged
 * specification version 20191213.
 */

/* Currently, the tier-1 JIT compiler only supports for RV32-IMC extension,
 * RV32-A and RV32-F extension are unsupported.
 */

/* The parameter of x64 instruction API
 * size: size of data
 * op: opcode
 * src: source register
 * dst: destination register
 * pc: program counter
 *
 * 1. alu[32|64]_imm, size, op, src, dst, imm; << Do ALU operation on src and
 * imm and store the result into dst.
 * 2. alu[32|64], op, src, dst; << Do ALU operation on src and dst and store the
 * result into dst.
 * 3. ld_imm, dst, imm;  << Load immediate into dst.
 * 4. ld_sext, size, src, dst, offset; << Load data of a specified size from
 * memory and sign-extend it into the dst, using the memory address calculated
 * as the sum of the src and the specified offset.
 * 5. ld, size, dst, member, field; << load the target field from rv data
 * structure to dst.
 * 6. st_imm, size, field, imm; << store immediate to the target field of rv
 * data structure.
 * 7. st, size, dst, member, field; << store src value to the target field of rv
 * data structure.
 * 8. cmp, src, dst; << compare the value between src and dst.
 * 9. cmp_imm, src, imm; << compare the value of src and imm.
 * 10. jmp, pc, imm; << jump to the program counter of pc + imm.
 * 11. jcc, op; << jump with condition.
 * 12. set_jmp_off; << set the location of jump with condition instruction.
 * 13. jmp_off; << set the jump target of jump with condition instruction.
 * 14. mem; << get memory base.
 * 15. call, handler; << call function handler stored in rv->io.
 * 16. exit; << exit machine code execution.
 * 17. mul, op, src, dst, imm; << Do mul operation on src and dst and store the
 * result into dst.
 * 18. div, op, src, dst, imm; << Do div operation on src and dst and store the
 * result into dst.
 * 19. mod, op, src, dst, imm; << Do mod operation on src and dst and store the
 * result into dst.
 * 20. cond, src; << set condition if (src)
 * 21. end; << set the end of condition if (src)
 */

/* Internal */
RVOP(
    nop,
    { rv->X[rv_reg_zero] = 0; },
    X64({}))

/* LUI is used to build 32-bit constants and uses the U-type format. LUI
 * places the U-immediate value in the top 20 bits of the destination
 * register rd, filling in the lowest 12 bits with zeros. The 32-bit
 * result is sign-extended to 64 bits.
 */
RVOP(
    lui,
    { rv->X[ir->rd] = ir->imm; },
    X64({
        ld_imm, RAX, imm;
        st, S32, RAX, X, rd;
    }))

/* AUIPC is used to build pc-relative addresses and uses the U-type format.
 * AUIPC forms a 32-bit offset from the 20-bit U-immediate, filling in the
 * lowest 12 bits with zeros, adds this offset to the address of the AUIPC
 * instruction, then places the result in register rd.
 */
RVOP(
    auipc,
    { rv->X[ir->rd] = ir->imm + PC; },
    X64({
        ld_imm, RAX, pc, imm;
        st, S32, RAX, X, rd;
    }))

/* JAL: Jump and Link
 * store successor instruction address into rd.
 * add next J imm (offset) to pc.
 */
RVOP(
    jal,
    {
        const uint32_t pc = PC;
        /* Jump */
        PC += ir->imm;
        /* link with return address */
        if (ir->rd)
            rv->X[ir->rd] = pc + 4;
        /* check instruction misaligned */
        RV_EXC_MISALIGN_HANDLER(pc, insn, false, 0);
        struct rv_insn *taken = ir->branch_taken;
        if (taken) {
#if RV32_HAS(JIT)
            block_t *block = cache_get(rv->block_cache, PC);
            if (!block) {
                ir->branch_taken = NULL;
                goto end_insn;
            }
            if (cache_hot(rv->block_cache, PC))
                goto end_insn;
#endif
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    end_insn:
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
    },
    X64({
        cond, rd;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, X, rd;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        jmp, pc, imm;
        exit;
    }))

/* The branch history table records historical data pertaining to indirect jump
 * targets. This functionality alleviates the need to invoke block_find() and
 * incurs overhead only when the indirect jump targets are not previously
 * recorded. Additionally, the C code generator can reference the branch history
 * table to link he indirect jump targets.
 */
#define LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE()                               \
    /* lookup branch history table */                                         \
    for (int i = 0; i < HISTORY_SIZE; i++) {                                  \
        if (ir->branch_table->PC[i] == PC) {                                  \
            MUST_TAIL return ir->branch_table->target[i]->impl(               \
                rv, ir->branch_table->target[i], cycle, PC);                  \
        }                                                                     \
    }                                                                         \
    block_t *block = block_find(&rv->block_map, PC);                          \
    if (block) {                                                              \
        /* update branch history table */                                     \
        ir->branch_table->PC[ir->branch_table->idx] = PC;                     \
        ir->branch_table->target[ir->branch_table->idx] = block->ir_head;     \
        ir->branch_table->idx = (ir->branch_table->idx + 1) % HISTORY_SIZE;   \
        MUST_TAIL return block->ir_head->impl(rv, block->ir_head, cycle, PC); \
    }

/* The indirect jump instruction JALR uses the I-type encoding. The target
 * address is obtained by adding the sign-extended 12-bit I-immediate to the
 * register rs1, then setting the least-significant bit of the result to zero.
 * The address of the instruction following the jump (pc+4) is written to
 * register rd. Register x0 can be used as the destination if the result is
 * not required.
 */
RVOP(
    jalr,
    {
        const uint32_t pc = PC;
        /* jump */
        PC = (rv->X[ir->rs1] + ir->imm) & ~1U;
        /* link */
        if (ir->rd)
            rv->X[ir->rd] = pc + 4;
        /* check instruction misaligned */
        RV_EXC_MISALIGN_HANDLER(pc, insn, false, 0);
#if !RV32_HAS(JIT)
        LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE();
#endif
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
    },
    X64({
        cond, rd;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, X, rd;
        end;
        ld, S32, RAX, X, rs1;
        alu32_imm, 32, 0x81, 0, RAX, imm;
        alu32_imm, 32, 0x81, 4, RAX, ~1U;
        st, S32, RAX, PC;
        exit;
    }))

/* clang-format off */
#define BRANCH_FUNC(type, cond)                                                \
    const uint32_t pc = PC;                                                    \
    if ((type) rv->X[ir->rs1] cond (type)rv->X[ir->rs2]) {                     \
        is_branch_taken = false;                                               \
        struct rv_insn *untaken = ir->branch_untaken;                          \
        if (!untaken)                                                          \
            goto nextop;                                                       \
        IIF(RV32_HAS(JIT))                                                     \
        (                                                                      \
            block_t *block = cache_get(rv->block_cache, PC + 4); if (!block) { \
                ir->branch_untaken = NULL;                                     \
                goto nextop;                                                   \
            } untaken = ir->branch_untaken = block->ir_head;                   \
            if (cache_hot(rv->block_cache, PC + 4)) goto nextop;, );           \
        PC += 4;                                                               \
        last_pc = PC;                                                          \
        MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);                \
    }                                                                          \
    is_branch_taken = true;                                                    \
    PC += ir->imm;                                                             \
    /* check instruction misaligned */                                         \
    RV_EXC_MISALIGN_HANDLER(pc, insn, false, 0);                               \
    struct rv_insn *taken = ir->branch_taken;                                  \
    if (taken) {                                                               \
        IIF(RV32_HAS(JIT))                                                     \
        (                                                                      \
            {block_t *block = cache_get(rv->block_cache, PC); if (!block) {     \
                ir->branch_taken = NULL;                                       \
                goto end_insn;                                                 \
            } taken = ir->branch_taken = block->ir_head;                       \
            if (cache_hot(rv->block_cache, PC)) goto end_insn;}, );             \
        last_pc = PC;                                                          \
        MUST_TAIL return taken->impl(rv, taken, cycle, PC);                    \
    }                                                                          \
    end_insn:                                                                  \
    rv->csr_cycle = cycle;                                                     \
    rv->PC = PC;                                                               \
    return true;
/* clang-format on */

/* In RV32I and RV64I, if the branch is taken, set pc = pc + offset, where
 * offset is a multiple of two; else do nothing. The offset is 13 bits long.
 *
 * The condition for branch taken depends on the value in mnemonic, which is
 * one of:
 * - "beq": src1 == src2
 * - "bne": src1 != src2
 * - "blt": src1 < src2 as signed integers
 * - "bge": src1 >= src2 as signed integers
 * - "bltu": src1 < src2 as unsigned integers
 * - "bgeu": src1 >= src2 as unsigned integers
 *
 * On branch taken, an instruction-address-misaligned exception is generated
 * if the target pc is not 4-byte aligned.
 */

/* BEQ: Branch if Equal */
RVOP(
    beq,
    { BRANCH_FUNC(uint32_t, !=); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        set_jmp_off;
        jcc, 0x84;
        cond, branch_untaken;
        jmp, pc, 4;
        end;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
    }))

/* BNE: Branch if Not Equal */
RVOP(
    bne,
    { BRANCH_FUNC(uint32_t, ==); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        set_jmp_off;
        jcc, 0x85;
        cond, branch_untaken;
        jmp, pc, 4;
        end;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
    }))

/* BLT: Branch if Less Than */
RVOP(
    blt,
    { BRANCH_FUNC(int32_t, >=); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        set_jmp_off;
        jcc, 0x8c;
        cond, branch_untaken;
        jmp, pc, 4;
        end;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
    }))

/* BGE: Branch if Greater Than */
RVOP(
    bge,
    { BRANCH_FUNC(int32_t, <); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        set_jmp_off;
        jcc, 0x8d;
        cond, branch_untaken;
        jmp, pc, 4;
        end;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
    }))

/* BLTU: Branch if Less Than Unsigned */
RVOP(
    bltu,
    { BRANCH_FUNC(uint32_t, >=); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        set_jmp_off;
        jcc, 0x82;
        cond, branch_untaken;
        jmp, pc, 4;
        end;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
    }))

/* BGEU: Branch if Greater Than Unsigned */
RVOP(
    bgeu,
    { BRANCH_FUNC(uint32_t, <); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        set_jmp_off;
        jcc, 0x83;
        cond, branch_untaken;
        jmp, pc, 4;
        end;
        ld_imm, RAX, pc, 4;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
    }))

/* There are 5 types of loads: two for byte and halfword sizes, and one for word
 * size. Two instructions are required for byte and halfword loads because they
 * can be either zero-extended or sign-extended to fill the register. However,
 * for word-sized loads, an entire register's worth of data is read from memory,
 * and no extension is needed.
 */

/* LB: Load Byte */
RVOP(
    lb,
    {
        rv->X[ir->rd] =
            sign_extend_b(rv->io.mem_read_b(rv->X[ir->rs1] + ir->imm));
    },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld_sext, S8, RAX, RBX, 0;
        st, S32, RBX, X, rd;
    }))

/* LH: Load Halfword */
RVOP(
    lh,
    {
        const uint32_t addr = rv->X[ir->rs1] + ir->imm;
        RV_EXC_MISALIGN_HANDLER(1, load, false, 1);
        rv->X[ir->rd] = sign_extend_h(rv->io.mem_read_s(addr));
    },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld_sext, S16, RAX, RBX, 0;
        st, S32, RBX, X, rd;
    }))

/* LW: Load Word */
RVOP(
    lw,
    {
        const uint32_t addr = rv->X[ir->rs1] + ir->imm;
        RV_EXC_MISALIGN_HANDLER(3, load, false, 1);
        rv->X[ir->rd] = rv->io.mem_read_w(addr);
    },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S32, RAX, RBX, 0;
        st, S32, RBX, X, rd;
    }))

/* LBU: Load Byte Unsigned */
RVOP(
    lbu,
    { rv->X[ir->rd] = rv->io.mem_read_b(rv->X[ir->rs1] + ir->imm); },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S8, RAX, RBX, 0;
        st, S32, RBX, X, rd;
    }))

/* LHU: Load Halfword Unsigned */
RVOP(
    lhu,
    {
        const uint32_t addr = rv->X[ir->rs1] + ir->imm;
        RV_EXC_MISALIGN_HANDLER(1, load, false, 1);
        rv->X[ir->rd] = rv->io.mem_read_s(addr);
    },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S16, RAX, RBX, 0;
        st, S32, RBX, X, rd;
    }))

/* There are 3 types of stores: byte, halfword, and word-sized. Unlike loads,
 * there are no signed or unsigned variants, as stores to memory write exactly
 * the number of bytes specified, and there is no sign or zero extension
 * involved.
 */

/* SB: Store Byte */
RVOP(
    sb,
    { rv->io.mem_write_b(rv->X[ir->rs1] + ir->imm, rv->X[ir->rs2]); },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S8, RBX, X, rs2;
        st, S8, RBX, RAX, 0;
    }))

/* SH: Store Halfword */
RVOP(
    sh,
    {
        const uint32_t addr = rv->X[ir->rs1] + ir->imm;
        RV_EXC_MISALIGN_HANDLER(1, store, false, 1);
        rv->io.mem_write_s(addr, rv->X[ir->rs2]);
    },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S16, RBX, X, rs2;
        st, S16, RBX, RAX, 0;
    }))

/* SW: Store Word */
RVOP(
    sw,
    {
        const uint32_t addr = rv->X[ir->rs1] + ir->imm;
        RV_EXC_MISALIGN_HANDLER(3, store, false, 1);
        rv->io.mem_write_w(addr, rv->X[ir->rs2]);
    },
    X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S32, RBX, X, rs2;
        st, S32, RBX, RAX, 0;
    }))

/* ADDI adds the sign-extended 12-bit immediate to register rs1. Arithmetic
 * overflow is ignored and the result is simply the low XLEN bits of the
 * result. ADDI rd, rs1, 0 is used to implement the MV rd, rs1 assembler
 * pseudo-instruction.
 */
RVOP(
    addi,
    { rv->X[ir->rd] = (int32_t) (rv->X[ir->rs1]) + ir->imm; },
    X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 32, 0x81, 0, RAX, imm;
        st, S32, RAX, X, rd;
    }))

/* SLTI place the value 1 in register rd if register rs1 is less than the
 * signextended immediate when both are treated as signed numbers, else 0 is
 * written to rd.
 */
RVOP(
    slti,
    { rv->X[ir->rd] = ((int32_t) (rv->X[ir->rs1]) < ir->imm) ? 1 : 0; },
    X64({
        ld, S32, RAX, X, rs1;
        cmp_imm, RAX, imm;
        st_imm, S32, rd, 1;
        set_jmp_off;
        jcc, 0x82;
        st_imm, S32, rd, 0;
        jmp_off;
    }))

/* SLTIU places the value 1 in register rd if register rs1 is less than the
 * immediate when both are treated as unsigned numbers, else 0 is written to rd.
 */
RVOP(
    sltiu,
    { rv->X[ir->rd] = (rv->X[ir->rs1] < (uint32_t) ir->imm) ? 1 : 0; },
    X64({
        ld, S32, RAX, X, rs1;
        cmp_imm, RAX, imm;
        st_imm, S32, rd, 1;
        set_jmp_off;
        jcc, 0x82;
        st_imm, S32, rd, 0;
        jmp_off;
    }))

/* XORI: Exclusive OR Immediate */
RVOP(
    xori,
    { rv->X[ir->rd] = rv->X[ir->rs1] ^ ir->imm; },
    X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 32, 0x81, 6, RAX, imm;
        st, S32, RAX, X, rd;
    }))

/* ORI: OR Immediate */
RVOP(
    ori,
    { rv->X[ir->rd] = rv->X[ir->rs1] | ir->imm; },
    X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 32, 0x81, 1, RAX, imm;
        st, S32, RAX, X, rd;
    }))

/* ANDI performs bitwise AND on register rs1 and the sign-extended 12-bit
 * immediate and place the result in rd.
 */
RVOP(
    andi,
    { rv->X[ir->rd] = rv->X[ir->rs1] & ir->imm; },
    X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 32, 0x81, 4, RAX, imm;
        st, S32, RAX, X, rd;
    }))

FORCE_INLINE void shift_func(riscv_t *rv, const rv_insn_t *ir)
{
    switch (ir->opcode) {
    case rv_insn_slli:
        rv->X[ir->rd] = rv->X[ir->rs1] << (ir->imm & 0x1f);
        break;
    case rv_insn_srli:
        rv->X[ir->rd] = rv->X[ir->rs1] >> (ir->imm & 0x1f);
        break;
    case rv_insn_srai:
        rv->X[ir->rd] = ((int32_t) rv->X[ir->rs1]) >> (ir->imm & 0x1f);
        break;
    default:
        __UNREACHABLE;
        break;
    }
};

/* SLLI performs logical left shift on the value in register rs1 by the shift
 * amount held in the lower 5 bits of the immediate.
 */
RVOP(
    slli,
    { shift_func(rv, ir); },
    X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 8, 0xc1, 4, RAX, imm, 0x1f;
        st, S32, RAX, X, rd;
    }))

/* SRLI performs logical right shift on the value in register rs1 by the shift
 * amount held in the lower 5 bits of the immediate.
 */
RVOP(
    srli,
    { shift_func(rv, ir); },
    X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 8, 0xc1, 5, RAX, imm, 0x1f;
        st, S32, RAX, X, rd;
    }))

/* SRAI performs arithmetic right shift on the value in register rs1 by the
 * shift amount held in the lower 5 bits of the immediate.
 */
RVOP(
    srai,
    { shift_func(rv, ir); },
    X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 8, 0xc1, 7, RAX, imm, 0x1f;
        st, S32, RAX, X, rd;
    }))

/* ADD */
RVOP(
    add,
    {
        rv->X[ir->rd] = (int32_t) (rv->X[ir->rs1]) + (int32_t) (rv->X[ir->rs2]);
    },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x01, RBX, RAX;
        st, S32, RAX, X, rd;
    }))

/* SUB: Substract */
RVOP(
    sub,
    {
        rv->X[ir->rd] = (int32_t) (rv->X[ir->rs1]) - (int32_t) (rv->X[ir->rs2]);
    },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x29, RBX, RAX;
        st, S32, RAX, X, rd;
    }))

/* SLL: Shift Left Logical */
RVOP(
    sll,
    { rv->X[ir->rd] = rv->X[ir->rs1] << (rv->X[ir->rs2] & 0x1f); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RCX, X, rs2;
        alu32_imm, 32, 0x81, 4, RCX, 0x1f;
        alu32, 0xd3, 4, RAX;
        st, S32, RAX, X, rd;
    }))

/* SLT: Set on Less Than */
RVOP(
    slt,
    {
        rv->X[ir->rd] =
            ((int32_t) (rv->X[ir->rs1]) < (int32_t) (rv->X[ir->rs2])) ? 1 : 0;
    },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        st_imm, S32, rd, 1;
        set_jmp_off;
        jcc, 0x82;
        st_imm, S32, rd, 0;
        jmp_off;
    }))

/* SLTU: Set on Less Than Unsigned */
RVOP(
    sltu,
    { rv->X[ir->rd] = (rv->X[ir->rs1] < rv->X[ir->rs2]) ? 1 : 0; },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        cmp, RBX, RAX;
        st_imm, S32, rd, 1;
        set_jmp_off;
        jcc, 0x82;
        st_imm, S32, rd, 0;
        jmp_off;
    }))

/* XOR: Exclusive OR */
RVOP(
    xor,
    {
      rv->X[ir->rd] = rv->X[ir->rs1] ^ rv->X[ir->rs2];
    },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x31, RBX, RAX;
        st, S32, RAX, X, rd;
    }))

/* SRL: Shift Right Logical */
RVOP(
    srl,
    { rv->X[ir->rd] = rv->X[ir->rs1] >> (rv->X[ir->rs2] & 0x1f); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RCX, X, rs2;
        alu32_imm, 32, 0x81, 4, RCX, 0x1f;
        alu32, 0xd3, 5, RAX;
        st, S32, RAX, X, rd;
    }))

/* SRA: Shift Right Arithmetic */
RVOP(
    sra,
    { rv->X[ir->rd] = ((int32_t) rv->X[ir->rs1]) >> (rv->X[ir->rs2] & 0x1f); },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RCX, X, rs2;
        alu32_imm, 32, 0x81, 4, RCX, 0x1f;
        alu32, 0xd3, 7, RAX;
        st, S32, RAX, X, rd;
    }))

/* OR */
RVOP(
    or
    ,
    { rv->X[ir->rd] = rv->X[ir->rs1] | rv->X[ir->rs2]; },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x09, RBX, RAX;
        st, S32, RAX, X, rd;
    }))

/* AND */
RVOP(
    and,
    { rv->X[ir->rd] = rv->X[ir->rs1] & rv->X[ir->rs2]; },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x21, RBX, RAX;
        st, S32, RAX, X, rd;
    }))

/* ECALL: Environment Call */
RVOP(
    ecall,
    {
        rv->compressed = false;
        rv->csr_cycle = cycle;
        rv->PC = PC;
        rv->io.on_ecall(rv);
        return true;
    },
    X64({
        ld_imm, RAX, pc;
        st, S32, RAX, PC;
        call, ecall;
        exit;
    }))

/* EBREAK: Environment Break */
RVOP(
    ebreak,
    {
        rv->compressed = false;
        rv->csr_cycle = cycle;
        rv->PC = PC;
        rv->io.on_ebreak(rv);
        return true;
    },
    X64({
        ld_imm, RAX, pc;
        st, S32, RAX, PC;
        call, ebreak;
        exit;
    }))

/* WFI: Wait for Interrupt */
RVOP(
    wfi,
    {
        /* FIXME: Implement */
        return false;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* URET: return from traps in U-mode */
RVOP(
    uret,
    {
        /* FIXME: Implement */
        return false;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* SRET: return from traps in S-mode */
RVOP(
    sret,
    {
        /* FIXME: Implement */
        return false;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* HRET: return from traps in H-mode */
RVOP(
    hret,
    {
        /* FIXME: Implement */
        return false;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* MRET: return from traps in U-mode */
RVOP(
    mret,
    {
        rv->csr_mstatus = MSTATUS_MPIE;
        rv->PC = rv->csr_mepc;
        return true;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

#if RV32_HAS(Zifencei) /* RV32 Zifencei Standard Extension */
RVOP(
    fencei,
    {
        PC += 4;
        /* FIXME: fill real implementations */
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
    },
    X64({
        assert; /* FIXME: Implement */
    }))
#endif

#if RV32_HAS(Zicsr) /* RV32 Zicsr Standard Extension */
/* CSRRW: Atomic Read/Write CSR */
RVOP(
    csrrw,
    {
        uint32_t tmp = csr_csrrw(rv, ir->imm, rv->X[ir->rs1]);
        rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* CSRRS: Atomic Read and Set Bits in CSR */
/* The initial value in integer register rs1 is treated as a bit mask that
 * specifies the bit positions to be set in the CSR. Any bit that is set in
 * rs1 will result in the corresponding bit being set in the CSR, provided
 * that the CSR bit is writable. Other bits in the CSR remain unaffected,
 * although some CSRs might exhibit side effects when written to.
 *
 * See Page 56 of the RISC-V Unprivileged Specification.
 */
RVOP(
    csrrs,
    {
        uint32_t tmp = csr_csrrs(
            rv, ir->imm, (ir->rs1 == rv_reg_zero) ? 0U : rv->X[ir->rs1]);
        rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* CSRRC: Atomic Read and Clear Bits in CSR */
RVOP(
    csrrc,
    {
        uint32_t tmp = csr_csrrc(
            rv, ir->imm, (ir->rs1 == rv_reg_zero) ? ~0U : rv->X[ir->rs1]);
        rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* CSRRWI */
RVOP(
    csrrwi,
    {
        uint32_t tmp = csr_csrrw(rv, ir->imm, ir->rs1);
        rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* CSRRSI */
RVOP(
    csrrsi,
    {
        uint32_t tmp = csr_csrrs(rv, ir->imm, ir->rs1);
        rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* CSRRCI */
RVOP(
    csrrci,
    {
        uint32_t tmp = csr_csrrc(rv, ir->imm, ir->rs1);
        rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
    },
    X64({
        assert; /* FIXME: Implement */
    }))
#endif

/* RV32M Standard Extension */

#if RV32_HAS(EXT_M)
/* MUL: Multiply */
RVOP(
    mul,
    { rv->X[ir->rd] = (int32_t) rv->X[ir->rs1] * (int32_t) rv->X[ir->rs2]; },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        mul, 0x28, RBX, RAX, 0;
        st, S32, RAX, X, rd;
    }))

/* MULH: Multiply High Signed Signed */
/* It is important to first cast rs1 and rs2 to i32 so that the subsequent
 * cast to i64 sign-extends the register values.
 */
RVOP(
    mulh,
    {
        const int64_t multiplicand = (int32_t) rv->X[ir->rs1];
        const int64_t multiplier = (int32_t) rv->X[ir->rs2];
        rv->X[ir->rd] = ((uint64_t) (multiplicand * multiplier)) >> 32;
    },
    X64({
        ld_sext, S32, RAX, X, rs1;
        ld_sext, S32, RBX, X, rs2;
        mul, 0x2f, RBX, RAX, 0;
        alu64_imm, 8, 0xc1, 5, RAX, 32;
        st, S32, RAX, X, rd;
    }))

/* MULHSU: Multiply High Signed Unsigned */
/* It is essential to perform an initial cast of rs1 to i32, ensuring that the
 * subsequent cast to i64 results in sign extension of the register value.
 * Additionally, rs2 should not undergo sign extension.
 */
RVOP(
    mulhsu,
    {
        const int64_t multiplicand = (int32_t) rv->X[ir->rs1];
        const uint64_t umultiplier = rv->X[ir->rs2];
        rv->X[ir->rd] = ((uint64_t) (multiplicand * umultiplier)) >> 32;
    },
    X64({
        ld_sext, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        mul, 0x2f, RBX, RAX, 0;
        alu64_imm, 8, 0xc1, 5, RAX, 32;
        st, S32, RAX, X, rd;
    }))

/* MULHU: Multiply High Unsigned Unsigned */
RVOP(
    mulhu,
    {
        rv->X[ir->rd] =
            ((uint64_t) rv->X[ir->rs1] * (uint64_t) rv->X[ir->rs2]) >> 32;
    },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        mul, 0x2f, RBX, RAX, 0;
        alu64_imm, 8, 0xc1, 5, RAX, 32;
        st, S32, RAX, X, rd;
    }))

/* DIV: Divide Signed */
/* +------------------------+-----------+----------+-----------+
 * |       Condition        |  Dividend |  Divisor |   DIV[W]  |
 * +------------------------+-----------+----------+-----------+
 * | Division by zero       |  x        |  0       |  −1       |
 * | Overflow (signed only) |  −2^{L−1} |  −1      |  −2^{L−1} |
 * +------------------------+-----------+----------+-----------+
 */
RVOP(
    div,
    {
        const int32_t dividend = (int32_t) rv->X[ir->rs1];
        const int32_t divisor = (int32_t) rv->X[ir->rs2];
        rv->X[ir->rd] = !divisor ? ~0U
                        : (divisor == -1 && rv->X[ir->rs1] == 0x80000000U)
                            ? rv->X[ir->rs1] /* overflow */
                            : (unsigned int) (dividend / divisor);
    },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        div, 0x38, RBX, RAX, 0;
        cmp_imm, RBX, 0;
        set_jmp_off;
        jcc, 0x85;
        ld_imm, RAX, -1;
        jmp_off;
        st, S32, RAX, X, rd;
        /* FIXME: handle overflow */
    }))

/* DIVU: Divide Unsigned */
/* +------------------------+-----------+----------+----------+
 * |       Condition        |  Dividend |  Divisor |  DIVU[W] |
 * +------------------------+-----------+----------+----------+
 * | Division by zero       |  x        |  0       |  2^L − 1 |
 * +------------------------+-----------+----------+----------+
 */
RVOP(
    divu,
    {
        const uint32_t udividend = rv->X[ir->rs1];
        const uint32_t udivisor = rv->X[ir->rs2];
        rv->X[ir->rd] = !udivisor ? ~0U : udividend / udivisor;
    },
    X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        div, 0x38, RBX, RAX, 0;
        cmp_imm, RBX, 0;
        set_jmp_off;
        jcc, 0x85;
        ld_imm, RAX, ~0U;
        jmp_off;
        st, S32, RAX, X, rd;
    }))

/* clang-format off */
/* REM: Remainder Signed */
/* +------------------------+-----------+----------+---------+
 * |       Condition        |  Dividend |  Divisor |  REM[W] |
 * +------------------------+-----------+----------+---------+
 * | Division by zero       |  x        |  0       |  x      |
 * | Overflow (signed only) |  −2^{L−1} |  −1      |  0      |
 * +------------------------+-----------+----------+---------+
 */
RVOP(rem, {
    const int32_t dividend = rv->X[ir->rs1];
    const int32_t divisor = rv->X[ir->rs2];
    rv->X[ir->rd] = !divisor ? dividend
                    : (divisor == -1 && rv->X[ir->rs1] == 0x80000000U)
                        ? 0  : (dividend 
                        % divisor);
}, 
X64({
    ld, S32, RAX, X, rs1;
    ld, S32, RBX, X, rs2;
    mod, 0x98, RBX, RAX, 0;
    st, S32, RAX, X, rd; 
    /* FIXME: handle overflow */
}))

/* REMU: Remainder Unsigned */
/* +------------------------+-----------+----------+----------+
 * |       Condition        |  Dividend |  Divisor |  REMU[W] |
 * +------------------------+-----------+----------+----------+
 * | Division by zero       |  x        |  0       |  x       |
 * +------------------------+-----------+----------+----------+
 */
RVOP(remu, {
    const uint32_t udividend = rv->X[ir->rs1];
    const uint32_t udivisor = rv->X[ir->rs2];
    rv->X[ir->rd] = !udivisor ? udividend : udividend 
    % udivisor;
}, 
X64({
    ld, S32, RAX, X, rs1;
    ld, S32, RBX, X, rs2;
    mod, 0x98, RBX, RAX, 0;
    st, S32, RAX, X, rd; 
}))
/* clang-format on */
#endif

/* RV32A Standard Extension */

#if RV32_HAS(EXT_A)
/* The Atomic Memory Operation (AMO) instructions execute read-modify-write
 * operations to synchronize multiple processors and are encoded in an R-type
 * instruction format.
 *
 * These AMO instructions guarantee atomicity when loading a data value from
 * the memory address stored in the register rs1. The loaded value is then
 * transferred to the register rd, where a binary operator is applied to this
 * value and the original value stored in the register rs2. Finally, the
 * resulting value is stored back to the memory address in rs1, ensuring
 * atomicity.
 *
 * AMOs support the manipulation of 64-bit words exclusively in RV64, whereas
 * both 64-bit and 32-bit words can be manipulated in other systems. In RV64,
 * when performing 32-bit AMOs, the value placed in the register rd is always
 * sign-extended.
 *
 * At present, AMO is not implemented atomically because the emulated RISC-V
 * core just runs on single thread, and no out-of-order execution happens.
 * In addition, rl/aq are not handled.
 */

/* LR.W: Load Reserved */
RVOP(
    lrw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(rv->X[ir->rs1]);
        /* skip registration of the 'reservation set'
         * FIXME: uimplemented
         */
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* SC.W: Store Conditional */
RVOP(
    scw,
    {
        /* assume the 'reservation set' is valid
         * FIXME: unimplemented
         */
        rv->io.mem_write_w(rv->X[ir->rs1], rv->X[ir->rs2]);
        rv->X[ir->rd] = 0;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOSWAP.W: Atomic Swap */
RVOP(
    amoswapw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        rv->io.mem_write_s(ir->rs1, rv->X[ir->rs2]);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOADD.W: Atomic ADD */
RVOP(
    amoaddw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const int32_t res = (int32_t) rv->X[ir->rd] + (int32_t) rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, res);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOXOR.W: Atomic XOR */
RVOP(
    amoxorw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const int32_t res = rv->X[ir->rd] ^ rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, res);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOAND.W: Atomic AND */
RVOP(
    amoandw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const int32_t res = rv->X[ir->rd] & rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, res);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOOR.W: Atomic OR */
RVOP(
    amoorw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const int32_t res = rv->X[ir->rd] | rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, res);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOMIN.W: Atomic MIN */
RVOP(
    amominw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const int32_t res =
            rv->X[ir->rd] < rv->X[ir->rs2] ? rv->X[ir->rd] : rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, res);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOMAX.W: Atomic MAX */
RVOP(
    amomaxw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const int32_t res =
            rv->X[ir->rd] > rv->X[ir->rs2] ? rv->X[ir->rd] : rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, res);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOMINU.W */
RVOP(
    amominuw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const uint32_t ures =
            rv->X[ir->rd] < rv->X[ir->rs2] ? rv->X[ir->rd] : rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, ures);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* AMOMAXU.W */
RVOP(
    amomaxuw,
    {
        rv->X[ir->rd] = rv->io.mem_read_w(ir->rs1);
        const uint32_t ures =
            rv->X[ir->rd] > rv->X[ir->rs2] ? rv->X[ir->rd] : rv->X[ir->rs2];
        rv->io.mem_write_s(ir->rs1, ures);
    },
    X64({
        assert; /* FIXME: Implement */
    }))
#endif /* RV32_HAS(EXT_A) */

/* RV32F Standard Extension */

#if RV32_HAS(EXT_F)
/* FLW */
RVOP(
    flw,
    {
        /* copy into the float register */
        const uint32_t data = rv->io.mem_read_w(rv->X[ir->rs1] + ir->imm);
        rv->F[ir->rd].v = data;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FSW */
RVOP(
    fsw,
    {
        /* copy from float registers */
        uint32_t data = rv->F[ir->rs2].v;
        rv->io.mem_write_w(rv->X[ir->rs1] + ir->imm, data);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FMADD.S */
RVOP(
    fmadds,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] =
            f32_mulAdd(rv->F[ir->rs1], rv->F[ir->rs2], rv->F[ir->rs3]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FMSUB.S */
RVOP(
    fmsubs,
    {
        set_rounding_mode(rv);
        riscv_float_t tmp = rv->F[ir->rs3];
        tmp.v ^= FMASK_SIGN;
        rv->F[ir->rd] = f32_mulAdd(rv->F[ir->rs1], rv->F[ir->rs2], tmp);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FNMSUB.S */
RVOP(
    fnmsubs,
    {
        set_rounding_mode(rv);
        riscv_float_t tmp = rv->F[ir->rs1];
        tmp.v ^= FMASK_SIGN;
        rv->F[ir->rd] = f32_mulAdd(tmp, rv->F[ir->rs2], rv->F[ir->rs3]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FNMADD.S */
RVOP(
    fnmadds,
    {
        set_rounding_mode(rv);
        riscv_float_t tmp1 = rv->F[ir->rs1];
        riscv_float_t tmp2 = rv->F[ir->rs3];
        tmp1.v ^= FMASK_SIGN;
        tmp2.v ^= FMASK_SIGN;
        rv->F[ir->rd] = f32_mulAdd(tmp1, rv->F[ir->rs2], tmp2);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FADD.S */
RVOP(
    fadds,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] = f32_add(rv->F[ir->rs1], rv->F[ir->rs2]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FSUB.S */
RVOP(
    fsubs,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] = f32_sub(rv->F[ir->rs1], rv->F[ir->rs2]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FMUL.S */
RVOP(
    fmuls,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] = f32_mul(rv->F[ir->rs1], rv->F[ir->rs2]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FDIV.S */
RVOP(
    fdivs,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] = f32_div(rv->F[ir->rs1], rv->F[ir->rs2]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FSQRT.S */
RVOP(
    fsqrts,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] = f32_sqrt(rv->F[ir->rs1]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FSGNJ.S */
RVOP(
    fsgnjs,
    {
        rv->F[ir->rd].v =
            (rv->F[ir->rs1].v & ~FMASK_SIGN) | (rv->F[ir->rs2].v & FMASK_SIGN);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FSGNJN.S */
RVOP(
    fsgnjns,
    {
        rv->F[ir->rd].v =
            (rv->F[ir->rs1].v & ~FMASK_SIGN) | (~rv->F[ir->rs2].v & FMASK_SIGN);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FSGNJX.S */
RVOP(
    fsgnjxs,
    { rv->F[ir->rd].v = rv->F[ir->rs1].v ^ (rv->F[ir->rs2].v & FMASK_SIGN); },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FMIN.S
 * In IEEE754-201x, fmin(x, y) return
 * - min(x,y) if both numbers are not NaN
 * - if one is NaN and another is a number, return the number
 * - if both are NaN, return NaN
 * When input is signaling NaN, raise invalid operation
 */
RVOP(
    fmins,
    {
        if (f32_isSignalingNaN(rv->F[ir->rs1]) ||
            f32_isSignalingNaN(rv->F[ir->rs2]))
            rv->csr_fcsr |= FFLAG_INVALID_OP;
        bool less = f32_lt_quiet(rv->F[ir->rs1], rv->F[ir->rs2]) ||
                    (f32_eq(rv->F[ir->rs1], rv->F[ir->rs2]) &&
                     (rv->F[ir->rs1].v & FMASK_SIGN));
        if (is_nan(rv->F[ir->rs1].v) && is_nan(rv->F[ir->rs2].v))
            rv->F[ir->rd].v = RV_NAN;
        else
            rv->F[ir->rd] = (less || is_nan(rv->F[ir->rs2].v) ? rv->F[ir->rs1]
                                                              : rv->F[ir->rs2]);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FMAX.S */
RVOP(
    fmaxs,
    {
        if (f32_isSignalingNaN(rv->F[ir->rs1]) ||
            f32_isSignalingNaN(rv->F[ir->rs2]))
            rv->csr_fcsr |= FFLAG_INVALID_OP;
        bool greater = f32_lt_quiet(rv->F[ir->rs2], rv->F[ir->rs1]) ||
                       (f32_eq(rv->F[ir->rs1], rv->F[ir->rs2]) &&
                        (rv->F[ir->rs2].v & FMASK_SIGN));
        if (is_nan(rv->F[ir->rs1].v) && is_nan(rv->F[ir->rs2].v))
            rv->F[ir->rd].v = RV_NAN;
        else
            rv->F[ir->rd] =
                (greater || is_nan(rv->F[ir->rs2].v) ? rv->F[ir->rs1]
                                                     : rv->F[ir->rs2]);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FCVT.W.S and FCVT.WU.S convert a floating point number to an integer,
 * the rounding mode is specified in rm field.
 */

/* FCVT.W.S */
RVOP(
    fcvtws,
    {
        set_rounding_mode(rv);
        uint32_t ret = f32_to_i32(rv->F[ir->rs1], softfloat_roundingMode, true);
        if (ir->rd)
            rv->X[ir->rd] = ret;
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FCVT.WU.S */
RVOP(
    fcvtwus,
    {
        set_rounding_mode(rv);
        uint32_t ret =
            f32_to_ui32(rv->F[ir->rs1], softfloat_roundingMode, true);
        if (ir->rd)
            rv->X[ir->rd] = ret;
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FMV.X.W */
RVOP(
    fmvxw,
    {
        if (ir->rd)
            rv->X[ir->rd] = rv->F[ir->rs1].v;
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FEQ.S performs a quiet comparison: it only sets the invalid operation
 * exception flag if either input is a signaling NaN.
 */
RVOP(
    feqs,
    {
        set_rounding_mode(rv);
        uint32_t ret = f32_eq(rv->F[ir->rs1], rv->F[ir->rs2]);
        if (ir->rd)
            rv->X[ir->rd] = ret;
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FLT.S and FLE.S perform what the IEEE 754-2008 standard refers to as
 * signaling comparisons: that is, they set the invalid operation exception
 * flag if either input is NaN.
 */
RVOP(
    flts,
    {
        set_rounding_mode(rv);
        uint32_t ret = f32_lt(rv->F[ir->rs1], rv->F[ir->rs2]);
        if (ir->rd)
            rv->X[ir->rd] = ret;
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

RVOP(
    fles,
    {
        set_rounding_mode(rv);
        uint32_t ret = f32_le(rv->F[ir->rs1], rv->F[ir->rs2]);
        if (ir->rd)
            rv->X[ir->rd] = ret;
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FCLASS.S */
RVOP(
    fclasss,
    {
        if (ir->rd)
            rv->X[ir->rd] = calc_fclass(rv->F[ir->rs1].v);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FCVT.S.W */
RVOP(
    fcvtsw,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] = i32_to_f32(rv->X[ir->rs1]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FCVT.S.WU */
RVOP(
    fcvtswu,
    {
        set_rounding_mode(rv);
        rv->F[ir->rd] = ui32_to_f32(rv->X[ir->rs1]);
        set_fflag(rv);
    },
    X64({
        assert; /* FIXME: Implement */
    }))

/* FMV.W.X */
RVOP(fmvwx,
     {
    rv->F[ir->rd].v = rv->X[ir->rs1]; },
     {

         X64({
        assert; /* FIXME: Implement */
         }))
#endif

    /* RV32C Standard Extension */

#if RV32_HAS(EXT_C)
/* C.ADDI4SPN is a CIW-format instruction that adds a zero-extended non-zero
 * immediate, scaledby 4, to the stack pointer, x2, and writes the result to
 * rd'.
 * This instruction is used to generate pointers to stack-allocated variables,
 * and expands to addi rd', x2, nzuimm[9:2].
 */
RVOP(caddi4spn,
     {
        rv->X[ir->rd] = rv->X[rv_reg_sp] + (uint16_t) ir->imm; },
     X64({
        ld, S32, RAX, X, rv_reg_sp;
        alu32_imm, 32, 0x81, 0, RAX, uint, 16, imm;
        st, S32, RAX, X, rd;
     }))

/* C.LW loads a 32-bit value from memory into register rd'. It computes an
 * effective address by adding the zero-extended offset, scaled by 4, to the
 * base address in register rs1'. It expands to lw rd', offset[6:2](rs1').
 */
RVOP(clw,
     {
        const uint32_t addr = rv->X[ir->rs1] + (uint32_t) ir->imm;
        RV_EXC_MISALIGN_HANDLER(3, load, true, 1);
        rv->X[ir->rd] = rv->io.mem_read_w(addr);
     },
     X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S32, RAX, RBX, 0;
        st, S32, RBX, X, rd;
     }))

/* C.SW stores a 32-bit value in register rs2' to memory. It computes an
 * effective address by adding the zero-extended offset, scaled by 4, to the
 * base address in register rs1'.
 * It expands to sw rs2', offset[6:2](rs1').
 */
RVOP(csw,
     {
        const uint32_t addr = rv->X[ir->rs1] + (uint32_t) ir->imm;
        RV_EXC_MISALIGN_HANDLER(3, store, true, 1);
        rv->io.mem_write_w(addr, rv->X[ir->rs2]);
     },
     X64({
        mem;
        ld, S32, RAX, X, rs1;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S32, RBX, X, rs2;
        st, S32, RBX, RAX, 0;
     }))

/* C.NOP */
RVOP(cnop, {/* no operation */}, X64({/* no operation */}))

/* C.ADDI adds the non-zero sign-extended 6-bit immediate to the value in
 * register rd then writes the result to rd. C.ADDI expands into
 * addi rd, rd, nzimm[5:0]. C.ADDI is only valid when rd'=x0. The code point
 * with both rd=x0 and nzimm=0 encodes the C.NOP instruction; the remaining
 * code points with either rd=x0 or nzimm=0 encode HINTs.
 */
RVOP(caddi, {
        rv->X[ir->rd] += (int16_t) ir->imm; }, X64({
        ld, S32, RAX, X, rd;
        alu32_imm, 32, 0x81, 0, RAX, int, 16, imm;
        st, S32, RAX, X, rd;
     }))

/* C.JAL */
RVOP(cjal,
     {
        rv->X[rv_reg_ra] = PC + 2;
        PC += ir->imm;
        RV_EXC_MISALIGN_HANDLER(PC, insn, true, 0);
        struct rv_insn *taken = ir->branch_taken;
        if (taken) {
#if RV32_HAS(JIT)
            block_t *block = cache_get(rv->block_cache, PC);
            if (!block) {
                ir->branch_taken = NULL;
                goto end_insn;
            }
            if (cache_hot(rv->block_cache, PC))
                goto end_insn;
#endif
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    end_insn:
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
     },
     X64({
        ld_imm, RAX, pc, 2;
        st, S32, RAX, X, rv_reg_ra;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        jmp, pc, imm;
        exit;
     }))

/* C.LI loads the sign-extended 6-bit immediate, imm, into register rd.
 * C.LI expands into addi rd, x0, imm[5:0].
 * C.LI is only valid when rd=x0; the code points with rd=x0 encode HINTs.
 */
RVOP(cli, {
        rv->X[ir->rd] = ir->imm; }, X64({
        ld_imm, RAX, imm;
        st, S32, RAX, X, rd;
     }))

/* C.ADDI16SP is used to adjust the stack pointer in procedure prologues
 * and epilogues. It expands into addi x2, x2, nzimm[9:4].
 * C.ADDI16SP is only valid when nzimm'=0; the code point with nzimm=0 is
 * reserved.
 */
RVOP(caddi16sp, {
        rv->X[ir->rd] += ir->imm; }, X64({
        ld, S32, RAX, X, rd;
        alu32_imm, 32, 0x81, 0, RAX, imm;
        st, S32, RAX, X, rd;
     }))

/* C.LUI loads the non-zero 6-bit immediate field into bits 17–12 of the
 * destination register, clears the bottom 12 bits, and sign-extends bit
 * 17 into all higher bits of the destination.
 * C.LUI expands into lui rd, nzimm[17:12].
 * C.LUI is only valid when rd'={x0, x2}, and when the immediate is not equal
 * to zero.
 */
RVOP(clui, {
        rv->X[ir->rd] = ir->imm; }, X64({
        ld_imm, RAX, imm;
        st, S32, RAX, X, rd;
     }))

/* C.SRLI is a CB-format instruction that performs a logical right shift
 * of the value in register rd' then writes the result to rd'. The shift
 * amount is encoded in the shamt field. C.SRLI expands into srli rd',
 * rd', shamt[5:0].
 */
RVOP(csrli, {
        rv->X[ir->rs1] >>= ir->shamt; }, X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 8, 0xc1, 5, RAX, shamt;
        st, S32, RAX, X, rs1;
     }))

/* C.SRAI is defined analogously to C.SRLI, but instead performs an
 * arithmetic right shift. C.SRAI expands to srai rd', rd', shamt[5:0].
 */
RVOP(csrai,
     {
        const uint32_t mask = 0x80000000 & rv->X[ir->rs1];
        rv->X[ir->rs1] >>= ir->shamt;
        for (unsigned int i = 0; i < ir->shamt; ++i)
            rv->X[ir->rs1] |= mask >> i;
     },
     X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 8, 0xc1, 7, RAX, shamt;
        st, S32, RAX, X, rs1;
        /* FIXME: Incomplete */
     }))

/* C.ANDI is a CB-format instruction that computes the bitwise AND of the
 * value in register rd' and the sign-extended 6-bit immediate, then writes
 * the result to rd'. C.ANDI expands to andi rd', rd', imm[5:0].
 */
RVOP(candi, {
        rv->X[ir->rs1] &= ir->imm; }, X64({
        ld, S32, RAX, X, rs1;
        alu32_imm, 32, 0x81, 4, RAX, imm;
        st, S32, RAX, X, rs1;
     }))

/* C.SUB */
RVOP(csub, {
        rv->X[ir->rd] = rv->X[ir->rs1] - rv->X[ir->rs2]; }, X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x29, RBX, RAX;
        st, S32, RAX, X, rd;
     }))

/* C.XOR */
RVOP(cxor, {
        rv->X[ir->rd] = rv->X[ir->rs1] ^ rv->X[ir->rs2]; }, X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x31, RBX, RAX;
        st, S32, RAX, X, rd;
     }))

RVOP(cor, {
        rv->X[ir->rd] = rv->X[ir->rs1] | rv->X[ir->rs2]; }, X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x09, RBX, RAX;
        st, S32, RAX, X, rd;
     }))

RVOP(cand, {
        rv->X[ir->rd] = rv->X[ir->rs1] & rv->X[ir->rs2]; }, X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x21, RBX, RAX;
        st, S32, RAX, X, rd;
     }))

/* C.J performs an unconditional control transfer. The offset is sign-extended
 * and added to the pc to form the jump target address.
 * C.J can therefore target a ±2 KiB range.
 * C.J expands to jal x0, offset[11:1].
 */
RVOP(cj,
     {
        PC += ir->imm;
        RV_EXC_MISALIGN_HANDLER(PC, insn, true, 0);
        struct rv_insn *taken = ir->branch_taken;
        if (taken) {
#if RV32_HAS(JIT)
            block_t *block = cache_get(rv->block_cache, PC);
            if (!block) {
                ir->branch_taken = NULL;
                goto end_insn;
            }
            if (cache_hot(rv->block_cache, PC))
                goto end_insn;
#endif
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    end_insn:
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
     },
     X64({
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        jmp, pc, imm;
        exit;
     }))

/* C.BEQZ performs conditional control transfers. The offset is sign-extended
 * and added to the pc to form the branch target address.
 * It can therefore target a ±256 B range. C.BEQZ takes the branch if the
 * value in register rs1' is zero. It expands to beq rs1', x0, offset[8:1].
 */
RVOP(cbeqz,
     {
        if (rv->X[ir->rs1]) {
            is_branch_taken = false;
            struct rv_insn *untaken = ir->branch_untaken;
            if (!untaken)
                goto nextop;
#if RV32_HAS(JIT)
            block_t *block = cache_get(rv->block_cache, PC + 2);
            if (!block) {
                ir->branch_untaken = NULL;
                goto nextop;
            }
            untaken = ir->branch_untaken = block->ir_head;
            if (cache_hot(rv->block_cache, PC + 2))
                goto nextop;
#endif
            PC += 2;
            last_pc = PC;
            MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);
        }
        is_branch_taken = true;
        PC += ir->imm;
        struct rv_insn *taken = ir->branch_taken;
        if (taken) {
#if RV32_HAS(JIT)
            block_t *block = cache_get(rv->block_cache, PC);
            if (!block) {
                ir->branch_taken = NULL;
                goto end_insn;
            }
            if (cache_hot(rv->block_cache, PC))
                goto end_insn;
#endif
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    end_insn:
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
     },
     X64({
        ld, S32, RAX, X, rs1;
        cmp_imm, RAX, 0;
        set_jmp_off;
        jcc, 0x84;
        cond, branch_untaken;
        jmp, pc, 2;
        end;
        ld_imm, RAX, pc, 2;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
     }))

/* C.BEQZ */
RVOP(cbnez,
     {
        if (!rv->X[ir->rs1]) {
            is_branch_taken = false;
            struct rv_insn *untaken = ir->branch_untaken;
            if (!untaken)
                goto nextop;
#if RV32_HAS(JIT)
            block_t *block = cache_get(rv->block_cache, PC + 2);
            if (!block) {
                ir->branch_untaken = NULL;
                goto nextop;
            }
            untaken = ir->branch_untaken = block->ir_head;
            if (cache_hot(rv->block_cache, PC + 2))
                goto nextop;
#endif
            PC += 2;
            last_pc = PC;
            MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);
        }
        is_branch_taken = true;
        PC += ir->imm;
        struct rv_insn *taken = ir->branch_taken;
        if (taken) {
#if RV32_HAS(JIT)
            block_t *block = cache_get(rv->block_cache, PC);
            if (!block) {
                ir->branch_taken = NULL;
                goto end_insn;
            }
            if (cache_hot(rv->block_cache, PC))
                goto end_insn;
#endif
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    end_insn:
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
     },
     X64({
        ld, S32, RAX, X, rs1;
        cmp_imm, RAX, 0;
        set_jmp_off;
        jcc, 0x85;
        cond, branch_untaken;
        jmp, pc, 2;
        end;
        ld_imm, RAX, pc, 2;
        st, S32, RAX, PC;
        exit;
        jmp_off;
        cond, branch_taken;
        jmp, pc, imm;
        end;
        ld_imm, RAX, pc, imm;
        st, S32, RAX, PC;
        exit;
     }))

/* C.SLLI is a CI-format instruction that performs a logical left shift of
 * the value in register rd then writes the result to rd. The shift amount
 * is encoded in the shamt field. C.SLLI expands into slli rd, rd, shamt[5:0].
 */
RVOP(cslli, {
        rv->X[ir->rd] <<= (uint8_t) ir->imm; }, X64({
        ld, S32, RAX, X, rd;
        alu32_imm, 8, 0xc1, 4, RAX, uint, 8, imm;
        st, S32, RAX, X, rd;
     }))

/* C.LWSP */
RVOP(clwsp,
     {
        const uint32_t addr = rv->X[rv_reg_sp] + ir->imm;
        RV_EXC_MISALIGN_HANDLER(3, load, true, 1);
        rv->X[ir->rd] = rv->io.mem_read_w(addr);
     },
     X64({
        mem;
        ld, S32, RAX, X, rv_reg_sp;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S32, RAX, RBX, 0;
        st, S32, RBX, X, rd;
     }))

/* C.JR */
RVOP(cjr,
     {
        PC = rv->X[ir->rs1];
#if !RV32_HAS(JIT)
        LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE();
#endif
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
     },
     X64({
        ld, S32, RAX, X, rs1;
        st, S32, RAX, PC;
        exit;
     }))

/* C.MV */
RVOP(cmv, {
        rv->X[ir->rd] = rv->X[ir->rs2]; }, X64({
        ld, S32, RAX, X, rs2;
        st, S32, RAX, X, rd;
     }))

/* C.EBREAK */
RVOP(cebreak,
     {
        rv->compressed = true;
        rv->csr_cycle = cycle;
        rv->PC = PC;
        rv->io.on_ebreak(rv);
        return true;
     },
     X64({
        ld_imm, RAX, pc;
        st, S32, RAX, PC;
        ld_imm, RAX, 1;
        st, S32, RAX, compressed;
        call, ebreak;
        exit;
     }))

/* C.JALR */
RVOP(cjalr,
     {
        /* Unconditional jump and store PC+2 to ra */
        const int32_t jump_to = rv->X[ir->rs1];
        rv->X[rv_reg_ra] = PC + 2;
        PC = jump_to;
        RV_EXC_MISALIGN_HANDLER(PC, insn, true, 0);
#if !RV32_HAS(JIT)
        LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE();
#endif
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
     },
     X64({
        ld_imm, RAX, pc, 2;
        st, S32, RAX, X, rv_reg_ra;
        ld, S32, RAX, X, rs1;
        st, S32, RAX, PC;
        exit;
     }))

/* C.ADD adds the values in registers rd and rs2 and writes the result to
 * register rd.
 * C.ADD expands into add rd, rd, rs2.
 * C.ADD is only valid when rs2=x0; the code points with rs2=x0 correspond to
 * the C.JALR and C.EBREAK instructions. The code points with rs2=x0 and rd=x0
 * are HINTs.
 */
RVOP(cadd, {
        rv->X[ir->rd] = rv->X[ir->rs1] + rv->X[ir->rs2]; }, X64({
        ld, S32, RAX, X, rs1;
        ld, S32, RBX, X, rs2;
        alu32, 0x01, RBX, RAX;
        st, S32, RAX, X, rd;
     }))

/* C.SWSP */
RVOP(cswsp,
     {
        const uint32_t addr = rv->X[rv_reg_sp] + ir->imm;
        RV_EXC_MISALIGN_HANDLER(3, store, true, 1);
        rv->io.mem_write_w(addr, rv->X[ir->rs2]);
     },
     X64({
        mem;
        ld, S32, RAX, X, rv_reg_sp;
        ld_imm, RBX, mem;
        alu64, 0x01, RBX, RAX;
        ld, S32, RBX, X, rs2;
        st, S32, RBX, RAX, 0;
     }))
#endif
