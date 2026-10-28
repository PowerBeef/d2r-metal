// Registers in the signal context (ucontext_t): the host always reads and writes them through these macros.
// UC_ESR: ESR_EL1 at the time of the fault (the WnR bit of a page fault = write access).
#pragma once
#include <stdint.h>
#include <sys/ucontext.h>
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define UC_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define UC_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define UC_X(uc, i) ((uc)->uc_mcontext->__ss.__x[i])
static inline uint64_t UC_ESR(const ucontext_t *uc) { return uc->uc_mcontext->__es.__esr; }
