#include "macros.inc"

test_suite s32c1i_atomctl

#if XCHAL_HAVE_S32C1I && XCHAL_NUM_DATARAM > 0 && XCHAL_HW_VERSION >= 230000

test s32c1i_atomctl_system_ram_fault
    set_vector kernel, 2f

    movi    a5, 0x60000100
    movi    a3, 3
    s32i    a3, a5, 0
    movi    a2, 0
    wsr     a2, atomctl
    wsr     a3, scompare1
    movi    a4, 2
1:
    s32c1i  a4, a5, 0
    test_fail
2:
    rsr     a2, epc1
    movi    a3, 1b
    assert  eq, a2, a3
    rsr     a2, excvaddr
    assert  eq, a2, a5
    rsr     a2, exccause
    assert  eqi, a2, 3
    set_vector kernel, 0
    movi    a2, 0x60000100
    l32i    a2, a2, 0
    assert  eqi, a2, 3
test_end

test s32c1i_atomctl_dataram_ok
    movi    a2, 0
    wsr     a2, atomctl
    movi    a2, XCHAL_DATARAM0_VADDR + 0x100
    movi    a3, 3
    s32i    a3, a2, 0
    wsr     a3, scompare1
    movi    a4, 2
    s32c1i  a4, a2, 0
    assert  eq, a4, a3
    l32i    a4, a2, 0
    assert  eqi, a4, 2
test_end

#endif

test_suite_end
