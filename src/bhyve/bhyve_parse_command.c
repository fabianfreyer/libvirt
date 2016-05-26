/*
 * bhyve_parse_command.c: Bhyve command parser
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (C) 2016 Fabian Freyer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Fabian Freyer <fabian.freyer@physik.tu-berlin.de>
 */

#include <config.h>
#include <getopt.h>

#include "bhyve_capabilities.h"
#include "bhyve_command.h"
#include "bhyve_parse_command.h"
#include "viralloc.h"
#include "virlog.h"
#include "virstring.h"
#include "virutil.h"
#include "c-ctype.h"

#define VIR_FROM_THIS VIR_FROM_BHYVE

VIR_LOG_INIT("bhyve.bhyve_parse_command");

/*
 * This function takes a string representation of the command line and removes
 * all newline characters, if they are prefixed by a backslash. The result
 * should be a string with one command per line.
 *
 * NB: command MUST be NULL-Terminated.
 */
static char *
bhyveParseCommandLineUnescape(const char *command)
{
    size_t len = strlen(command);
    char *unescaped = NULL;
    char *curr_src = NULL;
    char *curr_dst = NULL;

    /* Since we are only removing characters, allocating a buffer of the same
     * size as command shouldn't be a problem here */
    if (VIR_ALLOC_N(unescaped, len) < 0)
        return NULL;

    /*
     * Iterate over characters in the command, skipping "\\\n", "\\\r" as well as
     * "\\\r\n".
     *
     * FIXME: Clean up this code, possible use a while loop and strncmps.
     */
    for (curr_src = (char*) command, curr_dst = unescaped; *curr_src != '\0';
        curr_src++, curr_dst++) {
        if (*curr_src == '\\') {
            switch (*(curr_src + 1)) {
                case '\n': /* \LF */
                    curr_src++;
                    curr_dst--;
                    break;
                case '\r': /* \CR */
                    curr_src++;
                    curr_dst--;
                    if (*curr_src == '\n') /* \CRLF */
                        curr_src++;
                    break;
                default:
                    *curr_dst = '\\';
            }
        }
        else *curr_dst = *curr_src;
    }

    return unescaped;
}

/**
 * Try to extract loader and bhyve argv lists from a command line string.
 */
static int
bhyveCommandLine2argv(const char *nativeConfig,
                      int *loader_argc,
                      char ***loader_argv,
                      int *bhyve_argc,
                      char ***bhyve_argv)
{
    const char *curr = NULL;
    char *nativeConfig_unescaped = NULL;
    const char *start;
    const char *next;
    char *line;
    char **lines = NULL;
    size_t line_count = 0;
    size_t lines_alloc = 0;
    char **_bhyve_argv = NULL;
    char **_loader_argv = NULL;

    nativeConfig_unescaped = bhyveParseCommandLineUnescape(nativeConfig);
    if (nativeConfig_unescaped == NULL)
        goto error;

    curr = nativeConfig_unescaped;

    /* Iterate over string, splitting on sequences of '\n' */
    while (curr && *curr != '\0') {
        start = curr;
        next = strchr(curr, '\n');

        if (VIR_STRNDUP(line, curr, next ? next - curr : -1) < 0)
            goto error;

        if (VIR_RESIZE_N(lines, lines_alloc, line_count, 2) < 0) {
            VIR_FREE(line);
            goto error;
        }

        if (*line) lines[line_count++] = line;
        lines[line_count] = NULL;

        while (next && (*next == '\n' || *next == '\r'
                        || STRPREFIX(next, "\r\n")))
            next++;

        curr = next;
    }

    for (int i = 0; i < line_count; i++) {
        curr = lines[i];
        int j;
        char **arglist = NULL;
        size_t args_count = 0;
        size_t args_alloc = 0;

        /* iterate over each line, splitting on sequences of ' '. This code is
         * adapted from qemu/qemu_parse_command.c. */
        while (curr && *curr != '\0') {
            char *arg;
            start = curr;

            if (*start == '\'') {
                if (start == curr)
                    curr++;
                next = strchr(start + 1, '\'');
            } else if (*start == '"') {
                if (start == curr)
                    curr++;
                next = strchr(start + 1, '"');
            } else {
                next = strchr(start, ' ');
            }

            if (VIR_STRNDUP(arg, curr, next ? next - curr : -1) < 0)
                goto error;

            if (next && (*next == '\'' || *next == '"'))
                next++;

            if (VIR_RESIZE_N(arglist, args_alloc, args_count, 2) < 0) {
                VIR_FREE(arg);
                goto error;
            }

            arglist[args_count++] = arg;
            arglist[args_count] = NULL;

            while (next && c_isspace(*next))
                next++;

            curr = next;
        }

        /*
         * To prevent a memory leak here, only set the argument lists when
         * the first matching command is found. This shouldn't really be a
         * problem, since usually no multiple loaders or bhyverun commands
         * are specified (this wouldn't really be valid anyways).
         * Otherwise, later argument lists may be assigned to _argv without
         * freeing the earlier ones.
         */
        if (!_bhyve_argv && STREQ(arglist[0], "/usr/sbin/bhyve")) {
            if ((VIR_REALLOC_N(_bhyve_argv, args_count + 1) < 0)
                || (!bhyve_argc))
                goto error;
            for (j = 0; j < args_count; j++)
                _bhyve_argv[j] = arglist[j];
            _bhyve_argv[j] = NULL;
            *bhyve_argc = args_count-1;
        }
        else if (!_loader_argv && (STREQ(arglist[0], "/usr/sbin/bhyveload")
                 || STREQ(arglist[0], "/usr/sbin/grub-bhyve"))) {
            if ((VIR_REALLOC_N(_loader_argv, args_count + 1) < 0)
                || (!loader_argc))
                goto error;
            for (j = 0; j < args_count; j++)
                _loader_argv[j] = arglist[j];
            _loader_argv[j] = NULL;
            *loader_argc = args_count-1;
        }
        /* To prevent a use-after-free here, only free the argument list when it is
         * definitely not going to be used */
        else
                virStringFreeList(arglist);
    }

    *loader_argv = _loader_argv;
    *bhyve_argv = _bhyve_argv;

    virStringFreeList(lines);
    return 0;

 error:
    VIR_FREE(_loader_argv);
    VIR_FREE(_bhyve_argv);
    virStringFreeList(lines);
    return -1;
}

/*
 * Parse the /usr/bin/bhyve command line. Parts of this are taken from the
 * FreeBSD source code, specifically from usr.sbin/bhyve/bhyverun.c, which
 * is licensed under the following license:
 *
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
static int
bhyveParseBhyveCommandLine(virDomainDefPtr def, int argc, char **argv)
{
    int c;
    const char optstr[] = "abehuwxACHIPSWYp:g:c:s:m:l:U:";
    int vcpus = 1;

    if (!argv)
        goto error;

    optind = 1;
    while ((c = getopt(argc, argv, optstr)) != -1) {
        switch (c) {
        case 'a':
            // x2apic_mode = 0;
            break;
        case 'A':
            // acpi = 1;
            break;
        case 'b':
            // bvmcons = 1;
            break;
        case 'p':
            // if (pincpu_parse(optarg) != 0) {
            //     errx(EX_USAGE, "invalid vcpu pinning "
            //          "configuration '%s'", optarg);
            // }
            break;
        case 'c':
            /* -c: # cpus (default 1) */
            if (virStrToLong_i(optarg, NULL, 10, &vcpus) < 0)
                goto error;
            if (virDomainDefSetVcpusMax(def, vcpus) < 0)
                goto error;
            if (virDomainDefSetVcpus(def, vcpus) < 0)
                goto error;
            break;
        case 'C':
            // memflags |= VM_MEM_F_INCORE;
            break;
        case 'g':
            // gdb_port = atoi(optarg);
            break;
        case 'l':
            // if (lpc_device_parse(optarg) != 0) {
            //     errx(EX_USAGE, "invalid lpc device "
            //         "configuration '%s'", optarg);
            // }
            break;
        case 's':
            // if (pci_parse_slot(optarg) != 0)
            //     exit(1);
            // else
            break;
        case 'S':
            // memflags |= VM_MEM_F_WIRED;
            break;
        case 'm':
            // error = vm_parse_memsize(optarg, &memsize);
            // if (error)
            //     errx(EX_USAGE, "invalid memsize '%s'", optarg);
            break;
        case 'H':
            // guest_vmexit_on_hlt = 1;
            break;
        case 'I':
            /*
             * The "-I" option was used to add an ioapic to the
             * virtual machine.
             *
             * An ioapic is now provided unconditionally for each
             * virtual machine and this option is now deprecated.
             */
            break;
        case 'P':
            // guest_vmexit_on_pause = 1;
            break;
        case 'e':
            // strictio = 1;
            break;
        case 'u':
            // rtc_localtime = 0;
            break;
        case 'U':
            // guest_uuid_str = optarg;
            break;
        case 'w':
            // strictmsr = 0;
            break;
        case 'W':
            // virtio_msix = 0;
            break;
        case 'x':
            // x2apic_mode = 1;
            break;
        case 'Y':
            // mptgen = 0;
            break;
        }
    }

    if (argc != optind)
        goto error;

    if (def->name == NULL)
        def->name = argv[argc];
    else if (STRNEQ(def->name, argv[argc]))
        /* the vm name of the loader and the bhyverun command differ, throw an
         * error here
         * FIXME: Print a more verbose error message. */
        goto error;

    return 0;
error:
    return -1;
}

virDomainDefPtr
bhyveParseCommandLineString(const char* nativeConfig,
                            virCapsPtr caps ATTRIBUTE_UNUSED, /* For now. */
                            virDomainXMLOptionPtr xmlopt ATTRIBUTE_UNUSED)
{
    virDomainDefPtr def = NULL;
    int bhyve_argc = 0;
    char **bhyve_argv = NULL;
    int loader_argc = 0;
    char **loader_argv = NULL;

    if (!(def = virDomainDefNew()))
        goto cleanup;

    if (bhyveCommandLine2argv(nativeConfig,
                              &loader_argc, &loader_argv,
                              &bhyve_argc, &bhyve_argv))
        goto cleanup;

    if (bhyveParseBhyveCommandLine(def, bhyve_argc, bhyve_argv))
        goto cleanup;

cleanup:
    return def;
}
