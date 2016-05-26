/*
 * bhyve_parse_command.c: Bhyve command parser
 *
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
bhyveParseCommandLine2argv(const char *nativeConfig,
                           char ***loader_argv ATTRIBUTE_UNUSED,
                           char ***bhyve_argv ATTRIBUTE_UNUSED)
{
    int ret = 0;
    char *curr = NULL;
    char *nativeConfig_unescaped = NULL;
    const char *start;
    char **lines = NULL;
    size_t line_count = 0;
    size_t lines_alloc = 0;

    nativeConfig_unescaped = bhyveParseCommandLineUnescape(nativeConfig);
    if (nativeConfig_unescaped == NULL)
        goto error;

    curr = nativeConfig_unescaped;

    /* Iterate over string, splitting on sequences of '\n' */
    while (curr && *curr != '\0') {
        char *line;
        const char *next;

        start = curr;
        next = strchr(curr, '\n');

        if (VIR_STRNDUP(line, curr, next ? next - curr : -1) < 0)
            goto error;

        if (VIR_RESIZE_N(lines, lines_alloc, linecount, 2) < 0) {
            VIR_FREE(line);
            goto error;
        }

        lines[argcount++] = line;
        lines[argcount] = NULL;

        while (next && (*next == '\n' || *next == '\r'
                        || STRPREFIX(next, "\r\n")))
            next++;

        curr = next;
    }

    return 0;

 error:
    VIR_FREE(loader_argv);
    VIR_FREE(bhyve_argc);
    virStringFreeList(lines);
    return -1;
}

virDomainDefPtr
bhyveParseCommandLineString(const char* nativeConfig,
                            virCapsPtr caps ATTRIBUTE_UNUSED, /* For now. */
                            virDomainXMLOptionPtr xmlopt ATTRIBUTE_UNUSED)
{
    virDomainDefPtr def = NULL;
    char **bhyve_argv = NULL;
    char **loader_argv = NULL;

    if (bhyveCommandLine2argv(nativeConfig, &bhyve_argv, &loader_argv))
        goto cleanup;

cleanup:
    return def;
}
