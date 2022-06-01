
#include "parse_utils.h"

#include "igraph_foreign.h"
#include "igraph_memory.h"

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_XLOCALE)
/* On some systems, xlocale.h exists, but uselocale() is still in locale.h.
 * Thus we include both. */
#include <xlocale.h>
#include <locale.h>
#else
#include <locale.h>
#endif

/* Trims whitespace from the beginning and the end of a string with a specified length.
 * A pointer to the first character of the result substring, as well as its length, are returned.
 *
 * If you have a null-terminated string, call this function as
 *
 *     igraph_i_trim_whitespace(str, strlen(str), &res, &len);
 *
 * This does not carry a performance penalty, as the end of the string would need to be
 * determinted anyway.
 */
void igraph_i_trim_whitespace(const char *str, size_t str_len, const char **res, size_t *res_len) {
    const char *beg = str, *end = str + str_len;
    while (beg < end && isspace(beg[0]) ) beg++;
    while (end > beg && isspace(end[-1])) end--;
    *res = beg;
    *res_len = end - beg;
}


/* TODO: Support for reporting line number where parse error occurred. */

/* Converts a string to an integer. Throws an error if the result is not representable.
 *
 * The input is a not-necesarily-null-terminated string that must contain only the number.
 * Any additional characters at the end of the string, such as whitespace, will trigger
 * a parsing error.
 *
 * An error is returned if the input is an empty string.
 */
igraph_error_t igraph_i_parse_integer(const char *str, size_t length, igraph_integer_t *value) {
    char buffer[128];
    char *tmp, *end;
    char last_char;
    igraph_bool_t out_of_range, dynamic_alloc;
    long long val;

    if (length == 0) {
        IGRAPH_ERROR("Cannot parse integer from empty string.", IGRAPH_PARSEERROR);
    }

    dynamic_alloc = length+1 > sizeof(buffer) / sizeof(buffer[0]);

    if (dynamic_alloc) {
        tmp = IGRAPH_CALLOC(length+1, char);
        if (tmp == NULL) {
            IGRAPH_ERROR("Failed to parse integer.", IGRAPH_ENOMEM); /* LCOV_EXCL_LINE */
        }
    } else {
        tmp = buffer;
    }

    strncpy(tmp, str, length);
    tmp[length]='\0';

    /* To avoid having to choose the appropriate strto?() function based on
     * the definition of igraph_integer_t, we first use a long long variable
     * which should be at least as large as igraph_integer_t on any platform. */
    errno = 0;
    val = strtoll(tmp, &end, 10);
    out_of_range = errno == ERANGE;
    *value = (igraph_integer_t) val;
    last_char = *end;
    if (*value != val) {
        out_of_range = 1;
    }

    /* Free memory before raising any errors. */
    if (dynamic_alloc) {
        IGRAPH_FREE(tmp);
    }

    if (out_of_range) {
        IGRAPH_ERROR("Failed to parse integer.", val > 0 ? IGRAPH_EOVERFLOW : IGRAPH_EUNDERFLOW);
    }

    /* Did we parse to the end of the string? */
    if (last_char) {
        IGRAPH_ERRORF("Unexpected character '%c' while parsing integer.", IGRAPH_PARSEERROR, last_char);
    }

    return IGRAPH_SUCCESS;
}


/* Converts a string to a real number. Throws an error if the result is not representable.
 *
 * The input is a not-necesarily-null-terminated string that must contain only the number.
 * Any additional characters at the end of the string, such as whitespace, will trigger
 * a parsing error.
 *
 * NaN and Inf are supported. An error is returned if the input is an empty string.
 */
igraph_error_t igraph_i_parse_real(const char *str, size_t length, igraph_real_t *value) {
    char buffer[128];
    char *tmp, *end;
    char last_char;
    igraph_bool_t out_of_range, dynamic_alloc;

    if (length == 0) {
        IGRAPH_ERROR("Cannot parse real number from empty string.", IGRAPH_PARSEERROR);
    }

    dynamic_alloc = length+1 > sizeof(buffer) / sizeof(buffer[0]);

    if (dynamic_alloc) {
        tmp = IGRAPH_CALLOC(length+1, char);
        if (tmp == NULL) {
            IGRAPH_ERROR("Failed to parse real number.", IGRAPH_ENOMEM); /* LCOV_EXCL_LINE */
        }
    } else {
        tmp = buffer;
    }

    strncpy(tmp, str, length);
    tmp[length]='\0';

    errno = 0;
    *value = strtod(tmp, &end);
    out_of_range = errno == ERANGE; /* This does not trigger when reading +-Inf. */
    last_char = *end;

    /* Free memory before raising any errors. */
    if (dynamic_alloc) {
        IGRAPH_FREE(tmp);
    }

    if (out_of_range) {
        IGRAPH_ERROR("Failed to parse real number.", *value > 0 ? IGRAPH_EOVERFLOW : IGRAPH_EUNDERFLOW);
    }

    /* Did we parse to the end of the string? */
    if (last_char) {
        IGRAPH_ERRORF("Unexpected character '%c' while parsing real number.", IGRAPH_PARSEERROR, last_char);
    }

    return IGRAPH_SUCCESS;
}

/* igraph_i_safelocale() and igraph_i_unsafelocale() will set the numeric locale to "C"
 * and re-set it to its original value. This is to ensure that parsing and writing
 * numbers uses a decimal point instead of a comma.
 *
 * These functions attempt to set the locale only for the current thread on a best-effort
 * basis. On some platforms this is not possible, so the global locale will be changes.
 * This is not safe to do in multi-threaded programs (not even if igraph runs only in
 * a single thread).
 */

struct igraph_safelocale_s {
#ifdef HAVE_USELOCALE
    locale_t original_locale;
    locale_t c_locale;
#else
    char    *original_locale;
# ifdef HAVE__CONFIGTHREADLOCALE
    int      per_thread_locale;
# endif
#endif
};

/**
 * \function igraph_enter_safelocale
 * \brief Temporarily set the C locale.
 *
 * \experimental
 *
 * igraph's foreign format readers and writers require a locale that uses a
 * decimal point instead of a decimal comma. This is a convenience function
 * that temporarily sets the C locale so that readers and writer would work
 * correctly. It \em must be paired with a call to \ref igraph_exit_safelocale(),
 * otherwise a memory leak will occur.
 *
 * </para><para>
 * This function tries to set the locale for the current thread only on a
 * best-effort basis. Restricting the locale change to a single thread is not
 * supported on all platforms. In these cases, this function falls back to using
 * the standard <code>setlocale()</code> function, which affects the entire process
 * and is not safe to use from concurrent threads.
 *
 * </para><para>
 * It is generally recommended to run igraph within a thread that has been
 * permanently set to the C locale using system-specific means. This is a convenience
 * function for situations when this is not easily possible because the programmer
 * is not in control of the process, such as when developing plugins/extensions.
 * Note that processes start up in the C locale by default, thus nothing needs to
 * be done unless the locale has been changed away from the default.
 *
 * \param loc Pointer to a variable of type \c igraph_safelocale_t. The current
 *     locale will be stored here, so that it can be restored using
 *     \ref igraph_exit_safelocale().
 * \return Error code.
 *
 * \example examples/simple/safelocale.c
 */

igraph_error_t igraph_enter_safelocale(igraph_safelocale_t *loc) {
    *loc = IGRAPH_CALLOC(1, struct igraph_safelocale_s);
    IGRAPH_CHECK_OOM(loc, "Could not set C locale.");
    igraph_safelocale_t l = *loc;
#ifdef HAVE_USELOCALE
    l->c_locale = newlocale(LC_NUMERIC_MASK, "C", NULL);
    if (! l->c_locale) {
        IGRAPH_ERROR("Could not set C locale.", IGRAPH_FAILURE);
    }
    l->original_locale = uselocale(l->c_locale);
#else
    l->original_locale = strdup(setlocale(LC_NUMERIC, NULL));
    if (! l->original_locale) {
        IGRAPH_ERROR("Not enough memory.", IGRAPH_ENOMEM); /* LCOV_EXCL_LINE */
    }
# ifdef HAVE__CONFIGTHREADLOCALE
    /* On Windows, we can enable per-thread locale */
    l->per_thread_locale = _configthreadlocale(0);
    _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
# endif
    setlocale(LC_NUMERIC, "C");
#endif
    return IGRAPH_SUCCESS;
}

/**
 * \function igraph_exit_safelocale
 * \brief Temporarily set the C locale.
 *
 * \experimental
 *
 * Restores a locale saved by \ref igraph_enter_safelocale() and deallocates
 * all associated data. This function \em must be paired with a call to
 * \ref igraph_enter_safelocale().
 *
 * \param loc A variable of type \c igraph_safelocale_t, originally set
 *     by \ref igraph_enter_safelocale().
 */

void igraph_exit_safelocale(igraph_safelocale_t *loc) {
    igraph_safelocale_t l = *loc;
#ifdef HAVE_USELOCALE
    uselocale(l->original_locale);
    freelocale(l->c_locale);
#else
    setlocale(LC_NUMERIC, l->original_locale);
    IGRAPH_FREE(l->original_locale);
# ifdef HAVE__CONFIGTHREADLOCALE
    /* Restore per-thread locale setting on Windows */
    _configthreadlocale(l->per_thread_locale);
# endif
#endif
    IGRAPH_FREE(*loc);
}
