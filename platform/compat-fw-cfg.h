// SPDX-License-Identifier: MIT
#ifndef SEABIOS_VPD_COMPAT_FW_CFG_H
#define SEABIOS_VPD_COMPAT_FW_CFG_H

#include "romfile.h"

#define CERT_VPD_FW_CFG "opt/seabios/cert-vpd"
#define TPM_HANDOFF_FW_CFG "opt/seabios/tpm-platform-auth-handoff"

static inline int
compat_tpm_handoff_enabled(void)
{
    struct romfile_s *file = romfile_find(TPM_HANDOFF_FW_CFG);
    if (!file)
        return 0;
    u8 value;
    if (file->size != 1 || file->copy(file, &value, 1) != 1 ||
        (value != '0' && value != '1'))
        panic("TPM handoff: expected one ASCII 0 or 1 byte\n");
    return value == '1';
}
#endif
