/*
  AST
  interface to front-end, obtains and translates syntax trees
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#ifdef BOEHM_GC
#include <gc.h>
#endif
#include "llt.h"
#include "julia.h"

#include "flisp.h"

static htable_t gensym_table;

static char flisp_system_image[] = {
#include "julia_flisp.boot.inc"
};

extern fltype_t *iostreamtype;

static int is_uws(uint32_t wc)
{
    return (wc==9 || wc==10 || wc==11 || wc==12 || wc==13 || wc==32 ||
            wc==133 || wc==160 || wc==5760 || wc==6158 || wc==8192 ||
            wc==8193 || wc==8194 || wc==8195 || wc==8196 || wc==8197 ||
            wc==8198 || wc==8199 || wc==8200 || wc==8201 || wc==8202 ||
            wc==8232 || wc==8233 || wc==8239 || wc==8287 || wc==12288);
}

value_t fl_skipws(value_t *args, u_int32_t nargs)
{
    argcount("skip-ws", nargs, 2);
    ios_t *s = fl_toiostream(args[0], "skip-ws");
    int newlines = (args[1]!=FL_F);
    uint32_t wc;
    if (ios_peekutf8(s, &wc) == IOS_EOF)
        return FL_EOF;
    while (!ios_eof(s) && is_uws(wc) && (newlines || wc!=10)) {
        ios_getutf8(s, &wc);
        ios_peekutf8(s, &wc);
    }
    return FL_T;
}

static int jl_id_char(uint32_t wc)
{
    return ((wc >= 'A' && wc <= 'Z') ||
            (wc >= 'a' && wc <= 'z') ||
            (wc >= '0' && wc <= '9') ||
            (wc >= 0xA1) ||
            wc == '_');
}

value_t fl_accum_julia_symbol(value_t *args, u_int32_t nargs)
{
    argcount("accum-julia-symbol", nargs, 2);
    ios_t *s = fl_toiostream(args[1], "accum-julia-symbol");
    if (!iscprim(args[0]) || ((cprim_t*)ptr(args[0]))->type != wchartype)
        type_error("accum-julia-symbol", "wchar", args[0]);
    uint32_t wc = *(uint32_t*)cp_data((cprim_t*)ptr(args[0]));
    ios_t str;
    ios_mem(&str, 0);
    while (jl_id_char(wc)) {
        ios_getutf8(s, &wc);
        ios_pututf8(&str, wc);
        if (ios_peekutf8(s, &wc) == IOS_EOF)
            break;
    }
    ios_pututf8(&str, 0);
    return symbol(str.buf);
}

static builtinspec_t julia_flisp_func_info[] = {
    { "skip-ws", fl_skipws },
    { "accum-julia-symbol", fl_accum_julia_symbol },
    { NULL, NULL }
};

void jl_lisp_prompt()
{
    fl_applyn(1, symbol_value(symbol("__start")), fl_cons(FL_NIL,FL_NIL));
}

void jl_init_frontend()
{
    fl_init(2*512*1024);
    assign_global_builtins(julia_flisp_func_info);
    value_t img = cvalue(iostreamtype, sizeof(ios_t));
    ios_t *pi = value2c(ios_t*, img);
    ios_static_buffer(pi, flisp_system_image, sizeof(flisp_system_image));
    
    if (fl_load_system_image(img)) {
        ios_printf(ios_stderr, "fatal error loading system image");
        exit(1);
    }

    fl_applyn(0, symbol_value(symbol("__init_globals")));

    htable_new(&gensym_table, 0);
}

void jl_shutdown_frontend()
{
    //fl_applyn(0, symbol_value(symbol("show-profiles")));
}

static jl_sym_t *scmsym_to_julia(value_t s)
{
    assert(issymbol(s));
    if (fl_isgensym(s)) {
        uptrint_t n = ((gensym_t*)ptr(s))->id + 100;
        void **bp = ptrhash_bp(&gensym_table, (void*)n);
        if (*bp == HT_NOTFOUND) {
            *bp = jl_gensym();
        }
        return (jl_sym_t*)*bp;
    }
    return jl_symbol(symbol_name(s));
}

static char *scmsym_to_str(value_t s)
{
    assert(issymbol(s));
    return symbol_name(s);
}

static void syntax_error_check(value_t e)
{
    if (iscons(e)) {
        value_t hd = car_(e);
        if (issymbol(hd)) {
            char *s = scmsym_to_str(hd);
            if (!strcmp(s,"error")) {
                jl_errorf("\nsyntax error: %s",
                          (char*)cvalue_data(car_(cdr_(e))));
            }
        }
    }
}

static size_t scm_list_length(value_t x)
{
    return llength(x);
}

static jl_value_t *scm_to_julia(value_t e);

static jl_value_t *full_list(value_t e)
{
    jl_tuple_t *ar = jl_alloc_tuple(scm_list_length(e));
    size_t i=0;
    while (iscons(e)) {
        jl_tupleset(ar, i, scm_to_julia(car_(e)));
        e = cdr_(e);
        i++;
    }
    return (jl_value_t*)ar;
}

static jl_value_t *full_list_of_lists(value_t e)
{
    jl_tuple_t *ar = jl_alloc_tuple(scm_list_length(e));
    size_t i=0;
    while (iscons(e)) {
        jl_tupleset(ar, i, full_list(car_(e)));
        e = cdr_(e);
        i++;
    }
    return (jl_value_t*)ar;
}

static jl_value_t *scm_to_julia(value_t e)
{
    if (fl_isnumber(e)) {
        if (iscprim(e) && cp_numtype((cprim_t*)ptr(e))==T_DOUBLE) {
            return (jl_value_t*)jl_box_float64(*(double*)cp_data((cprim_t*)ptr(e)));
        }
        uint64_t n = toulong(e, "scm_to_julia");
        if (n > S64_MAX)
            return (jl_value_t*)jl_box_uint64(n);
        if (n > S32_MAX)
            return (jl_value_t*)jl_box_int64((int64_t)n);
        return (jl_value_t*)jl_box_int32((int32_t)n);
    }
    if (issymbol(e)) {
        jl_sym_t *sym = scmsym_to_julia(e);
        if (!strcmp(sym->name,"true"))
            return jl_true;
        else if (!strcmp(sym->name,"false"))
            return jl_false;
        return (jl_value_t*)sym;
    }
    if (fl_isstring(e)) {
        return (jl_value_t*)jl_cstr_to_array(cvalue_data(e));
    }
    if (e == FL_F) {
        return jl_false;
    }
    if (e == FL_T) {
        return jl_true;
    }
    if (e == FL_NIL) {
        return (jl_value_t*)jl_null;
    }
    if (iscons(e)) {
        value_t hd = car_(e);
        if (issymbol(hd)) {
            jl_sym_t *sym = scmsym_to_julia(hd);
            char *s = sym->name;
            /* tree node types:
               goto  goto-ifnot  label  return
               lambda  call  =  quote
               null  top  unbound  box-unbound  closure-ref
               body  file
               line
            */
            size_t n = scm_list_length(e)-1;
            size_t i;
            if (sym == lambda_sym) {
                jl_expr_t *ex = jl_exprn(lambda_sym, n);
                value_t largs = car_(cdr_(e));
                jl_tupleset(ex->args, 0, full_list(largs));
                e = cdr_(cdr_(e));
                for(i=1; i < n; i++) {
                    assert(iscons(e));
                    jl_tupleset(ex->args, i, scm_to_julia(car_(e)));
                    e = cdr_(e);
                }
                return (jl_value_t*)
                    jl_expr(quote_sym, 1,
                            jl_new_lambda_info((jl_value_t*)ex, jl_null));
            }
            if (!strcmp(s, "var-info")) {
                jl_expr_t *ex = jl_exprn(sym, n);
                e = cdr_(e);
                jl_tupleset(ex->args, 0, scm_to_julia(car_(e)));
                e = cdr_(e);
                jl_tupleset(ex->args, 1, full_list_of_lists(car_(e)));
                e = cdr_(e);
                jl_tupleset(ex->args, 2, full_list(car_(e)));
                e = cdr_(e);
                for(i=3; i < n; i++) {
                    assert(iscons(e));
                    jl_tupleset(ex->args, i, scm_to_julia(car_(e)));
                    e = cdr_(e);
                }
                return (jl_value_t*)ex;
            }
            jl_expr_t *ex = jl_exprn(sym, n);
            e = cdr_(e);
            for(i=0; i < n; i++) {
                assert(iscons(e));
                jl_tupleset(ex->args, i, scm_to_julia(car_(e)));
                e = cdr_(e);
            }
            return (jl_value_t*)ex;
        }
        else {
            jl_error("malformed tree");
        }
    }
    jl_error("malformed tree");
    
    return (jl_value_t*)jl_null;
}

jl_value_t *jl_parse_input_line(const char *str)
{
    value_t e = fl_applyn(1, symbol_value(symbol("jl-parse-string")),
                          cvalue_static_cstring(str));
    if (e == FL_T || e == FL_F || e == FL_EOF)
        return NULL;
    syntax_error_check(e);
    
    return scm_to_julia(e);
}

jl_value_t *jl_parse_file(const char *fname)
{
    value_t e = fl_applyn(1, symbol_value(symbol("jl-parse-file")),
                          cvalue_static_cstring(fname));
    syntax_error_check(e);
    if (!iscons(e))
        return (jl_value_t*)jl_null;
    return scm_to_julia(e);
}

// syntax tree accessors

// get array of formal argument expressions
jl_tuple_t *jl_lam_args(jl_expr_t *l)
{
    assert(l->head == lambda_sym);
    jl_value_t *ae = jl_exprarg(l,0);
    assert(jl_is_tuple(ae));
    return (jl_tuple_t*)ae;
}

// get array of local var symbols
jl_tuple_t *jl_lam_locals(jl_expr_t *l)
{
    jl_value_t *le = jl_exprarg(l, 1);
    assert(jl_is_expr(le));
    jl_expr_t *lle = (jl_expr_t*)jl_exprarg(le,0);
    assert(jl_is_expr(lle));
    assert(lle->head == locals_sym);
    return lle->args;
}

// get array of body forms
jl_tuple_t *jl_lam_body(jl_expr_t *l)
{
    jl_value_t *be = jl_exprarg(l, 2);
    assert(jl_is_expr(be));
    assert(((jl_expr_t*)be)->head == body_sym);
    return ((jl_expr_t*)be)->args;
}

jl_sym_t *jl_decl_var(jl_value_t *ex)
{
    if (jl_is_symbol(ex)) return (jl_sym_t*)ex;
    assert(jl_is_expr(ex));
    return (jl_sym_t*)jl_exprarg(ex, 0);
}

int jl_is_rest_arg(jl_value_t *ex)
{
    if (!jl_is_expr(ex)) return 0;
    if (((jl_expr_t*)ex)->head != colons_sym) return 0;
    jl_expr_t *atype = (jl_expr_t*)jl_exprarg(ex,1);
    if (!jl_is_expr(atype)) return 0;
    if (atype->head != call_sym ||
        atype->args->length != 3)
        return 0;
    if ((jl_sym_t*)jl_exprarg(atype,1) != dots_sym)
        return 0;
    return 1;
}
