/*
 * Syscall implementations for semihosting.
 *
 * Copyright (c) 2022 Linaro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/gdbstub.h"
#include "semihosting/guestfd.h"
#include "semihosting/syscalls.h"
#ifdef CONFIG_USER_ONLY
#include "qemu.h"
#else
#include "semihosting/softmmu-uaccess.h"
#endif


/*
 * Validate or compute the length of the string (including terminator).
 */
static int validate_strlen(CPUState *cs, target_ulong str, target_ulong tlen)
{
    CPUArchState *env G_GNUC_UNUSED = cs->env_ptr;
    char c;

    if (tlen == 0) {
        ssize_t slen = target_strlen(str);

        if (slen < 0) {
            return -EFAULT;
        }
        if (slen >= INT32_MAX) {
            return -ENAMETOOLONG;
        }
        return slen + 1;
    }
    if (tlen > INT32_MAX) {
        return -ENAMETOOLONG;
    }
    if (get_user_u8(c, str + tlen - 1)) {
        return -EFAULT;
    }
    if (c != 0) {
        return -EINVAL;
    }
    return tlen;
}

static int validate_lock_user_string(char **pstr, CPUState *cs,
                                     target_ulong tstr, target_ulong tlen)
{
    int ret = validate_strlen(cs, tstr, tlen);
    CPUArchState *env G_GNUC_UNUSED = cs->env_ptr;
    char *str = NULL;

    if (ret > 0) {
        str = lock_user(VERIFY_READ, tstr, ret, true);
        ret = str ? 0 : -EFAULT;
    }
    *pstr = str;
    return ret;
}

/*
 * GDB semihosting syscall implementations.
 */

static gdb_syscall_complete_cb gdb_open_complete;

static void gdb_open_cb(CPUState *cs, target_ulong ret, target_ulong err)
{
    if (!err) {
        int guestfd = alloc_guestfd();
        associate_guestfd(guestfd, ret);
        ret = guestfd;
    }
    gdb_open_complete(cs, ret, err);
}

static void gdb_open(CPUState *cs, gdb_syscall_complete_cb complete,
                     target_ulong fname, target_ulong fname_len,
                     int gdb_flags, int mode)
{
    int len = validate_strlen(cs, fname, fname_len);
    if (len < 0) {
        complete(cs, -1, -len);
        return;
    }

    gdb_open_complete = complete;
    gdb_do_syscall(gdb_open_cb, "open,%s,%x,%x",
                   fname, len, (target_ulong)gdb_flags, (target_ulong)mode);
}

static void gdb_close(CPUState *cs, gdb_syscall_complete_cb complete,
                      GuestFD *gf)
{
    gdb_do_syscall(complete, "close,%x", (target_ulong)gf->hostfd);
}

static void gdb_read(CPUState *cs, gdb_syscall_complete_cb complete,
                     GuestFD *gf, target_ulong buf, target_ulong len)
{
    gdb_do_syscall(complete, "read,%x,%x,%x",
                   (target_ulong)gf->hostfd, buf, len);
}

/*
 * Host semihosting syscall implementations.
 */

static void host_open(CPUState *cs, gdb_syscall_complete_cb complete,
                      target_ulong fname, target_ulong fname_len,
                      int gdb_flags, int mode)
{
    CPUArchState *env G_GNUC_UNUSED = cs->env_ptr;
    char *p;
    int ret, host_flags;

    ret = validate_lock_user_string(&p, cs, fname, fname_len);
    if (ret < 0) {
        complete(cs, -1, -ret);
        return;
    }

    if (gdb_flags & GDB_O_WRONLY) {
        host_flags = O_WRONLY;
    } else if (gdb_flags & GDB_O_RDWR) {
        host_flags = O_RDWR;
    } else {
        host_flags = O_RDONLY;
    }
    if (gdb_flags & GDB_O_CREAT) {
        host_flags |= O_CREAT;
    }
    if (gdb_flags & GDB_O_TRUNC) {
        host_flags |= O_TRUNC;
    }
    if (gdb_flags & GDB_O_EXCL) {
        host_flags |= O_EXCL;
    }

    ret = open(p, host_flags, mode);
    if (ret < 0) {
        complete(cs, -1, errno);
    } else {
        int guestfd = alloc_guestfd();
        associate_guestfd(guestfd, ret);
        complete(cs, guestfd, 0);
    }
    unlock_user(p, fname, 0);
}

static void host_close(CPUState *cs, gdb_syscall_complete_cb complete,
                       GuestFD *gf)
{
    /*
     * Only close the underlying host fd if it's one we opened on behalf
     * of the guest in SYS_OPEN.
     */
    if (gf->hostfd != STDIN_FILENO &&
        gf->hostfd != STDOUT_FILENO &&
        gf->hostfd != STDERR_FILENO &&
        close(gf->hostfd) < 0) {
        complete(cs, -1, errno);
    } else {
        complete(cs, 0, 0);
    }
}

static void host_read(CPUState *cs, gdb_syscall_complete_cb complete,
                      GuestFD *gf, target_ulong buf, target_ulong len)
{
    CPUArchState *env G_GNUC_UNUSED = cs->env_ptr;
    void *ptr = lock_user(VERIFY_WRITE, buf, len, 0);
    ssize_t ret;

    if (!ptr) {
        complete(cs, -1, EFAULT);
        return;
    }
    do {
        ret = read(gf->hostfd, ptr, len);
    } while (ret == -1 && errno == EINTR);
    if (ret == -1) {
        complete(cs, -1, errno);
        unlock_user(ptr, buf, 0);
    } else {
        complete(cs, ret, 0);
        unlock_user(ptr, buf, ret);
    }
}

/*
 * Static file semihosting syscall implementations.
 */

static void staticfile_read(CPUState *cs, gdb_syscall_complete_cb complete,
                            GuestFD *gf, target_ulong buf, target_ulong len)
{
    CPUArchState *env G_GNUC_UNUSED = cs->env_ptr;
    target_ulong rest = gf->staticfile.len - gf->staticfile.off;
    void *ptr;

    if (len > rest) {
        len = rest;
    }
    ptr = lock_user(VERIFY_WRITE, buf, len, 0);
    if (!ptr) {
        complete(cs, -1, EFAULT);
        return;
    }
    memcpy(ptr, gf->staticfile.data + gf->staticfile.off, len);
    gf->staticfile.off += len;
    complete(cs, len, 0);
    unlock_user(ptr, buf, len);
}

/*
 * Syscall entry points.
 */

void semihost_sys_open(CPUState *cs, gdb_syscall_complete_cb complete,
                       target_ulong fname, target_ulong fname_len,
                       int gdb_flags, int mode)
{
    if (use_gdb_syscalls()) {
        gdb_open(cs, complete, fname, fname_len, gdb_flags, mode);
    } else {
        host_open(cs, complete, fname, fname_len, gdb_flags, mode);
    }
}

void semihost_sys_close(CPUState *cs, gdb_syscall_complete_cb complete, int fd)
{
    GuestFD *gf = get_guestfd(fd);

    if (!gf) {
        complete(cs, -1, EBADF);
        return;
    }
    switch (gf->type) {
    case GuestFDGDB:
        gdb_close(cs, complete, gf);
        break;
    case GuestFDHost:
        host_close(cs, complete, gf);
        break;
    case GuestFDStatic:
        complete(cs, 0, 0);
        break;
    default:
        g_assert_not_reached();
    }
    dealloc_guestfd(fd);
}

void semihost_sys_read_gf(CPUState *cs, gdb_syscall_complete_cb complete,
                          GuestFD *gf, target_ulong buf, target_ulong len)
{
    switch (gf->type) {
    case GuestFDGDB:
        gdb_read(cs, complete, gf, buf, len);
        break;
    case GuestFDHost:
        host_read(cs, complete, gf, buf, len);
        break;
    case GuestFDStatic:
        staticfile_read(cs, complete, gf, buf, len);
        break;
    default:
        g_assert_not_reached();
    }
}

void semihost_sys_read(CPUState *cs, gdb_syscall_complete_cb complete,
                       int fd, target_ulong buf, target_ulong len)
{
    GuestFD *gf = get_guestfd(fd);

    if (gf) {
        semihost_sys_read_gf(cs, complete, gf, buf, len);
    } else {
        complete(cs, -1, EBADF);
    }
}