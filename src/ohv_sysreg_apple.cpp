// The Apple private system registers, which no SDK header describes and the
// generator therefore cannot see.  Kept beside the generated table rather than
// inside it, so regenerating does not throw them away.
//
// The encodings are s3_<op1>_c15_<CRm>_<op2>, read off a live monitor by
// matching logged accesses against its own disassembly.  The indices are into
// ohv_extregs_t; the GL1 bank sits there in the order sp, tpidr, aspsr, vbar,
// far, esr, elr, spsr, which is not the order of its CRm encodings and is
//
// Every one of these is read back after the hardware has written it -- the GL1
// bank is filled by GENTER itself -- so each access syncs first; a stale mirror
// answers zero where the return address should be.
// confirmed by the context offsets those members are anchored to.
#include "ohv_internal.h"

namespace ohv {

#define EXT(n) 2, (n)

static const SysRegDesc kAppleTable[] = {
    /* SPRR: the permission remapping the guarded levels are described by. */
    {0xf788, EXT(55), OHV_STATE_SPRR, true},   /* SPRR_CONFIG_EL1  c1_0 */
    {0xf78d, EXT(58), OHV_STATE_SPRR, true},   /* SPRR_UPERM_EL0   c1_5 */
    {0xf78e, EXT(57), OHV_STATE_SPRR, true},   /* SPRR_PPERM_EL1   c1_6 */

    /* GXF: the guarded execution feature itself. */
    {0xf78a, EXT(42), OHV_STATE_GXF, true},    /* GXF_CONFIG_EL1   c1_2 */
    {0xf7c1, EXT(43), OHV_STATE_GXF, true},    /* GXF_ENTRY_EL1    c8_1 */
    {0xf7c2, EXT(44), OHV_STATE_GXF, true},    /* GXF_ABORT_EL1    c8_2 */

    /* The GL1 bank, s3_6_c15_c10_*. */
    {0xf7d0, EXT(45), OHV_STATE_GXF, true},    /* SP_GL1     c10_0 */
    {0xf7d1, EXT(46), OHV_STATE_GXF, true},    /* TPIDR_GL1  c10_1 */
    {0xf7d2, EXT(48), OHV_STATE_GXF, true},    /* VBAR_GL1   c10_2 */
    {0xf7d3, EXT(52), OHV_STATE_GXF, true},    /* SPSR_GL1   c10_3 */
    {0xf7d4, EXT(47), OHV_STATE_GXF, true},    /* ASPSR_GL1  c10_4 */
    {0xf7d5, EXT(50), OHV_STATE_GXF, true},    /* ESR_GL1    c10_5 */
    {0xf7d6, EXT(51), OHV_STATE_GXF, true},    /* ELR_GL1    c10_6 */
    {0xf7d7, EXT(49), OHV_STATE_GXF, true},    /* FAR_GL1    c10_7 */
};

#undef EXT

/*
 * The EL2 registers, and the Apple private ones the generator has no source
 * for either.  Every offset below was measured, not derived: each register
 * was written through the framework with a value nothing else would hold and
 * the private context searched for where it landed.
 *
 * Three groups come out of that.  Some are already inside a region this
 * library describes -- VPIDR_EL2 and VMPIDR_EL2 are run controls, the s3_6
 * encodings are extended registers -- and are named by index there.  Some sit
 * in a nested-EL2 block just past every region at 0x1000, and are named by
 * raw context offset.  The rest the framework accepts while the context shows
 * nothing at all: under NV2 those are the guest's own, reached through its
 * VNCR page, and they go to the shadow.
 */
#ifndef EXT
#define EXT(n) 2, (n)
#endif
#define RAW(off) 6, ((off) / 8)
#define SHADOW(n) 7, (n)

static const SysRegDesc kEl2Table[] = {
    /*
     * Run controls.  Named by raw offset like the block below, not by an
     * index into a region: there is no kind for the controls, and the two
     * kinds that take an index are the shared and banked system registers --
     * which is where these three landed, on top of registers that had nothing
     * to do with them.
     */
    {0xe000, RAW(0x948), OHV_STATE_CONTROLS, false},   /* VPIDR_EL2  */
    {0xe005, RAW(0x940), OHV_STATE_CONTROLS, false},   /* VMPIDR_EL2 */
    {0xe08a, RAW(0x930), OHV_STATE_CONTROLS, false},   /* CPTR_EL2   */
    {0xe089, RAW(0x938), OHV_STATE_CONTROLS, false},   /* MDCR_EL2   */

    /* the nested-EL2 block */
    {0xe088, RAW(0x1078), OHV_STATE_CONTROLS, false},  /* HCR_EL2     */
    {0xe108, RAW(0x1020), OHV_STATE_CONTROLS, false},  /* VTTBR_EL2   */
    {0xe10a, RAW(0x1040), OHV_STATE_CONTROLS, false},  /* VTCR_EL2    */
    {0xe703, RAW(0x1060), OHV_STATE_CONTROLS, false},  /* CNTVOFF_EL2 */
    {0xe682, RAW(0x1090), OHV_STATE_CONTROLS, false},  /* TPIDR_EL2   */

    /* Apple private, inside the extended registers */
    /*
     * These seven asked for a state the kernel does not have.
     *
     * ARM_GUEST_STATE_APPLE is bit sixty-three and its own header says "this
     * state is not currently used": nothing in hv_vcpu.c ever tests it.  So a
     * write here landed in the context, read straight back, and was never
     * carried into the silicon -- the same shape as the guarded bank's old
     * problem, and the same cause.
     *
     * What each one wants is the state whose loader carries it, and the
     * extended-register index says which that is:
     *
     *   29 apctl_el1, 40 kernkeyhi_el1, 41 kernkeylo_el1
     *        -> _hv_load_guest_apple_ptrauth_regs, ARM_GUEST_STATE_PTRAUTH_APPLE
     *   53 pmcr1_gl1, 54 afsr1_gl1
     *        -> _hv_load_guest_gxf_regs, ARM_GUEST_STATE_GXF
     *   56 sprr_amrange_el1, 59 sprr_pmprr_el1
     *        -> _hv_load_guest_sprr_regs, ARM_GUEST_STATE_SPRR
     *
     * The first three are what TXM dies on.  SPTM writes APCTL_EL1 ninety-three
     * times and KERNKEY once, TXM authenticates a pointer out of its own image
     * under the key that should have installed, and takes an FPAC with the
     * value intact and the signature wrong.  Measured with a probe: changing
     * the architected APIAKey under a signed pointer breaks its authentication,
     * and changing APCTL_EL1 or KERNKEY does not -- the signature came out
     * byte-identical every time, which is a key that never moved.
     */
    {0xe784, EXT(29), OHV_STATE_PTRAUTH_APPLE, true},   /* APCTL_EL1        */
    {0xe789, EXT(40), OHV_STATE_PTRAUTH_APPLE, true},   /* KERNKEYHI_EL1    */
    {0xe788, EXT(41), OHV_STATE_PTRAUTH_APPLE, true},   /* KERNKEYLO_EL1    */
    {0xf7c7, EXT(53), OHV_STATE_GXF, true},             /* PMCR1_GL1        */
    {0xf781, EXT(54), OHV_STATE_GXF, true},             /* AFSR1_GL1        */
    {0xf78b, EXT(56), OHV_STATE_SPRR, true},            /* SPRR_AMRANGE_EL1 */
    {0xf799, EXT(59), OHV_STATE_SPRR, true},            /* SPRR_PMPRR_EL1   */

    /*
     * The JITBox bank, which is where the B key really lives.
     *
     * XNU's thread_invoke reads JCTL_EL0 out of the outgoing thread and writes
     * the incoming one's value back on every switch, and JCTL is the control
     * whose extended registers are JAPIAKey and JAPIBKey.  Refused here, the
     * VMM's forwarding fell back to a register file of its own and none of it
     * reached the silicon: every core signed with whatever the last thing on
     * that physical core had left in the bank.  With one core running that was
     * invisible.  With six, a thread signs its return address on one core and
     * returns on another, and the kernel takes
     *
     *     PAC failure from kernel with IB key at pc ... (thread_invoke)
     *
     * on a RETAB whose pointer value is perfectly intact.
     *
     *   76 jrange_el1, 77 jctl_el1, 78/79 japiakeyhi/lo, 80/81 japibkeyhi/lo
     *        -> _hv_load_guest_jitbox_regs, ARM_GUEST_STATE_JITBOX
     *
     * JCTL_EL0 has no place of its own in the block -- the kernel restores
     * JCTL through JCTL_EL12 -- so it is not listed here and stays with the
     * VMM.
     */
    {0xe7f9, EXT(76), OHV_STATE_JITBOX, true},          /* JRANGE_EL1       */
    {0xe7fc, EXT(77), OHV_STATE_JITBOX, true},          /* JCTL_EL1         */
    {0xe7ed, EXT(78), OHV_STATE_JITBOX, true},          /* JAPIAKEYHI_EL1   */
    {0xe7ec, EXT(79), OHV_STATE_JITBOX, true},          /* JAPIAKEYLO_EL1   */
    {0xe7ef, EXT(80), OHV_STATE_JITBOX, true},          /* JAPIBKEYHI_EL1   */
    {0xe7ee, EXT(81), OHV_STATE_JITBOX, true},          /* JAPIBKEYLO_EL1   */

    /* accepted by the framework, held nowhere it can be seen */
    {0xe080, SHADOW(0),  0, false},   /* SCTLR_EL2   */
    {0xe100, SHADOW(2),  0, false},   /* TTBR0_EL2   */
    {0xe101, SHADOW(3),  0, false},   /* TTBR1_EL2   */
    {0xe102, SHADOW(4),  0, false},   /* TCR_EL2     */
    {0xe200, SHADOW(5),  0, false},   /* SPSR_EL2    */
    {0xe201, SHADOW(6),  0, false},   /* ELR_EL2     */
    {0xe290, SHADOW(7),  0, false},   /* ESR_EL2     */
    {0xe300, SHADOW(8),  0, false},   /* FAR_EL2     */
    {0xe304, SHADOW(9),  0, false},   /* HPFAR_EL2   */
    {0xe510, SHADOW(10), 0, false},   /* MAIR_EL2    */
    {0xe600, SHADOW(11), 0, false},   /* VBAR_EL2    */
    {0xe708, SHADOW(12), 0, false},   /* CNTHCTL_EL2 */
    {0xf208, SHADOW(13), 0, false},   /* s3_6_c4_c1_0 */
};

const SysRegDesc *sysreg_lookup_apple(uint16_t enc) {
    for (const SysRegDesc &d : kAppleTable) {
        if (d.encoding == enc) {
            return &d;
        }
    }
    for (const SysRegDesc &d : kEl2Table) {
        if (d.encoding == enc) {
            return &d;
        }
    }
    return nullptr;
}

} // namespace ohv
