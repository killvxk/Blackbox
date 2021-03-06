/* **********************************************************
 * Copyright (c) 2013 Google, Inc.   All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* DynamoRio eXtension utilities */

#include "dr_api.h"
#include "drx.h"
#include "drhashtable.h"
#include "../ext_utils.h"

/* We use drmgr but only internally.  A user of drx will end up loading in
 * the drmgr library, but it won't affect the user's code.
 */
#include "drmgr.h"

#ifdef UNIX
# include "../../core/unix/include/syscall.h"
# include <signal.h> /* SIGKILL */
#endif

#ifdef DEBUG
# define ASSERT(x, msg) DR_ASSERT_MSG(x, msg)
# define IF_DEBUG(x) x
#else
# define ASSERT(x, msg) /* nothing */
# define IF_DEBUG(x) /* nothing */
#endif /* DEBUG */

#define MINSERT instrlist_meta_preinsert
#define TESTALL(mask, var) (((mask) & (var)) == (mask))
#define TESTANY(mask, var) (((mask) & (var)) != 0)
#define TEST TESTANY
#define ALIGNED(x, alignment) ((((ptr_uint_t)x) & ((alignment)-1)) == 0)
#define ALIGN_FORWARD(x, alignment)  \
    ((((ptr_uint_t)x) + ((alignment)-1)) & (~((alignment)-1)))
#define ALIGN_BACKWARD(x, alignment) \
    (((ptr_uint_t)x) & (~((ptr_uint_t)(alignment)-1)))

/* Reserved note range values */
enum {
    DRX_NOTE_AFLAGS_RESTORE_BEGIN,
    DRX_NOTE_AFLAGS_RESTORE_SAHF,
    DRX_NOTE_AFLAGS_RESTORE_END,
    DRX_NOTE_COUNT,
};
static ptr_uint_t note_base;
#define NOTE_VAL(enum_val) ((void *)(ptr_int_t)(note_base + (enum_val)))

static bool soft_kills_enabled;

static void soft_kills_exit(void);

/***************************************************************************
 * INIT
 */

static int drx_init_count;

DR_EXPORT
bool
drx_init(void)
{
    int count = dr_atomic_add32_return_sum(&drx_init_count, 1);
    if (count > 1)
        return true;

    drmgr_init();
    note_base = drmgr_reserve_note_range(DRX_NOTE_COUNT);
    ASSERT(note_base != DRMGR_NOTE_NONE, "failed to reserve note range");

    return true;
}

DR_EXPORT
void
drx_exit()
{
    int count = dr_atomic_add32_return_sum(&drx_init_count, -1);
    if (count != 0)
        return;

    if (soft_kills_enabled)
        soft_kills_exit();

    drmgr_exit();
}


/***************************************************************************
 * INSTRUCTION NOTE FIELD
 */

/* For historical reasons we have this routine exported by drx.
 * We just forward to drmgr.
 */
DR_EXPORT
ptr_uint_t
drx_reserve_note_range(size_t size)
{
    return drmgr_reserve_note_range(size);
}

/***************************************************************************
 * ANALYSIS
 */

DR_EXPORT
bool
drx_aflags_are_dead(instr_t *where)
{
    instr_t *instr;
    uint flags;
    for (instr = where; instr != NULL; instr = instr_get_next(instr)) {
        /* we treat syscall/interrupt as aflags read */
        if (instr_is_syscall(instr) || instr_is_interrupt(instr))
            return false;
        flags = instr_get_arith_flags(instr);
        if (TESTANY(EFLAGS_READ_6, flags))
            return false;
        if (TESTALL(EFLAGS_WRITE_6, flags))
            return true;
        if (instr_is_cti(instr)) {
            if (instr_ok_to_mangle(instr) &&
                (instr_is_ubr(instr) || instr_is_call_direct(instr))) {
                instr_t *next = instr_get_next(instr);
                opnd_t   tgt  = instr_get_target(instr);
                /* continue on elision */
                if (next != NULL && instr_ok_to_mangle(next) &&
                    opnd_is_pc(tgt) &&
                    opnd_get_pc(tgt) == instr_get_app_pc(next))
                    continue;
            }
            /* unknown target, assume aflags is live */
            return false;
        }
    }
    return false;
}


/***************************************************************************
 * INSTRUMENTATION
 */

/* insert a label instruction with note */
static void
ilist_insert_note_label(void *drcontext, instrlist_t *ilist, instr_t *where,
                        void *note)
{
    instr_t *instr = INSTR_CREATE_label(drcontext);
    instr_set_note(instr, note);
    MINSERT(ilist, where, instr);
}

/* insert arithmetic flags saving code with more control
 * - skip %eax save if !save_eax
 * - save %eax to reg if reg is not DR_REG_NULL,
 * - save %eax to slot otherwise
 */
static void
drx_save_arith_flags(void *drcontext, instrlist_t *ilist, instr_t *where,
                     bool save_eax, bool save_oflag,
                     dr_spill_slot_t slot, reg_id_t reg)
{
    instr_t *instr;

    /* save %eax if necessary */
    if (save_eax) {
        if (reg != DR_REG_NULL) {
            ASSERT(reg >= DR_REG_START_GPR && reg <= DR_REG_STOP_GPR &&
                   reg != DR_REG_XAX, "wrong dead reg");
            MINSERT(ilist, where,
                    INSTR_CREATE_mov_st(drcontext,
                                        opnd_create_reg(reg),
                                        opnd_create_reg(DR_REG_XAX)));
        } else {
            ASSERT(slot >= SPILL_SLOT_1 && slot <= SPILL_SLOT_MAX,
                   "wrong spill slot");
            dr_save_reg(drcontext, ilist, where, DR_REG_XAX, slot);
        }
    }
    /* lahf */
    instr = INSTR_CREATE_lahf(drcontext);
    MINSERT(ilist, where, instr);
    if (save_oflag) {
        /* seto %al */
        instr = INSTR_CREATE_setcc(drcontext, OP_seto, opnd_create_reg(DR_REG_AL));
        MINSERT(ilist, where, instr);
    }
}

/* insert arithmetic flags restore code with more control
 * - skip %eax restore if !restore_eax
 * - restore %eax from reg if reg is not DR_REG_NULL
 * - restore %eax from slot otherwise
 *
 * Routine merge_prev_drx_aflags_switch looks for labels inserted by
 * drx_restore_arith_flags, so changes to this routine may affect
 * merge_prev_drx_aflags_switch.
 */
static void
drx_restore_arith_flags(void *drcontext, instrlist_t *ilist, instr_t *where,
                        bool restore_eax, bool restore_oflag,
                        dr_spill_slot_t slot, reg_id_t reg)
{
    instr_t *instr;
    ilist_insert_note_label(drcontext, ilist, where,
                            NOTE_VAL(DRX_NOTE_AFLAGS_RESTORE_BEGIN));
    if (restore_oflag) {
        /* add 0x7f, %al */
        instr = INSTR_CREATE_add(drcontext, opnd_create_reg(DR_REG_AL),
                                 OPND_CREATE_INT8(0x7f));
        MINSERT(ilist, where, instr);
    }
    /* sahf */
    instr = INSTR_CREATE_sahf(drcontext);
    instr_set_note(instr, NOTE_VAL(DRX_NOTE_AFLAGS_RESTORE_SAHF));
    MINSERT(ilist, where, instr);
    /* restore eax if necessary */
    if (restore_eax) {
        if (reg != DR_REG_NULL) {
            ASSERT(reg >= DR_REG_START_GPR && reg <= DR_REG_STOP_GPR &&
                   reg != DR_REG_XAX, "wrong dead reg");
            MINSERT(ilist, where,
                    INSTR_CREATE_mov_st(drcontext,
                                        opnd_create_reg(DR_REG_XAX),
                                        opnd_create_reg(reg)));
        } else {
            ASSERT(slot >= SPILL_SLOT_1 && slot <= SPILL_SLOT_MAX,
                   "wrong spill slot");
            dr_restore_reg(drcontext, ilist, where, DR_REG_XAX, slot);
        }
    }
    ilist_insert_note_label(drcontext, ilist, where,
                            NOTE_VAL(DRX_NOTE_AFLAGS_RESTORE_END));
}

/* Check if current instrumentation can be merged into previous aflags
 * save/restore inserted by drx_restore_arith_flags.
 * Returns NULL if cannot merge. Otherwise, returns the right insertion point,
 * i.e., DRX_NOTE_AFLAGS_RESTORE_BEGIN label instr.
 *
 * This routine looks for labels inserted by drx_restore_arith_flags,
 * so changes to drx_restore_arith_flags may affect this routine.
 */
static instr_t *
merge_prev_drx_aflags_switch(instr_t *where)
{
    instr_t *instr;
#ifdef DEBUG
    bool has_sahf = false;
#endif

    if (where == NULL)
        return NULL;
    instr = instr_get_prev(where);
    if (instr == NULL)
        return NULL;
    if (!instr_is_label(instr))
        return NULL;
    /* Check if prev instr is DRX_NOTE_AFLAGS_RESTORE_END.
     * We bail even there is only a label instr in between, which
     * might be a target of internal cti.
     */
    if (instr_get_note(instr) != NOTE_VAL(DRX_NOTE_AFLAGS_RESTORE_END))
        return NULL;

    /* find DRX_NOTE_AFLAGS_RESTORE_BEGIN */
    for (instr  = instr_get_prev(instr);
         instr != NULL;
         instr  = instr_get_prev(instr)) {
        if (instr_ok_to_mangle(instr)) {
            /* we do not expect any app instr */
            ASSERT(false, "drx aflags restore is corrupted");
            return NULL;
        }
        if (instr_is_label(instr)) {
            if (instr_get_note(instr) == NOTE_VAL(DRX_NOTE_AFLAGS_RESTORE_BEGIN)) {
                ASSERT(has_sahf, "missing sahf");
                return instr;
            }
            /* we do not expect any other label instr */
            ASSERT(false, "drx aflags restore is corrupted");
            return NULL;
#ifdef DEBUG
        } else {
            if (instr_get_note(instr) == NOTE_VAL(DRX_NOTE_AFLAGS_RESTORE_SAHF))
                has_sahf = true;
#endif
        }
    }
    return NULL;
}

static bool
counter_crosses_cache_line(byte *addr, size_t size)
{
    size_t cache_line_size = proc_get_cache_line_size();
    if (ALIGN_BACKWARD(addr, cache_line_size) ==
        ALIGN_BACKWARD(addr+size-1, cache_line_size))
        return false;
    return true;
}

DR_EXPORT
bool
drx_insert_counter_update(void *drcontext, instrlist_t *ilist, instr_t *where,
                          dr_spill_slot_t slot, void *addr, int value,
                          uint flags)
{
    instr_t *instr;
    bool save_aflags = !drx_aflags_are_dead(where);
    bool is_64 = TEST(DRX_COUNTER_64BIT, flags);

    if (drcontext == NULL) {
        ASSERT(false, "drcontext cannot be NULL");
        return false;
    }
    if (!(slot >= SPILL_SLOT_1 && slot <= SPILL_SLOT_MAX)) {
        ASSERT(false, "wrong spill slot");
        return false;
    }

    /* check whether we can add lock */
    if (TEST(DRX_COUNTER_LOCK, flags)) {
        if (IF_NOT_X64(is_64 ||) /* 64-bit counter in 32-bit mode */
            counter_crosses_cache_line((byte *)addr, is_64 ? 8 : 4))
            return false;
    }

    /* if save_aflags, check if we can merge with the prev aflags save */
    if (save_aflags) {
        instr = merge_prev_drx_aflags_switch(where);
        if (instr != NULL) {
            save_aflags = false;
            where = instr;
        }
    }

    /* save aflags if necessary */
    if (save_aflags) {
        drx_save_arith_flags(drcontext, ilist, where,
                             true /* save eax */, true /* save oflag */,
                             slot, DR_REG_NULL);
    }
    /* update counter */
    instr = INSTR_CREATE_add(drcontext,
                             OPND_CREATE_ABSMEM
                             (addr, IF_X64_ELSE((is_64 ? OPSZ_8 : OPSZ_4), OPSZ_4)),
                             OPND_CREATE_INT32(value));
    if (TEST(DRX_COUNTER_LOCK, flags))
        instr = LOCK(instr);
    MINSERT(ilist, where, instr);

#ifndef X64
    if (is_64) {
        MINSERT(ilist, where,
                INSTR_CREATE_adc(drcontext,
                                 OPND_CREATE_ABSMEM
                                 ((void *)((ptr_int_t)addr + 4), OPSZ_4),
                                 OPND_CREATE_INT32(0)));
    }
#endif /* !X64 */
    /* restore aflags if necessary */
    if (save_aflags) {
        drx_restore_arith_flags(drcontext, ilist, where,
                                true /* restore eax */, true /* restore oflag */,
                                slot, DR_REG_NULL);
    }
    return true;
}

/***************************************************************************
 * SOFT KILLS
 */

/* Track callbacks in a simple list protected by a lock */
typedef struct _cb_entry_t {
    /* XXX: the bool return value is complex to support in some situations.  We
     * ignore the return value and always skip the app's termination of the
     * child process for jobs containing multiple pids and for
     * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.  If we wanted to not skip those we'd
     * have to emulate the kill via NtTerminateProcess, which doesn't seem worth
     * it when our two use cases (DrMem and drcov) don't need that kind of
     * control.
     */
    bool (*cb)(process_id_t, int);
    struct _cb_entry_t *next;
} cb_entry_t;

static cb_entry_t *cb_list;
static void *cb_lock;

static bool
soft_kills_invoke_cbs(process_id_t pid, int exit_code)
{
    cb_entry_t *e;
    bool skip = false;
    dr_mutex_lock(cb_lock);
    for (e = cb_list; e != NULL; e = e->next) {
        /* If anyone wants to skip, we skip */
        skip = e->cb(pid, exit_code) || skip;
    }
    dr_mutex_unlock(cb_lock);
    return skip;
}

#ifdef WINDOWS

/* The system calls we need to watch for soft kills.
 * These are are in ntoskrnl so we get away without drsyscall.
 */
enum {
    SYS_NUM_PARAMS_TerminateProcess        = 2,
    SYS_NUM_PARAMS_TerminateJobObject      = 2,
    SYS_NUM_PARAMS_SetInformationJobObject = 4,
    SYS_NUM_PARAMS_Close                   = 1,
};

enum {
    SYS_WOW64_IDX_TerminateProcess        = 0,
    SYS_WOW64_IDX_TerminateJobObject      = 0,
    SYS_WOW64_IDX_SetInformationJobObject = 7,
    SYS_WOW64_IDX_Close                   = 0,
};

static int sysnum_TerminateProcess;
static int sysnum_TerminateJobObject;
static int sysnum_SetInformationJobObject;
static int sysnum_Close;

/* Table of job handles for which the app set JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE */
#define JOB_TABLE_HASH_BITS 6
static hashtable_t job_table;

/* We need CLS as we track data across syscalls, where TLS is not sufficient */
static int cls_idx_soft;

typedef struct _cls_soft_t {
    DWORD job_limit_flags_orig;
    DWORD *job_limit_flags_loc;
} cls_soft_t;

/* XXX: should we have some kind of shared wininc/ dir for these common defines?
 * I don't really want to include core/win32/ntdll.h here.
 */

typedef LONG NTSTATUS;
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)

/* Since we invoke only in a client/privlib context, we can statically link
 * with ntdll to call these syscall wrappers:
 */
#define GET_NTDLL(NtFunction, signature) NTSYSAPI NTSTATUS NTAPI NtFunction signature

GET_NTDLL(NtQueryInformationJobObject, (IN HANDLE JobHandle,
                                        IN JOBOBJECTINFOCLASS JobInformationClass,
                                        OUT PVOID JobInformation,
                                        IN ULONG JobInformationLength,
                                        OUT PULONG ReturnLength OPTIONAL));

#define STATUS_BUFFER_OVERFLOW           ((NTSTATUS)0x80000005L)

#define NT_CURRENT_PROCESS ((HANDLE)(ptr_int_t)-1)

typedef LONG KPRIORITY;

typedef enum _PROCESSINFOCLASS {
    ProcessBasicInformation,
} PROCESSINFOCLASS;

typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    void * PebBaseAddress;
    ULONG_PTR AffinityMask;
    KPRIORITY BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;
typedef PROCESS_BASIC_INFORMATION *PPROCESS_BASIC_INFORMATION;

GET_NTDLL(NtQueryInformationProcess, (IN HANDLE ProcessHandle,
                                      IN PROCESSINFOCLASS ProcessInformationClass,
                                      OUT PVOID ProcessInformation,
                                      IN ULONG ProcessInformationLength,
                                      OUT PULONG ReturnLength OPTIONAL));
GET_NTDLL(NtTerminateProcess, (IN HANDLE ProcessHandle,
                               IN NTSTATUS ExitStatus));

static ssize_t
num_job_object_pids(HANDLE job)
{
    JOBOBJECT_BASIC_PROCESS_ID_LIST empty;
    NTSTATUS res;
    res = NtQueryInformationJobObject(job, JobObjectBasicProcessIdList,
                                      &empty, sizeof(empty), NULL);
    if (NT_SUCCESS(res) || res == STATUS_BUFFER_OVERFLOW)
        return empty.NumberOfAssignedProcesses;
    else
        return -1;
}

static bool
get_job_object_pids(HANDLE job, JOBOBJECT_BASIC_PROCESS_ID_LIST *list, size_t list_sz)
{
    NTSTATUS res;
    res = NtQueryInformationJobObject(job, JobObjectBasicProcessIdList,
                                      list, (ULONG) list_sz, NULL);
    return NT_SUCCESS(res);
}

/* XXX: should DR provide a routine to query this? */
static bool
get_app_exit_code(int *exit_code)
{
    ULONG got;
    PROCESS_BASIC_INFORMATION info;
    NTSTATUS res;
    memset(&info, 0, sizeof(PROCESS_BASIC_INFORMATION));
    res = NtQueryInformationProcess(NT_CURRENT_PROCESS, ProcessBasicInformation,
                                    &info, sizeof(PROCESS_BASIC_INFORMATION), &got);
    if (!NT_SUCCESS(res) || got != sizeof(PROCESS_BASIC_INFORMATION))
        return false;
    *exit_code = (int) info.ExitStatus;
    return true;
}

static void
soft_kills_context_init(void *drcontext, bool new_depth)
{
    cls_soft_t *cls;
    if (new_depth) {
        cls = (cls_soft_t *) dr_thread_alloc(drcontext, sizeof(*cls));
        drmgr_set_cls_field(drcontext, cls_idx_soft, cls);
    } else {
        cls = (cls_soft_t *) drmgr_get_cls_field(drcontext, cls_idx_soft);
    }
    memset(cls, 0, sizeof(*cls));
}

static void
soft_kills_context_exit(void *drcontext, bool thread_exit)
{
    if (thread_exit) {
        cls_soft_t *cls = (cls_soft_t *) drmgr_get_cls_field(drcontext, cls_idx_soft);
        dr_thread_free(drcontext, cls, sizeof(*cls));
    }
    /* else, nothing to do: we leave the struct for re-use on next callback */
}

static int
soft_kills_get_sysnum(const char *name, int num_params, int wow64_idx)
{
    static module_handle_t ntdll;
    app_pc wrapper;
    int sysnum;

    if (ntdll == NULL) {
        module_data_t *data = dr_lookup_module_by_name("ntdll.dll");
        if (data == NULL)
            return -1;
        ntdll = data->handle;
        dr_free_module_data(data);
    }
    wrapper = (app_pc) dr_get_proc_address(ntdll, name);
    if (wrapper == NULL)
        return -1;
    sysnum = drmgr_decode_sysnum_from_wrapper(wrapper);
    if (sysnum == -1)
        return -1;
    /* Ensure that DR intercepts these if we go native.
     * XXX: better to only do this if client plans to use native execution
     * to reduce the hook count and shrink chance of hook conflicts?
     */
    if (!dr_syscall_intercept_natively(name, sysnum, num_params, wow64_idx))
        return -1;
    return sysnum;
}

static void
soft_kills_handle_job_termination(void *drcontext, HANDLE job, int exit_code)
{
    ssize_t num_jobs = num_job_object_pids(job);
    if (num_jobs > 0) {
        JOBOBJECT_BASIC_PROCESS_ID_LIST *list;
        size_t sz = sizeof(*list) + (num_jobs- 1)*sizeof(list->ProcessIdList[0]);
        byte *buf = dr_thread_alloc(drcontext, sz);
        list = (JOBOBJECT_BASIC_PROCESS_ID_LIST *) buf;
        if (get_job_object_pids(job, list, sz)) {
            int i;
            for (i = 0; i < num_jobs; i++) {
                process_id_t pid = list->ProcessIdList[i];
                if (!soft_kills_invoke_cbs(pid, exit_code)) {
                    /* Client is not terminating and requests not to skip the action.
                     * But since we have multiple pids, we go with a local decision
                     * here and emulate the kill.
                     */
                    HANDLE phandle = dr_convert_pid_to_handle(pid);
                    if (phandle != INVALID_HANDLE_VALUE)
                        NtTerminateProcess(phandle, exit_code);
                    /* else, child stays alive: not much we can do */
                }
            }
        }
        dr_thread_free(drcontext, buf, sz);
    } /* else query failed: I'd issue a warning log msg if not inside drx library */
}

static void
soft_kills_handle_close(void *drcontext, HANDLE job, int exit_code)
{
    soft_kills_handle_job_termination(drcontext, job, exit_code);
}

static bool
soft_kills_filter_syscall(void *drcontext, int sysnum)
{
    return (sysnum == sysnum_TerminateProcess ||
            sysnum == sysnum_TerminateJobObject ||
            sysnum == sysnum_SetInformationJobObject ||
            sysnum == sysnum_Close);
}

/* Returns whether to execute the system call */
static bool
soft_kills_pre_syscall(void *drcontext, int sysnum)
{
    cls_soft_t *cls = (cls_soft_t *) drmgr_get_cls_field(drcontext, cls_idx_soft);
    /* Xref DrMem i#544, DrMem i#1297, and DRi#1231: give child
     * processes a chance for clean exit for dumping of data or other
     * actions.
     *
     * XXX: a child under DR but not a supporting client will be left
     * alive: but that's a risk we can live with.
     */
    if (sysnum == sysnum_TerminateProcess) {
        HANDLE proc = (HANDLE) dr_syscall_get_param(drcontext, 0);
        process_id_t pid = dr_convert_handle_to_pid(proc);
        if (pid != INVALID_PROCESS_ID && pid != dr_get_process_id()) {
            int exit_code = (int) dr_syscall_get_param(drcontext, 1);
            if (soft_kills_invoke_cbs(pid, exit_code)) {
                dr_syscall_set_result(drcontext, 0/*success*/);
                return false; /* skip syscall */
            } else
                return true; /* execute syscall */
        }
    }
    else if (sysnum == sysnum_TerminateJobObject) {
        /* There are several ways a process in a job can be killed:
         *
         *   1) NtTerminateJobObject
         *   2) The last handle is closed + JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is set
         *   3) JOB_OBJECT_LIMIT_ACTIVE_PROCESS is hit
         *   4) Time limit and JOB_OBJECT_TERMINATE_AT_END_OF_JOB is hit
         *
         * XXX: we only handle #1 and #2.
         */
        HANDLE job = (HANDLE) dr_syscall_get_param(drcontext, 0);
        NTSTATUS exit_code = (NTSTATUS) dr_syscall_get_param(drcontext, 1);
        soft_kills_handle_job_termination(drcontext, job, exit_code);
        /* We always skip this syscall.  If individual processes were requested
         * to not be skipped, we emulated via NtTerminateProcess in
         * soft_kills_handle_job_termination().
         */
        dr_syscall_set_result(drcontext, 0/*success*/);
        return false; /* skip syscall */
    }
    else if (sysnum == sysnum_SetInformationJobObject) {
        HANDLE job = (HANDLE) dr_syscall_get_param(drcontext, 0);
        JOBOBJECTINFOCLASS class = (JOBOBJECTINFOCLASS)
            dr_syscall_get_param(drcontext, 1);
        ULONG sz = (ULONG) dr_syscall_get_param(drcontext, 3);
        /* MSDN claims that JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE requires an
         * extended info struct, which we trust, though it seems odd as it's
         * a flag in the basic struct.
         */
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        if (class == JobObjectExtendedLimitInformation &&
            sz >= sizeof(info) &&
            dr_safe_read((byte *)dr_syscall_get_param(drcontext, 2),
                         sizeof(info), &info, NULL)) {
            if (TEST(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
                     info.BasicLimitInformation.LimitFlags)) {
                /* Remove the kill-on-close flag from the syscall arg.
                 * We restore in post-syscall in case app uses the memory
                 * for something else.  There is of course a race where another
                 * thread could use it and get the wrong value: -soft_kills isn't
                 * perfect.
                 */
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION *ptr =
                    (JOBOBJECT_EXTENDED_LIMIT_INFORMATION *)
                    dr_syscall_get_param(drcontext, 2);
                ULONG new_flags = info.BasicLimitInformation.LimitFlags &
                    (~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
                bool isnew;
                cls->job_limit_flags_orig = info.BasicLimitInformation.LimitFlags;
                cls->job_limit_flags_loc = &ptr->BasicLimitInformation.LimitFlags;
                ASSERT(sizeof(cls->job_limit_flags_orig) ==
                       sizeof(ptr->BasicLimitInformation.LimitFlags), "size mismatch");
                if (!dr_safe_write(cls->job_limit_flags_loc,
                                   sizeof(ptr->BasicLimitInformation.LimitFlags),
                                   &new_flags, NULL)) {
                    /* XXX: Any way we can send a WARNING on our failure to write? */
                }
                /* Track the handle so we can notify the client on close or exit */
                isnew = hashtable_add(&job_table, (void *)job, (void *)job);
                ASSERT(isnew, "missed an NtClose");
            }
        }
    }
    else if (sysnum == sysnum_Close) {
        /* If a job object, act on it, and remove from our table */
        HANDLE handle = (HANDLE) dr_syscall_get_param(drcontext, 0);
        if (hashtable_remove(&job_table, (void *)handle)) {
            /* The exit code is set to 0 by the kernel for this case */
            soft_kills_handle_close(drcontext, handle, 0);
        }
    }
    return true;
}

static void
soft_kills_post_syscall(void *drcontext, int sysnum)
{
    if (sysnum == sysnum_SetInformationJobObject) {
        cls_soft_t *cls = (cls_soft_t *) drmgr_get_cls_field(drcontext, cls_idx_soft);
        if (cls->job_limit_flags_loc != NULL) {
            /* Restore the app's memory */
            if (!dr_safe_write(cls->job_limit_flags_loc,
                               sizeof(cls->job_limit_flags_orig),
                               &cls->job_limit_flags_orig, NULL)) {
                /* If we weren't in drx lib I'd log a warning */
            }
            cls->job_limit_flags_loc = NULL;
        }
    }
}
#else /* WINDOWS */

static bool
soft_kills_filter_syscall(void *drcontext, int sysnum)
{
    return (sysnum == SYS_kill);
}

/* Returns whether to execute the system call */
static bool
soft_kills_pre_syscall(void *drcontext, int sysnum)
{
    if (sysnum == SYS_kill) {
        process_id_t pid = (process_id_t) dr_syscall_get_param(drcontext, 0);
        int sig = (int) dr_syscall_get_param(drcontext, 1);
        if (sig == SIGKILL && pid != INVALID_PROCESS_ID && pid != dr_get_process_id()) {
            /* Pass exit code << 8 for use with dr_exit_process() */
            int exit_code = sig << 8;
            if (soft_kills_invoke_cbs(pid, exit_code)) {
                dr_syscall_set_result(drcontext, 0/*success*/);
                return false; /* skip syscall */
            } else
                return true; /* execute syscall */
        }
    }
    return true;
}

static void
soft_kills_post_syscall(void *drcontext, int sysnum)
{
    /* nothing yet */
}

#endif /* UNIX */

static bool
soft_kills_init(void)
{
#ifdef WINDOWS
    IF_DEBUG(bool ok;)
#endif
    /* XXX: would be nice to fail if it's not still process init,
     * but we don't have an easy way to check.
     */
    soft_kills_enabled = true;

    cb_lock = dr_mutex_create();

#ifdef WINDOWS
    hashtable_init(&job_table, JOB_TABLE_HASH_BITS, HASH_INTPTR, false);

    sysnum_TerminateProcess =
        soft_kills_get_sysnum("NtTerminateProcess",
                              SYS_NUM_PARAMS_TerminateProcess,
                              SYS_WOW64_IDX_TerminateProcess);
    if (sysnum_TerminateProcess == -1)
        return false;
    sysnum_TerminateJobObject =
        soft_kills_get_sysnum("NtTerminateJobObject",
                              SYS_NUM_PARAMS_TerminateJobObject,
                              SYS_WOW64_IDX_TerminateJobObject);
    if (sysnum_TerminateJobObject == -1)
        return false;
    sysnum_SetInformationJobObject =
        soft_kills_get_sysnum("NtSetInformationJobObject",
                              SYS_NUM_PARAMS_SetInformationJobObject,
                              SYS_WOW64_IDX_SetInformationJobObject);
    if (sysnum_SetInformationJobObject == -1)
        return false;
    sysnum_Close = soft_kills_get_sysnum("NtClose",
                                         SYS_NUM_PARAMS_Close, SYS_WOW64_IDX_Close);
    if (sysnum_Close == -1)
        return false;

    cls_idx_soft = drmgr_register_cls_field(soft_kills_context_init,
                                            soft_kills_context_exit);
    if (cls_idx_soft == -1)
        return false;

    /* Ensure that DR intercepts these when we're native */
    IF_DEBUG(ok = )
        dr_syscall_intercept_natively("NtTerminateProcess",
                                      sysnum_TerminateProcess,
                                      SYS_NUM_PARAMS_TerminateProcess,
                                      SYS_WOW64_IDX_TerminateProcess);
    ASSERT(ok, "failure to watch syscall while native");
    IF_DEBUG(ok = )
        dr_syscall_intercept_natively("NtTerminateJobObject",
                                      sysnum_TerminateJobObject,
                                      SYS_NUM_PARAMS_TerminateJobObject,
                                      SYS_WOW64_IDX_TerminateJobObject);
    ASSERT(ok, "failure to watch syscall while native");
    IF_DEBUG(ok = )
        dr_syscall_intercept_natively("NtSetInformationJobObject",
                                      sysnum_SetInformationJobObject,
                                      SYS_NUM_PARAMS_SetInformationJobObject,
                                      SYS_WOW64_IDX_SetInformationJobObject);
    ASSERT(ok, "failure to watch syscall while native");
    IF_DEBUG(ok = )
        dr_syscall_intercept_natively("NtClose",
                                      sysnum_Close,
                                      SYS_NUM_PARAMS_Close,
                                      SYS_WOW64_IDX_Close);
    ASSERT(ok, "failure to watch syscall while native");
#endif

    if (!drmgr_register_pre_syscall_event(soft_kills_pre_syscall) ||
        !drmgr_register_post_syscall_event(soft_kills_post_syscall))
        return false;
    dr_register_filter_syscall_event(soft_kills_filter_syscall);

    return true;
}

static void
soft_kills_exit(void)
{
    cb_entry_t *e;
#ifdef WINDOWS
    /* Any open job handles will be closed, triggering
     * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
     */
    uint i;
    /* The exit code used is the exit code for this process */
    int exit_code;
    if (!get_app_exit_code(&exit_code))
        exit_code = 0;
    hashtable_lock(&job_table);
    for (i = 0; i < HASHTABLE_SIZE(job_table.table_bits); i++) {
        hashtable_entry_t *he;
        for (he = job_table.table[i]; he != NULL; he = he->next) {
            HANDLE job = (HANDLE) he->payload;
            soft_kills_handle_close(dr_get_current_drcontext(), job, exit_code);
        }
    }
    hashtable_unlock(&job_table);

    hashtable_delete(&job_table);

    drmgr_unregister_cls_field(soft_kills_context_init, soft_kills_context_exit,
                               cls_idx_soft);
#endif

    dr_mutex_lock(cb_lock);
    while (cb_list != NULL) {
        e = cb_list;
        cb_list = e->next;
        dr_global_free(e, sizeof(*e));
    }
    dr_mutex_unlock(cb_lock);

    dr_mutex_destroy(cb_lock);
}

bool
drx_register_soft_kills(bool (*event_cb)(process_id_t pid, int exit_code))
{
    /* We split our init from drx_init() to avoid extra work when nobody
     * requests this feature.
     */
    static int soft_kills_init_count;
    cb_entry_t *e;
    int count = dr_atomic_add32_return_sum(&soft_kills_init_count, 1);
    if (count == 1) {
        soft_kills_init();
    }

    e = dr_global_alloc(sizeof(*e));
    e->cb = event_cb;

    dr_mutex_lock(cb_lock);
    e->next = cb_list;
    cb_list = e;
    dr_mutex_unlock(cb_lock);
    return true;
}

/***************************************************************************
 * LOGGING
 */

file_t
drx_open_unique_file(const char *dir, const char *prefix, const char *suffix,
                     uint extra_flags, char *result OUT, size_t result_len)
{
    char buf[MAXIMUM_PATH];
    file_t f;
    int i;
    size_t len;
    for (i = 0; i < 10000; i++) {
        len = dr_snprintf(buf, BUFFER_SIZE_ELEMENTS(buf),
                          "%s/%s.%04d.%s", dir, prefix, i, suffix);
        if (len < 0)
            return false;
        NULL_TERMINATE_BUFFER(buf);
        f = dr_open_file(buf, DR_FILE_WRITE_REQUIRE_NEW | extra_flags);
        if (f != INVALID_FILE) {
            if (result != NULL)
                dr_snprintf(result, result_len, "%s", buf);
            return f;
        }
    }
    return INVALID_FILE;
}

file_t
drx_open_unique_appid_file(const char *dir, ptr_int_t id,
                           const char *prefix, const char *suffix,
                           uint extra_flags, char *result OUT, size_t result_len)
{
    int len;
    char appid[MAXIMUM_PATH];
    const char *app_name = dr_get_application_name();
    if (app_name == NULL)
        app_name = "<unknown-app>";
    len = dr_snprintf(appid, BUFFER_SIZE_ELEMENTS(appid),
                      "%s.%s.%05d", prefix, app_name, id);
    if (len < 0 || (size_t)len >= BUFFER_SIZE_ELEMENTS(appid))
        return INVALID_FILE;
    NULL_TERMINATE_BUFFER(appid);

    return drx_open_unique_file(dir, appid, suffix, extra_flags, result, result_len);
}
