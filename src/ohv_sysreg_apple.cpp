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

const SysRegDesc *sysreg_lookup_apple(uint16_t enc) {
    for (const SysRegDesc &d : kAppleTable) {
        if (d.encoding == enc) {
            return &d;
        }
    }
    return nullptr;
}

} // namespace ohv
