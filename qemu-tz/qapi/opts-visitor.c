/*
 * Options Visitor
 *
 * Copyright Red Hat, Inc. 2012, 2013
 *
 * Author: Laszlo Ersek <lersek@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "qemu-common.h"
#include "qapi/qmp/qerror.h"
#include "qapi/opts-visitor.h"
#include "qemu/queue.h"
#include "qemu/option_int.h"
#include "qapi/visitor-impl.h"


enum ListMode
{
    LM_NONE,             /* not traversing a list of repeated options */
    LM_STARTED,          /* opts_start_list() succeeded */

    LM_IN_PROGRESS,      /* opts_next_list() has been called.
                          *
                          * Generating the next list link will consume the most
                          * recently parsed QemuOpt instance of the repeated
                          * option.
                          *
                          * Parsing a value into the list link will examine the
                          * next QemuOpt instance of the repeated option, and
                          * possibly enter LM_SIGNED_INTERVAL or
                          * LM_UNSIGNED_INTERVAL.
                          */

    LM_SIGNED_INTERVAL,  /* opts_next_list() has been called.
                          *
                          * Generating the next list link will consume the most
                          * recently stored element from the signed interval,
                          * parsed from the most recent QemuOpt instance of the
                          * repeated option. This may consume QemuOpt itself
                          * and return to LM_IN_PROGRESS.
                          *
                          * Parsing a value into the list link will store the
                          * next element of the signed interval.
                          */

    LM_UNSIGNED_INTERVAL /* Same as above, only for an unsigned interval. */
};

typedef enum ListMode ListMode;

struct OptsVisitor
{
    Visitor visitor;

    /* Ownership remains with opts_visitor_new()'s caller. */
    const QemuOpts *opts_root;

    unsigned depth;

    /* Non-null iff depth is positive. Each key is a QemuOpt name. Each value
     * is a non-empty GQueue, enumerating all QemuOpt occurrences with that
     * name. */
    GHashTable *unprocessed_opts;

    /* The list currently being traversed with opts_start_list() /
     * opts_next_list(). The list must have a struct element type in the
     * schema, with a single mandatory scalar member. */
    ListMode list_mode;
    GQueue *repeated_opts;

    /* When parsing a list of repeating options as integers, values of the form
     * "a-b", representing a closed interval, are allowed. Elements in the
     * range are generated individually.
     */
    union {
        int64_t s;
        uint64_t u;
    } range_next, range_limit;

    /* If "opts_root->id" is set, reinstantiate it as a fake QemuOpt for
     * uniformity. Only its "name" and "str" fields are set. "fake_id_opt" does
     * not survive or escape the OptsVisitor object.
     */
    QemuOpt *fake_id_opt;
};


static void
destroy_list(gpointer list)
{
  g_queue_free(list);
}


static void
opts_visitor_insert(GHashTable *unprocessed_opts, const QemuOpt *opt)
{
    GQueue *list;

    list = g_hash_table_lookup(unprocessed_opts, opt->name);
    if (list == NULL) {
        list = g_queue_new();

        /* GHashTable will never try to free the keys -- we supply NULL as
         * "key_destroy_func" in opts_start_struct(). Thus cast away key
         * const-ness in order to suppress gcc's warning.
         */
        g_hash_table_insert(unprocessed_opts, (gpointer)opt->name, list);
    }

    /* Similarly, destroy_list() doesn't call g_queue_free_full(). */
    g_queue_push_tail(list, (gpointer)opt);
}


static void
opts_start_struct(Visitor *v, void **obj, const char *kind,
                  const char *name, size_t size, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    const QemuOpt *opt;

    if (obj) {
        *obj = g_malloc0(size > 0 ? size : 1);
    }
    if (ov->depth++ > 0) {
        return;
    }

    ov->unprocessed_opts = g_hash_table_new_full(&g_str_hash, &g_str_equal,
                                                 NULL, &destroy_list);
    QTAILQ_FOREACH(opt, &ov->opts_root->head, next) {
        /* ensured by qemu-option.c::opts_do_parse() */
        assert(strcmp(opt->name, "id") != 0);

        opts_visitor_insert(ov->unprocessed_opts, opt);
    }

    if (ov->opts_root->id != NULL) {
        ov->fake_id_opt = g_malloc0(sizeof *ov->fake_id_opt);

        ov->fake_id_opt->name = "id";
        ov->fake_id_opt->str = ov->opts_root->id;
        opts_visitor_insert(ov->unprocessed_opts, ov->fake_id_opt);
    }
}


static gboolean
ghr_true(gpointer ign_key, gpointer ign_value, gpointer ign_user_data)
{
    return TRUE;
}


static void
opts_end_struct(Visitor *v, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    GQueue *any;

    if (--ov->depth > 0) {
        return;
    }

    /* we should have processed all (distinct) QemuOpt instances */
    any = g_hash_table_find(ov->unprocessed_opts, &ghr_true, NULL);
    if (any) {
        const QemuOpt *first;

        first = g_queue_peek_head(any);
        error_set(errp, QERR_INVALID_PARAMETER, first->name);
    }
    g_hash_table_destroy(ov->unprocessed_opts);
    ov->unprocessed_opts = NULL;
    g_free(ov->fake_id_opt);
    ov->fake_id_opt = NULL;
}


static GQueue *
lookup_distinct(const OptsVisitor *ov, const char *name, Error **errp)
{
    GQueue *list;

    list = g_hash_table_lookup(ov->unprocessed_opts, name);
    if (!list) {
        error_set(errp, QERR_MISSING_PARAMETER, name);
    }
    return list;
}


static void
opts_start_list(Visitor *v, const char *name, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);

    /* we can't traverse a list in a list */
    assert(ov->list_mode == LM_NONE);
    ov->repeated_opts = lookup_distinct(ov, name, errp);
    if (ov->repeated_opts != NULL) {
        ov->list_mode = LM_STARTED;
    }
}


static GenericList *
opts_next_list(Visitor *v, GenericList **list, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    GenericList **link;

    switch (ov->list_mode) {
    case LM_STARTED:
        ov->list_mode = LM_IN_PROGRESS;
        link = list;
        break;

    case LM_SIGNED_INTERVAL:
    case LM_UNSIGNED_INTERVAL:
        link = &(*list)->next;

        if (ov->list_mode == LM_SIGNED_INTERVAL) {
            if (ov->range_next.s < ov->range_limit.s) {
                ++ov->range_next.s;
                break;
            }
        } else if (ov->range_next.u < ov->range_limit.u) {
            ++ov->range_next.u;
            break;
        }
        ov->list_mode = LM_IN_PROGRESS;
        /* range has been completed, fall through in order to pop option */

    case LM_IN_PROGRESS: {
        const QemuOpt *opt;

        opt = g_queue_pop_head(ov->repeated_opts);
        if (g_queue_is_empty(ov->repeated_opts)) {
            g_hash_table_remove(ov->unprocessed_opts, opt->name);
            return NULL;
        }
        link = &(*list)->next;
        break;
    }

    default:
        abort();
    }

    *link = g_malloc0(sizeof **link);
    return *link;
}


static void
opts_end_list(Visitor *v, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);

    assert(ov->list_mode == LM_STARTED ||
           ov->list_mode == LM_IN_PROGRESS ||
           ov->list_mode == LM_SIGNED_INTERVAL ||
           ov->list_mode == LM_UNSIGNED_INTERVAL);
    ov->repeated_opts = NULL;
    ov->list_mode = LM_NONE;
}


static const QemuOpt *
lookup_scalar(const OptsVisitor *ov, const char *name, Error **errp)
{
    if (ov->list_mode == LM_NONE) {
        GQueue *list;

        /* the last occurrence of any QemuOpt takes effect when queried by name
         */
        list = lookup_distinct(ov, name, errp);
        return list ? g_queue_peek_tail(list) : NULL;
    }
    assert(ov->list_mode == LM_IN_PROGRESS);
    return g_queue_peek_head(ov->repeated_opts);
}


static void
processed(OptsVisitor *ov, const char *name)
{
    if (ov->list_mode == LM_NONE) {
        g_hash_table_remove(ov->unprocessed_opts, name);
        return;
    }
    assert(ov->list_mode == LM_IN_PROGRESS);
    /* do nothing */
}


static void
opts_type_str(Visitor *v, char **obj, const char *name, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    const QemuOpt *opt;

    opt = lookup_scalar(ov, name, errp);
    if (!opt) {
        return;
    }
    *obj = g_strdup(opt->str ? opt->str : "");
    processed(ov, name);
}


/* mimics qemu-option.c::parse_option_bool() */
static void
opts_type_bool(Visitor *v, bool *obj, const char *name, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    const QemuOpt *opt;

    opt = lookup_scalar(ov, name, errp);
    if (!opt) {
        return;
    }

    if (opt->str) {
        if (strcmp(opt->str, "on") == 0 ||
            strcmp(opt->str, "yes") == 0 ||
            strcmp(opt->str, "y") == 0) {
            *obj = true;
        } else if (strcmp(opt->str, "off") == 0 ||
            strcmp(opt->str, "no") == 0 ||
            strcmp(opt->str, "n") == 0) {
            *obj = false;
        } else {
            error_set(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
                "on|yes|y|off|no|n");
            return;
        }
    } else {
        *obj = true;
    }

    processed(ov, name);
}


static void
opts_type_int(Visitor *v, int64_t *obj, const char *name, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    const QemuOpt *opt;
    const char *str;
    long long val;
    char *endptr;

    if (ov->list_mode == LM_SIGNED_INTERVAL) {
        *obj = ov->range_next.s;
        return;
    }

    opt = lookup_scalar(ov, name, errp);
    if (!opt) {
        return;
    }
    str = opt->str ? opt->str : "";

    /* we've gotten past lookup_scalar() */
    assert(ov->list_mode == LM_NONE || ov->list_mode == LM_IN_PROGRESS);

    errno = 0;
    val = strtoll(str, &endptr, 0);
    if (errno == 0 && endptr > str && INT64_MIN <= val && val <= INT64_MAX) {
        if (*endptr == '\0') {
            *obj = val;
            processed(ov, name);
            return;
        }
        if (*endptr == '-' && ov->list_mode == LM_IN_PROGRESS) {
            long long val2;

            str = endptr + 1;
            val2 = strtoll(str, &endptr, 0);
            if (errno == 0 && endptr > str && *endptr == '\0' &&
                INT64_MIN <= val2 && val2 <= INT64_MAX && val <= val2 &&
                (val > INT64_MAX - OPTS_VISITOR_RANGE_MAX ||
                 val2 < val + OPTS_VISITOR_RANGE_MAX)) {
                ov->range_next.s = val;
                ov->range_limit.s = val2;
                ov->list_mode = LM_SIGNED_INTERVAL;

                /* as if entering on the top */
                *obj = ov->range_next.s;
                return;
            }
        }
    }
    error_set(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
              (ov->list_mode == LM_NONE) ? "an int64 value" :
                                           "an int64 value or range");
}


static void
opts_type_uint64(Visitor *v, uint64_t *obj, const char *name, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    const QemuOpt *opt;
    const char *str;
    unsigned long long val;
    char *endptr;

    if (ov->list_mode == LM_UNSIGNED_INTERVAL) {
        *obj = ov->range_next.u;
        return;
    }

    opt = lookup_scalar(ov, name, errp);
    if (!opt) {
        return;
    }
    str = opt->str;

    /* we've gotten past lookup_scalar() */
    assert(ov->list_mode == LM_NONE || ov->list_mode == LM_IN_PROGRESS);

    if (parse_uint(str, &val, &endptr, 0) == 0 && val <= UINT64_MAX) {
        if (*endptr == '\0') {
            *obj = val;
            processed(ov, name);
            return;
        }
        if (*endptr == '-' && ov->list_mode == LM_IN_PROGRESS) {
            unsigned long long val2;

            str = endptr + 1;
            if (parse_uint_full(str, &val2, 0) == 0 &&
                val2 <= UINT64_MAX && val <= val2 &&
                val2 - val < OPTS_VISITOR_RANGE_MAX) {
                ov->range_next.u = val;
                ov->range_limit.u = val2;
                ov->list_mode = LM_UNSIGNED_INTERVAL;

                /* as if entering on the top */
                *obj = ov->range_next.u;
                return;
            }
        }
    }
    error_set(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
              (ov->list_mode == LM_NONE) ? "a uint64 value" :
                                           "a uint64 value or range");
}


static void
opts_type_size(Visitor *v, uint64_t *obj, const char *name, Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);
    const QemuOpt *opt;
    int64_t val;
    char *endptr;

    opt = lookup_scalar(ov, name, errp);
    if (!opt) {
        return;
    }

    val = strtosz_suffix(opt->str ? opt->str : "", &endptr,
                         STRTOSZ_DEFSUFFIX_B);
    if (val < 0 || *endptr) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
                  "a size value representible as a non-negative int64");
        return;
    }

    *obj = val;
    processed(ov, name);
}


static void
opts_start_optional(Visitor *v, bool *present, const char *name,
                       Error **errp)
{
    OptsVisitor *ov = DO_UPCAST(OptsVisitor, visitor, v);

    /* we only support a single mandatory scalar field in a list node */
    assert(ov->list_mode == LM_NONE);
    *present = (lookup_distinct(ov, name, NULL) != NULL);
}


OptsVisitor *
opts_visitor_new(const QemuOpts *opts)
{
    OptsVisitor *ov;

    ov = g_malloc0(sizeof *ov);

    ov->visitor.start_struct = &opts_start_struct;
    ov->visitor.end_struct   = &opts_end_struct;

    ov->visitor.start_list = &opts_start_list;
    ov->visitor.next_list  = &opts_next_list;
    ov->visitor.end_list   = &opts_end_list;

    /* input_type_enum() covers both "normal" enums and union discriminators.
     * The union discriminator field is always generated as "type"; it should
     * match the "type" QemuOpt child of any QemuOpts.
     *
     * input_type_enum() will remove the looked-up key from the
     * "unprocessed_opts" hash even if the lookup fails, because the removal is
     * done earlier in opts_type_str(). This should be harmless.
     */
    ov->visitor.type_enum = &input_type_enum;

    ov->visitor.type_int    = &opts_type_int;
    ov->visitor.type_uint64 = &opts_type_uint64;
    ov->visitor.type_size   = &opts_type_size;
    ov->visitor.type_bool   = &opts_type_bool;
    ov->visitor.type_str    = &opts_type_str;

    /* type_number() is not filled in, but this is not the first visitor to
     * skip some mandatory methods... */

    ov->visitor.start_optional = &opts_start_optional;

    ov->opts_root = opts;

    return ov;
}


void
opts_visitor_cleanup(OptsVisitor *ov)
{
    if (ov->unprocessed_opts != NULL) {
        g_hash_table_destroy(ov->unprocessed_opts);
    }
    g_free(ov->fake_id_opt);
    g_free(ov);
}


Visitor *
opts_get_visitor(OptsVisitor *ov)
{
    return &ov->visitor;
}
