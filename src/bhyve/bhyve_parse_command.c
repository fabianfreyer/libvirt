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

        if (STREQ(arglist[0], "/usr/sbin/bhyve")) {
            if ((VIR_REALLOC_N(_bhyve_argv, args_count + 1) < 0)
                || (!bhyve_argc))
                goto error;
            for (j = 0; j < args_count; j++)
                _bhyve_argv[j] = arglist[j];
            _bhyve_argv[j] = NULL;
            *bhyve_argc = args_count-1;
        }
        else if (STREQ(arglist[0], "/usr/sbin/bhyveload")
                 || STREQ(arglist[0], "/usr/sbin/grub-bhyve")) {
            if ((VIR_REALLOC_N(_loader_argv, args_count + 1) < 0)
                || (!loader_argc))
                goto error;
            for (j = 0; j < args_count; j++)
                _loader_argv[j] = arglist[j];
            _loader_argv[j] = NULL;
            *loader_argc = args_count-1;
        }
        virStringFreeList(arglist);
    }

    *loader_argv = _loader_argv;
    *bhyve_argv = _bhyve_argv;

    virStringFreeList(lines);
    return 0;

 error:
    virStringFreeList(_loader_argv);
    virStringFreeList(_bhyve_argv);
    virStringFreeList(lines);
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

    if (bhyveCommandLine2argv(nativeConfig,
                              &loader_argc, &loader_argv,
                              &bhyve_argc, &bhyve_argv))
        goto cleanup;

cleanup:
    return def;
}
