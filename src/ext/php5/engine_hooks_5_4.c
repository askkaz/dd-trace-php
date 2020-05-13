// clang-format off
#include "engine_hooks.h"
// clang-format on

#include <Zend/zend.h>
#include <Zend/zend_closures.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_interfaces.h>
#include <php.h>

#include <ext/spl/spl_exceptions.h>

#include "compatibility.h"
#include "ddtrace.h"
#include "debug.h"
#include "dispatch.h"
#include "logging.h"

ZEND_EXTERN_MODULE_GLOBALS(ddtrace);

#define RETURN_VALUE_USED(opline) (!((opline)->result_type & EXT_TYPE_UNUSED))

#define CTOR_CALL_BIT 0x1
#define CTOR_USED_BIT 0x2
#define DECODE_CTOR(ce) ((zend_class_entry *)(((zend_uintptr_t)(ce)) & ~(CTOR_CALL_BIT | CTOR_USED_BIT)))

/* This is used for op_array.reserved caching for calls that do not trace;
 * currently unused in PHP 5.4
 */
int ddtrace_resource = -1;

// True gloals; only modify in minit/mshutdown
static user_opcode_handler_t _prev_fcall_handler;
static user_opcode_handler_t _prev_fcall_by_name_handler;

static zval *ddtrace_this(zend_execute_data *execute_data) {
    zval *This = NULL;
    if (EX(opline)->opcode != ZEND_DO_FCALL && EX(object)) {
        This = EX(object);
    }
    if (This && Z_TYPE_P(This) != IS_OBJECT) {
        This = NULL;
    }

    return This;
}

static zend_function *_get_current_fbc(zend_execute_data *execute_data TSRMLS_DC) {
    if (EX(opline)->opcode == ZEND_DO_FCALL_BY_NAME) {
        return FBC();
    }
    zend_op *opline = EX(opline);
    zend_function *fbc = NULL;
    zval *fname = opline->op1.zv;

    if (CACHED_PTR(opline->op1.literal->cache_slot)) {
        return CACHED_PTR(opline->op1.literal->cache_slot);
    } else if (EXPECTED(zend_hash_quick_find(EG(function_table), Z_STRVAL_P(fname), Z_STRLEN_P(fname) + 1,
                                             Z_HASH_P(fname), (void **)&fbc) == SUCCESS)) {
        return fbc;
    } else {
        return NULL;
    }
}

static void **vm_stack_push_args_with_copy(int count TSRMLS_DC) {
    zend_vm_stack p = EG(argument_stack);

    zend_vm_stack_extend(count + 1 TSRMLS_CC);

    EG(argument_stack)->top += count;
    *(EG(argument_stack)->top) = (void *)(zend_uintptr_t)count;
    while (count-- > 0) {
        void *data = *(--p->top);

        if (UNEXPECTED(p->top == ZEND_VM_STACK_ELEMETS(p))) {
            zend_vm_stack r = p;

            EG(argument_stack)->prev = p->prev;
            p = p->prev;
            efree(r);
        }
        *(ZEND_VM_STACK_ELEMETS(EG(argument_stack)) + count) = data;
    }
    return EG(argument_stack)->top++;
}

static void **vm_stack_push_args(int count TSRMLS_DC) {
    if (UNEXPECTED(EG(argument_stack)->top - ZEND_VM_STACK_ELEMETS(EG(argument_stack)) < count) ||
        UNEXPECTED(EG(argument_stack)->top == EG(argument_stack)->end)) {
        return vm_stack_push_args_with_copy(count TSRMLS_CC);
    }
    *(EG(argument_stack)->top) = (void *)(zend_uintptr_t)count;
    return EG(argument_stack)->top++;
}

static void setup_fcal_name(zend_execute_data *execute_data, zend_fcall_info *fci, zval **result TSRMLS_DC) {
    int argc = EX(opline)->extended_value + NUM_ADDITIONAL_ARGS();
    fci->param_count = argc;

    if (NUM_ADDITIONAL_ARGS()) {
        vm_stack_push_args(fci->param_count TSRMLS_CC);
    } else {
        if (fci->param_count) {
            EX(function_state).arguments = zend_vm_stack_top(TSRMLS_C);
        }
        zend_vm_stack_push((void *)(zend_uintptr_t)fci->param_count TSRMLS_CC);
    }

    if (fci->param_count) {
        fci->params = (zval ***)safe_emalloc(sizeof(zval *), fci->param_count, 0);
        zend_get_parameters_array_ex(fci->param_count, fci->params);
    }
#if PHP_VERSION_ID < 50500
    if (EG(return_value_ptr_ptr)) {
        fci->retval_ptr_ptr = EG(return_value_ptr_ptr);
    } else {
        fci->retval_ptr_ptr = result;
    }
#else
    fci->retval_ptr_ptr = result;
#endif
}

static void ddtrace_setup_fcall(zend_execute_data *execute_data, zend_fcall_info *fci, zval **result TSRMLS_DC) {
    if (EX(opline)->opcode != ZEND_DO_FCALL_BY_NAME) {
#if PHP_VERSION_ID >= 50600
        call_slot *call = EX(call_slots) + EX(opline)->op2.num;
        call->fbc = EX(function_state).function;
        call->object = NULL;
        call->called_scope = NULL;
        call->num_additional_args = 0;
        call->is_ctor_call = 0;
        EX(call) = call;
#else
        FBC() = EX(function_state).function;
#endif
    }

#if PHP_VERSION_ID < 50500
    EX(original_return_value) = EG(return_value_ptr_ptr);
    EG(return_value_ptr_ptr) = result;
#endif

    setup_fcal_name(execute_data, fci, result TSRMLS_CC);
}

// todo: use op_array.reserved slot to cache negative lookups (ones that do not trace)
static BOOL_T _dd_should_trace_call(zend_execute_data *execute_data, zend_function **fbc,
                                    ddtrace_dispatch_t **dispatch TSRMLS_DC) {
    if (DDTRACE_G(disable) || DDTRACE_G(disable_in_current_request) || DDTRACE_G(class_lookup) == NULL ||
        DDTRACE_G(function_lookup) == NULL) {
        return FALSE;
    }
    *fbc = _get_current_fbc(execute_data TSRMLS_CC);
    if (!*fbc) {
        return FALSE;
    }

    zval zv, *fname;
    fname = &zv;
    if (EX(opline)->opcode == ZEND_DO_FCALL_BY_NAME) {
        ZVAL_STRING(fname, (*fbc)->common.function_name, 0);
    } else if (EX(opline)->op1.zv) {
        fname = EX(opline)->op1.zv;
    } else {
        return FALSE;
    }

    // Don't trace closures
    if ((*fbc)->common.fn_flags & ZEND_ACC_CLOSURE) {
        return FALSE;
    }

    zval *this = execute_data->object && Z_TYPE_P(execute_data->object) == IS_OBJECT ? execute_data->object : NULL;
    *dispatch = ddtrace_find_dispatch(this ? Z_OBJCE_P(this) : (*fbc)->common.scope, fname TSRMLS_CC);
    if (!*dispatch || (*dispatch)->busy) {
        return FALSE;
    }
    if (ddtrace_tracer_is_limited(TSRMLS_C) && ((*dispatch)->options & DDTRACE_DISPATCH_INSTRUMENT_WHEN_LIMITED) == 0) {
        return FALSE;
    }

    return TRUE;
}

int ddtrace_forward_call(zend_execute_data *execute_data, zend_function *fbc, zval *return_value TSRMLS_DC) {
    int fcall_status;

    zend_fcall_info fci = {0};
    zend_fcall_info_cache fcc = {0};
    zval *retval_ptr = NULL;

    fcc.initialized = 1;
    fcc.function_handler = fbc;
    fcc.object_ptr = ddtrace_this(execute_data);
    fcc.calling_scope = fbc->common.scope;  // EG(scope);
#if PHP_VERSION_ID < 50500
    fcc.called_scope = EX(called_scope);
#else
    fcc.called_scope = EX(call) ? EX(call)->called_scope : NULL;
#endif

    ddtrace_setup_fcall(execute_data, &fci, &retval_ptr TSRMLS_CC);
    fci.size = sizeof(fci);
    fci.no_separation = 1;
    fci.object_ptr = fcc.object_ptr;

    fcall_status = zend_call_function(&fci, &fcc TSRMLS_CC);
    if (fcall_status == SUCCESS && fci.retval_ptr_ptr && *fci.retval_ptr_ptr) {
        COPY_PZVAL_TO_ZVAL(*return_value, *fci.retval_ptr_ptr);
    }

    zend_fcall_info_args_clear(&fci, 1);
    return fcall_status;
}

static void _dd_update_opcode_leave(zend_execute_data *execute_data TSRMLS_DC) {
    DD_PRINTF("Update opcode leave");
#if PHP_VERSION_ID < 50500
    EX(function_state).function = (zend_function *)EX(op_array);
    EX(function_state).arguments = NULL;
    EG(opline_ptr) = &EX(opline);
    EG(active_op_array) = EX(op_array);

    EG(return_value_ptr_ptr) = EX(original_return_value);
    EX(original_return_value) = NULL;

    EG(active_symbol_table) = EX(symbol_table);

    EX(object) = EX(current_object);
    EX(called_scope) = DECODE_CTOR(EX(called_scope));

    zend_arg_types_stack_3_pop(&EG(arg_types_stack), &EX(called_scope), &EX(current_object), &EX(fbc));
    zend_vm_stack_clear_multiple(TSRMLS_C);
#else
    zend_vm_stack_clear_multiple(0 TSRMLS_CC);
    EX(call)--;
#endif
}

static zend_function *datadog_current_function(zend_execute_data *execute_data) {
    if (EX(opline)->opcode == ZEND_DO_FCALL_BY_NAME) {
        return FBC();
    } else {
        return EX(function_state).function;
    }
}

static void execute_fcall(ddtrace_dispatch_t *dispatch, zval *this, zend_execute_data *execute_data,
                          zval **return_value_ptr TSRMLS_DC) {
    zend_fcall_info fci = {0};
    zend_fcall_info_cache fcc = {0};
    char *error = NULL;
    zval closure;
    INIT_ZVAL(closure);
    zend_function *current_fbc = DDTRACE_G(original_context).fbc;
    zend_class_entry *executed_method_class = NULL;
    if (this) {
        executed_method_class = Z_OBJCE_P(this);
    }

    zend_function *func;

    const char *func_name = DDTRACE_CALLBACK_NAME;
    func = datadog_current_function(execute_data);

    zend_function *callable = (zend_function *)zend_get_closure_method_def(&dispatch->callable TSRMLS_CC);

    // convert passed callable to not be static as we're going to bind it to *this
    if (this) {
        callable->common.fn_flags &= ~ZEND_ACC_STATIC;
    }

    zend_create_closure(&closure, callable, executed_method_class, this TSRMLS_CC);
    if (zend_fcall_info_init(&closure, 0, &fci, &fcc, NULL, &error TSRMLS_CC) != SUCCESS) {
        if (DDTRACE_G(strict_mode)) {
            const char *scope_name, *function_name;

            scope_name = (func->common.scope) ? func->common.scope->name : NULL;
            function_name = func->common.function_name;
            if (scope_name) {
                zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0 TSRMLS_CC,
                                        "cannot set override for %s::%s - %s", scope_name, function_name, error);
            } else {
                zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0 TSRMLS_CC, "cannot set override for %s - %s",
                                        function_name, error);
            }
        }

        if (error) {
            efree(error);
        }
        goto _exit_cleanup;
    }

    ddtrace_setup_fcall(execute_data, &fci, return_value_ptr TSRMLS_CC);

    // Move this to closure zval before zend_fcall_info_init()
    fcc.function_handler->common.function_name = func_name;

    zend_execute_data *prev_original_execute_data = DDTRACE_G(original_context).execute_data;
    DDTRACE_G(original_context).execute_data = execute_data;

    zval *prev_original_function_name = DDTRACE_G(original_context).function_name;
    DDTRACE_G(original_context).function_name = (*EG(opline_ptr))->op1.zv;

    zend_call_function(&fci, &fcc TSRMLS_CC);

    DDTRACE_G(original_context).function_name = prev_original_function_name;

    DDTRACE_G(original_context).execute_data = prev_original_execute_data;

    if (fci.params) {
        efree(fci.params);
    }

_exit_cleanup:

    if (this) {
        Z_DELREF_P(this);
    }
    Z_DELREF(closure);
    zval_dtor(&closure);
    DDTRACE_G(original_context).fbc = current_fbc;
}

static void wrap_and_run(zend_execute_data *execute_data, ddtrace_dispatch_t *dispatch TSRMLS_DC) {
    zval *this = ddtrace_this(execute_data);

#if PHP_VERSION_ID < 50500
    zval *original_object = EX(object);
    if (EX(opline)->opcode == ZEND_DO_FCALL) {
        zend_op *opline = EX(opline);
        zend_ptr_stack_3_push(&EG(arg_types_stack), FBC(), EX(object), EX(called_scope));

        if (CACHED_PTR(opline->op1.literal->cache_slot)) {
            EX(function_state).function = CACHED_PTR(opline->op1.literal->cache_slot);
        } else {
            EX(function_state).function = DDTRACE_G(original_context).fbc;
            CACHE_PTR(opline->op1.literal->cache_slot, EX(function_state).function);
        }

        EX(object) = NULL;
    }
    if (this) {
        EX(object) = original_object;
    }
#endif
    const zend_op *opline = EX(opline);

#if PHP_VERSION_ID < 50500
#define EX_T(offset) (*(temp_variable *)((char *)EX(Ts) + offset))
    zval rv;
    INIT_ZVAL(rv);

    zval **return_value = NULL;
    zval *rv_ptr = &rv;

    if (RETURN_VALUE_USED(opline)) {
        EX_T(opline->result.var).var.ptr = &EG(uninitialized_zval);
        EX_T(opline->result.var).var.ptr_ptr = NULL;

        return_value = NULL;
    } else {
        return_value = &rv_ptr;
    }

    if (RETURN_VALUE_USED(opline)) {
        temp_variable *ret = &EX_T(opline->result.var);

        if (EG(return_value_ptr_ptr) && *EG(return_value_ptr_ptr)) {
            ret->var.ptr = *EG(return_value_ptr_ptr);
            ret->var.ptr_ptr = EG(return_value_ptr_ptr);
        } else {
            ret->var.ptr = NULL;
            ret->var.ptr_ptr = &ret->var.ptr;
        }

        ret->var.fcall_returned_reference =
            (DDTRACE_G(original_context).fbc->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0;
        return_value = ret->var.ptr_ptr;
    }

    execute_fcall(dispatch, this, execute_data, return_value TSRMLS_CC);
    EG(return_value_ptr_ptr) = EX(original_return_value);

    if (!RETURN_VALUE_USED(opline) && return_value && *return_value) {
        zval_delref_p(*return_value);
        if (Z_REFCOUNT_PP(return_value) == 0) {
            efree(*return_value);
            *return_value = NULL;
        }
    }

#else
    zval *return_value = NULL;
    execute_fcall(dispatch, this, execute_data, &return_value TSRMLS_CC);

    if (return_value != NULL) {
        if (RETURN_VALUE_USED(opline)) {
            EX_TMP_VAR(execute_data, opline->result.var)->var.ptr = return_value;
        } else {
            zval_ptr_dtor(&return_value);
        }
    }
#endif
}

static int _dd_opcode_default_dispatch(zend_execute_data *execute_data TSRMLS_DC) {
    if (!EX(opline)->opcode) {
        return ZEND_USER_OPCODE_DISPATCH;
    }
    switch (EX(opline)->opcode) {
        case ZEND_DO_FCALL:
            if (_prev_fcall_handler) {
                return _prev_fcall_handler(execute_data TSRMLS_CC);
            }
            break;

        case ZEND_DO_FCALL_BY_NAME:
            if (_prev_fcall_by_name_handler) {
                return _prev_fcall_by_name_handler(execute_data TSRMLS_CC);
            }
            break;
    }
    return ZEND_USER_OPCODE_DISPATCH;
}

static int _dd_begin_fcall_handler(zend_execute_data *execute_data TSRMLS_DC) {
    zend_function *current_fbc = NULL;
    ddtrace_dispatch_t *dispatch = NULL;
    if (!_dd_should_trace_call(execute_data, &current_fbc, &dispatch TSRMLS_CC)) {
        return _dd_opcode_default_dispatch(execute_data TSRMLS_CC);
    }
    int vm_retval = _dd_opcode_default_dispatch(execute_data TSRMLS_CC);
    if (vm_retval != ZEND_USER_OPCODE_DISPATCH) {
        if (get_dd_trace_debug()) {
            const char *fname = current_fbc->common.function_name ?: Z_STRVAL_P(EX(opline)->op1.zv);
            ddtrace_log_errf("A neighboring extension has altered the VM state for '%s()'; cannot reliably instrument",
                             fname ?: "{unknown}");
        }
        return vm_retval;
    }
    ddtrace_dispatch_copy(dispatch);  // protecting against dispatch being freed during php code execution
    dispatch->busy = 1;               // guard against recursion, catching only topmost execution

    if (dispatch->options & DDTRACE_DISPATCH_INNERHOOK) {
        // Store original context for forwarding the call from userland
        zend_function *previous_fbc = DDTRACE_G(original_context).fbc;
        DDTRACE_G(original_context).fbc = current_fbc;
        zend_function *previous_calling_fbc = DDTRACE_G(original_context).calling_fbc;
        DDTRACE_G(original_context).calling_fbc =
            execute_data->function_state.function && execute_data->function_state.function->common.scope
                ? execute_data->function_state.function
                : current_fbc;
        zval *this = ddtrace_this(execute_data);
        zval *previous_this = DDTRACE_G(original_context).this;
        DDTRACE_G(original_context).this = this;
        zend_class_entry *previous_calling_ce = DDTRACE_G(original_context).calling_ce;
        DDTRACE_G(original_context).calling_ce = DDTRACE_G(original_context).calling_fbc->common.scope;

        wrap_and_run(execute_data, dispatch TSRMLS_CC);

        // Restore original context
        DDTRACE_G(original_context).calling_ce = previous_calling_ce;
        DDTRACE_G(original_context).this = previous_this;
        DDTRACE_G(original_context).calling_fbc = previous_calling_fbc;
        DDTRACE_G(original_context).fbc = previous_fbc;

        _dd_update_opcode_leave(execute_data TSRMLS_CC);

        dispatch->busy = 0;
        ddtrace_dispatch_release(dispatch);
    }

    EX(opline)++;

    return ZEND_USER_OPCODE_LEAVE;
}
void ddtrace_opcode_minit(void) {
    _prev_fcall_handler = zend_get_user_opcode_handler(ZEND_DO_FCALL);
    _prev_fcall_by_name_handler = zend_get_user_opcode_handler(ZEND_DO_FCALL_BY_NAME);
    zend_set_user_opcode_handler(ZEND_DO_FCALL, _dd_begin_fcall_handler);
    zend_set_user_opcode_handler(ZEND_DO_FCALL_BY_NAME, _dd_begin_fcall_handler);
}

void ddtrace_opcode_mshutdown(void) {
    zend_set_user_opcode_handler(ZEND_DO_FCALL, NULL);
    zend_set_user_opcode_handler(ZEND_DO_FCALL_BY_NAME, NULL);
}

void ddtrace_execute_internal_minit(void) {
    // TODO
}

void ddtrace_execute_internal_mshutdown(void) {
    // TODO
}
