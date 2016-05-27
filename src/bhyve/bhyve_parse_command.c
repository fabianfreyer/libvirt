/*
 * bhyve_parse_command.c: Bhyve command parser
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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
    if (nativeConfig_unescaped == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to unescape command line string"));
        goto error;
    }

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

static int
bhyveParseBhyveLPCArg(virDomainDefPtr def,
                      unsigned caps ATTRIBUTE_UNUSED,
                      const char *arg)
{
    const char *separator = NULL;
    const char *param = NULL;
    size_t last = 0;
    virDomainChrDefPtr chr = NULL;
    char *type = NULL;

    separator = strchr(arg, ',');
    param = separator + 1;

    if (!separator)
        goto error;

    if (VIR_STRNDUP(type, arg, separator - arg) < 0)
        goto error;

    /* Only support com%d */
    if (STRPREFIX(type, "com") && type[4] == 0) {
        if (!(chr = virDomainChrDefNew()))
            goto error;

        chr->source.type = VIR_DOMAIN_CHR_TYPE_NMDM;
        chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;

        if (!STRPREFIX(param, "/dev/nmdm")) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Failed to set com port %s: does not start with "
                             "'/dev/nmdm'."), type);
                goto error;
        }

        if (VIR_STRDUP(chr->source.data.file.path, param) < 0) {
            virDomainChrDefFree(chr);
            goto error;
        }

        if (VIR_STRDUP(chr->source.data.nmdm.slave, chr->source.data.file.path)
            < 0) {
            virDomainChrDefFree(chr);
            goto error;
        }

        /* If the last character of the master is 'A', the slave will be 'B'
         * and vice versa */
        last = strlen(chr->source.data.file.path) - 1;
        switch (chr->source.data.file.path[last]) {
            case 'A':
                chr->source.data.file.path[last] = 'B';
                break;
            case 'B':
                chr->source.data.file.path[last] = 'A';
                break;
            default:
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("Failed to set slave for %s: last letter not "
                                 "'A' or 'B'"),
                               chr->source.data.file.path);
                goto error;
        }

        switch(type[3]-'0') {
        case 1:
        case 2:
            chr->target.port = type[3] - '1';
            break;
        default:
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Failed to parse %s: only com1 and com2"
                             "supported."), type);
            virDomainChrDefFree(chr);
            goto error;
            break;
        }

        if (VIR_APPEND_ELEMENT(def->serials, def->nserials, chr) < 0) {
            virDomainChrDefFree(chr);
            goto error;
        }
    }

    VIR_FREE(type);
    return 0;

error:
    VIR_FREE(chr);
    VIR_FREE(type);
    return -1;
}

/*
 * Parse the /usr/bin/bhyve command line. */
static int
bhyveParseBhyveCommandLine(virDomainDefPtr def,
                           unsigned caps,
                           int argc, char **argv)
{
    int c;
    const char optstr[] = "abehuwxACHIPSWYp:g:c:s:m:l:U:";
    int vcpus = 1;
    size_t memory = 0;

    if (!argv)
        goto error;

    optind = 1;
    while ((c = getopt(argc, argv, optstr)) != -1) {
        switch (c) {
        case 'A':
            def->features[VIR_DOMAIN_FEATURE_ACPI] = VIR_TRISTATE_SWITCH_ON;
            break;
        case 'c':
            if (virStrToLong_i(optarg, NULL, 10, &vcpus) < 0) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("Failed to parse number of vCPUs."));
                goto error;
            }
            if (virDomainDefSetVcpusMax(def, vcpus) < 0)
                goto error;
            if (virDomainDefSetVcpus(def, vcpus) < 0)
                goto error;
            break;
        case 'l':
            if (bhyveParseBhyveLPCArg(def, caps, optarg))
                goto error;
            break;
        case 's':
            // FIXME: Implement.
            break;
        case 'm':
            if (virStrToLong_ul(optarg, NULL, 10, &memory) < 0) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("Failed to parse Memory."));
                goto error;
            }
            /* For compatibility reasons, assume memory is givin in MB
             * when < 1024, otherwise it is given in bytes */
            if (memory < 1024)
                memory *= 1024;
            else
                memory /= 1024UL;
            if (def->mem.cur_balloon != 0 && def->mem.cur_balloon != memory) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Failed to parse Memory: Memory size mismatch."));
                goto error;
            }
            def->mem.cur_balloon = memory;
            virDomainDefSetMemoryTotal(def, memory);
            break;
            break;
        case 'I':
            /* While this flag was deprecated in FreeBSD r257423, keep checking
             * for it for backwards compatibility. */
            def->features[VIR_DOMAIN_FEATURE_APIC] = VIR_TRISTATE_SWITCH_ON;
            break;
        case 'u':
            if ((caps & BHYVE_CAP_RTC_UTC) != 0) {
                def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_UTC;
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Installed bhyve binary does not support "
                              "UTC clock"));
                goto error;
            }
            break;
        case 'U':
            if (virUUIDParse(optarg, def->uuid) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, \
                               _("cannot parse UUID '%s'"), optarg);
                goto error;
            }
            break;
        }
    }

    if (argc != optind) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Failed to parse arguments for bhyve command."));
        goto error;
    }

    if (def->name == NULL)
        def->name = argv[argc];
    else if (STRNEQ(def->name, argv[argc])) {
        /* the vm name of the loader and the bhyverun command differ, throw an
         * error here */
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Failed to parse arguments: VM name mismatch."));
        goto error;
    }

    return 0;
error:
    return -1;
}

virDomainDefPtr
bhyveParseCommandLineString(const char* nativeConfig,
                            unsigned caps,
                            virDomainXMLOptionPtr xmlopt ATTRIBUTE_UNUSED)
{
    virDomainDefPtr def = NULL;
    int bhyve_argc = 0;
    char **bhyve_argv = NULL;
    int loader_argc = 0;
    char **loader_argv = NULL;

    if (!(def = virDomainDefNew()))
        goto cleanup;

    // Initialize defaults.
    if (virUUIDGenerate(def->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to generate uuid"));
        goto cleanup;
    }
    def->id = -1;
    def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME;

    if (bhyveCommandLine2argv(nativeConfig,
                              &loader_argc, &loader_argv,
                              &bhyve_argc, &bhyve_argv)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Failed to convert the command string to argv-lists.."));
        goto cleanup;
    }

    if (bhyveParseBhyveCommandLine(def, caps, bhyve_argc, bhyve_argv))
        goto cleanup;

cleanup:
    return def;
}
