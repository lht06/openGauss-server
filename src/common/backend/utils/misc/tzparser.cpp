/* -------------------------------------------------------------------------
 *
 * tzparser.c
 *	  Functions for parsing timezone offset files
 *
 * Note: this code is invoked from the check_hook for the GUC variable
 * timezone_abbreviations.	Therefore, it should report problems using
 * GUC_check_errmsg() and related functions, and try to avoid throwing
 * elog(ERROR).  This is not completely bulletproof at present --- in
 * particular out-of-memory will throw an error.  Could probably fix with
 * PG_TRY if necessary.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/tzparser.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "knl/knl_variable.h"

#include <ctype.h>

#include "miscadmin.h"
#include "catalog/pg_collation.h"
#include "storage/smgr/fd.h"
#include "regex/regex.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/tzparser.h"
#include "threadpool/threadpool.h"

#define WHITESPACE " \t\n\r"
constexpr int LIST_LENGTH_MIN = 2;

static bool validateTzEntry(tzEntry* tzentry);
static bool splitTzLine(const char* filename, int lineno, char* line, tzEntry* tzentry);
static int addToArray(tzEntry** base, int* arraysize, int n, tzEntry* entry, bool override);
static int ParseTzFile(const char* filename, int depth, tzEntry** base, int* arraysize, int n);

/*
 * Apply additional validation checks to a tzEntry
 *
 * Returns TRUE if OK, else false
 */
static bool validateTzEntry(tzEntry* tzentry)
{
    unsigned char* p = NULL;

    /*
     * Check restrictions imposed by datetkntbl storage format (see
     * datetime.c)
     */
    if (strlen(tzentry->abbrev) > TOKMAXLEN) {
        GUC_check_errmsg(
            "time zone abbreviation \"%s\" is too long (maximum %d characters) in time zone file \"%s\", line %d",
            tzentry->abbrev,
            TOKMAXLEN,
            tzentry->filename,
            tzentry->lineno);
        return false;
    }
    if (tzentry->offset % 900 != 0) {
        GUC_check_errmsg("time zone offset %d is not a multiple of 900 sec (15 min) in time zone file \"%s\", line %d",
            tzentry->offset,
            tzentry->filename,
            tzentry->lineno);
        return false;
    }

    /*
     * Sanity-check the offset: shouldn't exceed 14 hours
     */
    if (tzentry->offset > 14 * 60 * 60 || tzentry->offset < -14 * 60 * 60) {
        GUC_check_errmsg("time zone offset %d is out of range in time zone file \"%s\", line %d",
            tzentry->offset,
            tzentry->filename,
            tzentry->lineno);
        return false;
    }

    /*
     * Convert abbrev to lowercase (must match datetime.c's conversion)
     */
    for (p = (unsigned char*)tzentry->abbrev; *p; p++)
        *p = pg_tolower(*p);

    return true;
}

/*
 * Attempt to parse the line as a timezone abbrev spec (name, offset, dst)
 *
 * Returns TRUE if OK, else false; data is stored in *tzentry
 */
static bool splitTzLine(const char* filename, int lineno, char* line, tzEntry* tzentry)
{
    char* abbrev = NULL;
    char* offset = NULL;
    char* offset_endptr = NULL;
    char* remain = NULL;
    char* is_dst = NULL;
    char* tmp_token = NULL;

    tzentry->lineno = lineno;
    tzentry->filename = filename;
    abbrev = strtok_r(line, WHITESPACE, &tmp_token);
    if (abbrev == NULL) {
        GUC_check_errmsg("missing time zone abbreviation in time zone file \"%s\", line %d", filename, lineno);
        return false;
    }
    tzentry->abbrev = abbrev;
    offset = strtok_r(NULL, WHITESPACE, &tmp_token);
    if (offset == NULL) {
        GUC_check_errmsg("missing time zone offset in time zone file \"%s\", line %d", filename, lineno);
        return false;
    }
    tzentry->offset = strtol(offset, &offset_endptr, 10);
    if (offset_endptr == offset || *offset_endptr != '\0') {
        GUC_check_errmsg("invalid number for time zone offset in time zone file \"%s\", line %d", filename, lineno);
        return false;
    }
    is_dst = strtok_r(NULL, WHITESPACE, &tmp_token);
    if (is_dst && pg_strcasecmp(is_dst, "D") == 0) {
        tzentry->is_dst = true;
        remain = strtok_r(NULL, WHITESPACE, &tmp_token);
    } else {
        /* there was no 'D' dst specifier */
        tzentry->is_dst = false;
        remain = is_dst;
    }

    if (remain == NULL) /* no more non-whitespace chars */
        return true;

    if (remain[0] != '#') /* must be a comment */
    {
        GUC_check_errmsg("invalid syntax in time zone file \"%s\", line %d", filename, lineno);
        return false;
    }
    return true;
}

/*
 * Insert entry into sorted array
 *
 * *base: base address of array (changeable if must enlarge array)
 * *arraysize: allocated length of array (changeable if must enlarge array)
 * n: current number of valid elements in array
 * entry: new data to insert
 * override: TRUE if OK to override
 *
 * Returns the new array length (new value for n), or -1 if error
 */
static int addToArray(tzEntry** base, int* arraysize, int n, tzEntry* entry, bool override)
{
    tzEntry* arrayptr = NULL;
    int low;
    int high;

    /*
     * Search the array for a duplicate; as a useful side effect, the array is
     * maintained in sorted order.	We use strcmp() to ensure we match the
     * sort order datetime.c expects.
     */
    arrayptr = *base;
    low = 0;
    high = n - 1;
    while (low <= high) {
        int mid = (unsigned int)(low + high) >> 1;
        tzEntry* midptr = arrayptr + mid;
        int cmp;

        cmp = strcmp(entry->abbrev, midptr->abbrev);
        if (cmp < 0)
            high = mid - 1;
        else if (cmp > 0)
            low = mid + 1;
        else {
            /*
             * Found a duplicate entry; complain unless it's the same.
             */
            if (midptr->offset == entry->offset && midptr->is_dst == entry->is_dst) {
                /* return unchanged array */
                return n;
            }
            if (override) {
                /* same abbrev but something is different, override */
                midptr->offset = entry->offset;
                midptr->is_dst = entry->is_dst;
                return n;
            }
            /* same abbrev but something is different, complain */
            GUC_check_errmsg("time zone abbreviation \"%s\" is multiply defined", entry->abbrev);
            GUC_check_errdetail(
                "Entry in time zone file \"%s\", line %d, conflicts with entry in file \"%s\", line %d.",
                midptr->filename,
                midptr->lineno,
                entry->filename,
                entry->lineno);
            return -1;
        }
    }

    /*
     * No match, insert at position "low".
     */
    if (n >= *arraysize) {
        *arraysize *= 2;
        *base = (tzEntry*)repalloc(*base, *arraysize * sizeof(tzEntry));
    }

    arrayptr = *base + low;
    uint32 max_array_size = *arraysize * sizeof(tzEntry);

    errno_t rc = memmove_s(arrayptr + 1, max_array_size - sizeof(tzEntry), arrayptr, (n - low) * sizeof(tzEntry));
    securec_check(rc, "\0", "\0");

    rc = memcpy_s(arrayptr, max_array_size, entry, sizeof(tzEntry));
    securec_check(rc, "\0", "\0");

    /* Must dup the abbrev to ensure it survives */
    arrayptr->abbrev = pstrdup(entry->abbrev);

    return n + 1;
}

/*
 * Parse a single timezone abbrev file --- can recurse to handle @INCLUDE
 *
 * filename: user-specified file name (does not include path)
 * depth: current recursion depth
 * *base: array for results (changeable if must enlarge array)
 * *arraysize: allocated length of array (changeable if must enlarge array)
 * n: current number of valid elements in array
 *
 * Returns the new array length (new value for n), or -1 if error
 */
static int ParseTzFile(const char* filename, int depth, tzEntry** base, int* arraysize, int n)
{
    char share_path[MAXPGPATH];
    char file_path[MAXPGPATH];
    FILE* tzFile = NULL;
    char tzbuf[1024];
    char* line = NULL;
    tzEntry tzentry;
    int lineno = 0;
    bool override = false;
    const char* p = NULL;
    char* tmp_token = NULL;

    /*
     * We enforce that the filename is all alpha characters.  This may be
     * overly restrictive, but we don't want to allow access to anything
     * outside the timezonesets directory, so for instance '/' *must* be
     * rejected.
     */
    for (p = filename; *p; p++) {
        if (!isalpha((unsigned char)*p)) {
            /* at level 0, just use guc.c's regular "invalid value" message */
            if (depth > 0)
                GUC_check_errmsg("invalid time zone file name \"%s\"", filename);
            return -1;
        }
    }

    /*
     * The maximal recursion depth is a pretty arbitrary setting. It is hard
     * to imagine that someone needs more than 3 levels so stick with this
     * conservative setting until someone complains.
     */
    if (depth > 3) {
        GUC_check_errmsg("time zone file recursion limit exceeded in file \"%s\"", filename);
        return -1;
    }

    get_share_path(my_exec_path, share_path);
    errno_t rc = snprintf_s(file_path, MAXPGPATH, MAXPGPATH - 1, "%s/timezonesets/%s", share_path, filename);
    securec_check_ss(rc, "\0", "\0");
    tzFile = AllocateFile(file_path, "r");
    if (tzFile == NULL) {
        /*
         * Check to see if the problem is not the filename but the directory.
         * This is worth troubling over because if the installation share/
         * directory is missing or unreadable, this is likely to be the first
         * place we notice a problem during postmaster startup.
         */
        int save_errno = errno;
        DIR* tzdir = NULL;

        rc = snprintf_s(file_path, MAXPGPATH, sizeof(file_path) - 1, "%s/timezonesets", share_path);
        securec_check_ss_c(rc, "\0", "\0");
        tzdir = AllocateDir(file_path);
        if (tzdir == NULL) {
            GUC_check_errmsg("could not open directory \"%s\": %m", file_path);
            GUC_check_errhint("This may indicate an incomplete PostgreSQL installation, or that the file \"%s\" has "
                              "been moved away from its proper location.",
                my_exec_path);
            return -1;
        }
        FreeDir(tzdir);
        errno = save_errno;

        /*
         * otherwise, if file doesn't exist and it's level 0, guc.c's
         * complaint is enough
         */
        if (errno != ENOENT || depth > 0)
            GUC_check_errmsg("could not read time zone file \"%s\": %m", filename);

        return -1;
    }

    while (!feof(tzFile)) {
        lineno++;
        if (fgets(tzbuf, sizeof(tzbuf), tzFile) == NULL) {
            if (ferror(tzFile)) {
                GUC_check_errmsg("could not read time zone file \"%s\": %m", filename);
                return -1;
            }
            /* else we're at EOF after all */
            break;
        }
        if (strlen(tzbuf) == sizeof(tzbuf) - 1) {
            /* the line is too long for tzbuf */
            GUC_check_errmsg("line is too long in time zone file \"%s\", line %d", filename, lineno);
            return -1;
        }

        /* skip over whitespace */
        line = tzbuf;
        while (*line && isspace((unsigned char)*line))
            line++;

        if (*line == '\0') /* empty line */
            continue;
        if (*line == '#') /* comment line */
            continue;

        if (pg_strncasecmp(line, "@INCLUDE", strlen("@INCLUDE")) == 0) {
            /* pstrdup so we can use filename in result data structure */
            char* includeFile = pstrdup(line + strlen("@INCLUDE"));
            includeFile = strtok_r(includeFile, WHITESPACE, &tmp_token);
            if (includeFile == NULL || !*includeFile) {
                GUC_check_errmsg("@INCLUDE without file name in time zone file \"%s\", line %d", filename, lineno);
                return -1;
            }
            n = ParseTzFile(includeFile, depth + 1, base, arraysize, n);
            if (n < 0)
                return -1;
            continue;
        }

        if (pg_strncasecmp(line, "@OVERRIDE", strlen("@OVERRIDE")) == 0) {
            override = true;
            continue;
        }

        if (!splitTzLine(filename, lineno, line, &tzentry))
            return -1;
        if (!validateTzEntry(&tzentry))
            return -1;
        n = addToArray(base, arraysize, n, &tzentry, override);
        if (n < 0)
            return -1;
    }

    FreeFile(tzFile);

    return n;
}

/*
 * load_tzoffsets --- read and parse the specified timezone offset file
 *
 * On success, return a filled-in TimeZoneAbbrevTable, which must have been
 * malloc'd not palloc'd.  On failure, return NULL, using GUC_check_errmsg
 * and friends to give details of the problem.
 */
TimeZoneAbbrevTable* load_tzoffsets(const char* filename)
{
    TimeZoneAbbrevTable* result = NULL;
    MemoryContext tmpContext;
    MemoryContext oldContext;
    tzEntry* array = NULL;
    int arraysize;
    int n;

    /*
     * Create a temp memory context to work in.  This makes it easy to clean
     * up afterwards.
     */
    tmpContext = AllocSetContextCreate(CurrentMemoryContext,
        "TZParserMemory",
        ALLOCSET_SMALL_MINSIZE,
        ALLOCSET_SMALL_INITSIZE,
        ALLOCSET_SMALL_MAXSIZE);
    oldContext = MemoryContextSwitchTo(tmpContext);

    /* Initialize array at a reasonable size */
    arraysize = 128;
    array = (tzEntry*)palloc(arraysize * sizeof(tzEntry));

    /* Parse the file(s) */
    n = ParseTzFile(filename, 0, &array, &arraysize, 0);

    /* If no errors so far, allocate result and let datetime.c convert data */
    if (n >= 0) {
        result = (TimeZoneAbbrevTable*)MemoryContextAlloc(SESS_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_EXECUTOR),
            (offsetof(TimeZoneAbbrevTable, abbrevs) + n * sizeof(datetkn)));
        if (result == NULL)
            GUC_check_errmsg("out of memory");
        else
            ConvertTimeZoneAbbrevs(result, array, n);
    }

    /* Clean up */
    MemoryContextSwitchTo(oldContext);
    MemoryContextDelete(tmpContext);

    return result;
}

/** To test the source whether satify the pattern, if yes, return the rest of.
    otherwise, return null;
 */
char* pg_findformat(const char* key, const char* source)
{
    List* elemlist = NULL;
    ListCell* l = NULL;

    if (source == NULL || source[0] == '\0') {
        return NULL; /* nothing to do */
    }

    auto source_dup = pstrdup(source);
    /* Parse string into list of identifiers */
    if (!SplitIdentifierString((char*)source_dup, '=', &elemlist, false, false) ||
                                list_length(elemlist) != LIST_LENGTH_MIN) {
        /* syntax error in list */
        pfree(source_dup);
        list_free(elemlist);
        return NULL;
    }

    l = list_head(elemlist);
    char* tok = (char*)lfirst(l);
    if (strcmp(tok, key) == 0) {
        char* ret = pstrdup((char *)lsecond(elemlist));
        pfree(source_dup);
        list_free(elemlist);
        return ret;
    }

    pfree(source_dup);
    list_free(elemlist);
    return NULL;
}