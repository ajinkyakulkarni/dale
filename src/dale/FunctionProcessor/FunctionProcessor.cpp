#include "FunctionProcessor.h"

#include "../SavePoint/SavePoint.h"
#include "../Form/Proc/Inst/Inst.h"
#include "../Operation/Coerce/Coerce.h"
#include "../Operation/Cast/Cast.h"
#include "../Operation/Destruct/Destruct.h"
#include "../Operation/Copy/Copy.h"

#define IMPLICIT 1

using namespace dale::ErrorInst::Generator;

namespace dale
{
FunctionProcessor::FunctionProcessor(Units *units)
{
    this->units = units;
}

FunctionProcessor::~FunctionProcessor()
{
}

void
FunctionProcessor::processRetval(Type *return_type, llvm::BasicBlock *block,
                                 ParseResult *pr,
                                 std::vector<llvm::Value*> *call_args)
{
    if (!return_type->is_retval) {
        return;
    }

    pr->do_not_destruct = 1;
    pr->do_not_copy_with_setf = 1;
    pr->retval_used = true;

    if (pr->retval) {
        call_args->push_back(pr->retval);
        return;
    }

    llvm::IRBuilder<> builder(block);
    llvm::Type *et =
        units->top()->ctx->toLLVMType(return_type, NULL, false, false);
    if (!et) {
        return;
    }
    llvm::Value *retval_ptr =
        llvm::cast<llvm::Value>(builder.CreateAlloca(et));
    call_args->push_back(retval_ptr);
    pr->retval = retval_ptr;
    pr->retval_type = units->top()->ctx->tr->getPointerType(return_type);
}

bool
checkArgumentCount(Type *fn_ptr, Node *n, int num_args, ErrorReporter *er)
{
    int num_required_args = fn_ptr->numberOfRequiredArgs();

    if (fn_ptr->isVarArgs()) {
        if (num_args < num_required_args) {
            Error *e = new Error(
                IncorrectMinimumNumberOfArgs,
                n, "function pointer call", num_required_args, num_args
            );
            er->addError(e);
            return false;
        }
    } else {
        if (num_args != num_required_args) {
            Error *e = new Error(
                IncorrectNumberOfArgs,
                n, "function pointer call", num_required_args, num_args
            );
            er->addError(e);
            return false;
        }
    }

    return true;
}

bool
processReferenceTypes(std::vector<llvm::Value *> *call_args,
                      std::vector<llvm::Value *> *call_args_final,
                      std::vector<Node *> *call_arg_nodes,
                      std::vector<ParseResult> *call_arg_prs,
                      Function *dfn, std::vector<Type*> *parameter_types,
                      Context *ctx, bool args_cast,
                      int extra_call_args_size)
{
    int caps = call_arg_prs->size();
    int pts  = parameter_types->size();
    int limit = (caps > pts ? pts : caps);
    ParseResult refpr;
    for (int i = extra_call_args_size; i < limit; i++) {
        Type *pt = parameter_types->at(i);
        ParseResult *arg_refpr = &(call_arg_prs->at(i));
        if (pt->is_reference) {
            if (!pt->is_const && !arg_refpr->value_is_lvalue) {
                Error *e = new Error(
                    CannotTakeAddressOfNonLvalue,
                    call_arg_nodes->at(i)
                );
                ctx->er->addError(e);
                return false;
            }
            bool res = arg_refpr->getAddressOfValue(ctx, &refpr);
            if (!res) {
                return false;
            }
            call_args_final->at(i) = refpr.getValue(ctx);
        } else if (!args_cast) {
            bool res = Operation::Copy(ctx, dfn, arg_refpr, arg_refpr);
            if (!res) {
                return false;
            }
            call_args_final->at(i) = arg_refpr->getValue(ctx);
        }
    }

    return true;
}

bool
FunctionProcessor::parseFuncallInternal(Function *dfn, Node *n,
                                        bool get_address,
                                        ParseResult *fn_ptr_pr,
                                        int skip,
                                        std::vector<llvm::Value*> *extra_call_args,
                                        ParseResult *pr)
{
    Type *fn_ptr = fn_ptr_pr->type->points_to;

    llvm::BasicBlock *block = fn_ptr_pr->block;
    std::vector<llvm::Value *> empty;
    if (!extra_call_args) {
        extra_call_args = &empty;
    }

    int num_args = n->list->size() - skip + extra_call_args->size();
    bool res = checkArgumentCount(fn_ptr, n, num_args,
                                  units->top()->ctx->er);
    if (!res) {
        return false;
    }

    std::vector<llvm::Value *> call_args;
    std::vector<ParseResult> call_arg_prs;
    std::vector<Node *> call_arg_nodes;

    bool args_coerced = false;
    int arg_count = 1;

    std::copy(extra_call_args->begin(), extra_call_args->end(),
              std::back_inserter(call_args));

    std::vector<Type *>::iterator param_iter =
        fn_ptr->parameter_types.begin() + extra_call_args->size();

    for (std::vector<Node *>::iterator b = n->list->begin() + skip,
                                       e = n->list->end();
            b != e;
            ++b) {
        ParseResult arg_pr;
        bool res = FormProcInstParse(units, dfn, block, (*b), get_address,
                                     false, NULL, &arg_pr, true);
        if (!res) {
            return false;
        }

        call_arg_prs.push_back(arg_pr);
        call_arg_nodes.push_back(*b);
        block = arg_pr.block;

        /* If the parsed argument is not of the correct type, attempt
         * to coerce the type to the correct type. */
        if ((param_iter != fn_ptr->parameter_types.end())
                && (!(arg_pr.type->isEqualTo((*param_iter), 1)))
                && ((*param_iter)->base_type != BaseType::VarArgs)) {
            ParseResult coerce_pr;
            bool coerce_result =
                Operation::Coerce(units->top()->ctx, block,
                                  arg_pr.getValue(units->top()->ctx),
                                  arg_pr.type,
                                  (*param_iter),
                                  &coerce_pr);
            if (!coerce_result) {
                std::string wanted_type;
                std::string got_type;
                (*param_iter)->toString(&wanted_type);
                arg_pr.type->toString(&got_type);

                Error *e = new Error(IncorrectArgType, (*b),
                                     "function pointer call",
                                     wanted_type.c_str(), arg_count,
                                     got_type.c_str());
                units->top()->ctx->er->addError(e);
                return false;
            } else {
                args_coerced = true;
                call_args.push_back(coerce_pr.value);
            }
        } else {
            call_args.push_back(arg_pr.getValue(units->top()->ctx));
        }

        if (param_iter != fn_ptr->parameter_types.end()) {
            ++param_iter;
            /* Skip the varargs type. */
            if (param_iter != fn_ptr->parameter_types.end()) {
                if ((*param_iter)->base_type == BaseType::VarArgs) {
                    ++param_iter;
                }
            }
        }
    }

    llvm::IRBuilder<> builder(block);

    std::vector<llvm::Value *> call_args_final = call_args;
    res = processReferenceTypes(&call_args, &call_args_final,
                                &call_arg_nodes, &call_arg_prs, dfn,
                                &(fn_ptr->parameter_types),
                                units->top()->ctx, args_coerced,
                                extra_call_args->size());
    if (!res) {
        return false;
    }

    processRetval(fn_ptr->return_type,
                  block, pr, &call_args_final);

    llvm::Value *call_res =
        builder.CreateCall(fn_ptr_pr->value,
                           llvm::ArrayRef<llvm::Value*>(call_args_final));

    pr->set(block, fn_ptr->return_type, call_res);

    fn_ptr_pr->block = pr->block;
    ParseResult temp;
    res = Operation::Destruct(units->top()->ctx, fn_ptr_pr, &temp);
    if (!res) {
        return false;
    }
    pr->block = temp.block;

    return true;
}

bool
isUnoverloadedMacro(Units *units, const char *name,
                    std::vector<Node*> *lst,
                    Function **macro_to_call)
{
    std::map<std::string, std::vector<Function *> *>::iterator
        iter;
    Function *fn = NULL;
    for (std::vector<NSNode *>::reverse_iterator
            rb = units->top()->ctx->used_ns_nodes.rbegin(),
            re = units->top()->ctx->used_ns_nodes.rend();
            rb != re;
            ++rb) {
        iter = (*rb)->ns->functions.find(name);
        if (iter != (*rb)->ns->functions.end()) {
            fn = iter->second->at(0);
            break;
        }
    }
    if (fn && fn->is_macro) {
        /* If the third argument is either non-existent, or a (p
         * DNode) (because typed arguments must appear before the
         * first (p DNode) argument), then short-circuit, so long
         * as the argument count is ok. */
        std::vector<Variable*>::iterator
            b = (fn->parameter_types.begin() + 1);
        if ((b == fn->parameter_types.end())
                || (*b)->type->isEqualTo(units->top()->ctx->tr->type_pdnode)) {
            bool use = false;
            int size = lst->size();
            if (fn->isVarArgs()) {
                use = ((fn->numberOfRequiredArgs() - 1)
                        <= (size - 1));
            } else {
                use = ((fn->numberOfRequiredArgs() - 1)
                        == (size - 1));
            }
            if (use) {
                *macro_to_call = fn;
                return true;
            }
        }
    }

    return false;
}

bool
processExternCFunction(Context *ctx,
                       const char *name, Node *n, Function **fn_ptr,
                       llvm::BasicBlock *block,
                       std::vector<llvm::Value *> *call_args,
                       std::vector<Type *> *call_arg_types,
                       bool *args_cast)
{
    ErrorReporter *er = ctx->er;

    /* Get this single function, try to cast each integral
     * call_arg to the expected type. If that succeeds
     * without error, then keep going. */

    std::vector<llvm::Value *> call_args_newer;
    std::vector<Type *> call_arg_types_newer;

    Function *fn = ctx->getFunction(name, NULL, NULL, 0);

    std::vector<Variable *> myarg_types =
        fn->parameter_types;
    std::vector<Variable *>::iterator miter =
        myarg_types.begin();

    std::vector<llvm::Value *>::iterator citer =
        call_args->begin();
    std::vector<Type *>::iterator caiter =
        call_arg_types->begin();

    /* Create strings describing the types, for use in a
        * possible error message. */

    std::string expected_args;
    std::string provided_args;
    while (miter != myarg_types.end()) {
        (*miter)->type->toString(&expected_args);
        expected_args.append(" ");
        ++miter;
    }
    if (expected_args.size() == 0) {
        expected_args.append("void");
    } else {
        expected_args.erase(expected_args.size() - 1, 1);
    }
    while (caiter != call_arg_types->end()) {
        (*caiter)->toString(&provided_args);
        provided_args.append(" ");
        ++caiter;
    }
    if (provided_args.size() == 0) {
        provided_args.append("void");
    } else {
        provided_args.erase(provided_args.size() - 1, 1);
    }
    miter = myarg_types.begin();
    caiter = call_arg_types->begin();
    int size = call_args->size();

    if (size < fn->numberOfRequiredArgs()) {
        Error *e = new Error(FunctionNotInScope, n,
                                name,
                                provided_args.c_str(),
                                expected_args.c_str());
        er->addError(e);
        return false;
    }
    if (!fn->isVarArgs()
            && size != fn->numberOfRequiredArgs()) {
        Error *e = new Error(FunctionNotInScope, n,
                                name,
                                provided_args.c_str(),
                                expected_args.c_str());
        er->addError(e);
        return false;
    }

    while (miter != myarg_types.end()
            && citer != call_args->end()
            && caiter != call_arg_types->end()) {
        if ((*caiter)->isEqualTo((*miter)->type, 1)) {
            call_args_newer.push_back((*citer));
            call_arg_types_newer.push_back((*caiter));
            ++miter;
            ++citer;
            ++caiter;
            continue;
        }
        if (!(*miter)->type->isIntegerType()
                and (*miter)->type->base_type != BaseType::Bool) {
            Error *e = new Error(FunctionNotInScope, n,
                                    name,
                                    provided_args.c_str(),
                                    expected_args.c_str());
            er->addError(e);
            return false;
        }
        if (!(*caiter)->isIntegerType()
                and (*caiter)->base_type != BaseType::Bool) {
            Error *e = new Error(FunctionNotInScope, n,
                                    name,
                                    provided_args.c_str(),
                                    expected_args.c_str());
            er->addError(e);
            return false;
        }

        ParseResult mytemp;
        bool res = Operation::Cast(ctx, block,
                    (*citer),
                    (*caiter),
                    (*miter)->type,
                    n,
                    IMPLICIT,
                    &mytemp);
        if (!res) {
            Error *e = new Error(FunctionNotInScope, n,
                                    name,
                                    provided_args.c_str(),
                                    expected_args.c_str());
            er->addError(e);
            return false;
        }
        block = mytemp.block;
        call_args_newer.push_back(mytemp.getValue(ctx));
        call_arg_types_newer.push_back(mytemp.type);

        ++miter;
        ++citer;
        ++caiter;
    }

    *call_args = call_args_newer;
    *call_arg_types = call_arg_types_newer;
    *args_cast = true;

    *fn_ptr = fn;

    return true;
}

bool
processVarArgsFunction(Context *ctx, Function *fn,
                       std::vector<llvm::Value *> *call_args,
                       std::vector<Type *> *call_arg_types,
                       llvm::IRBuilder<> *builder)
{
    int n = fn->numberOfRequiredArgs();

    std::vector<llvm::Value *>::iterator call_args_iter
    = call_args->begin();
    std::vector<Type *>::iterator call_arg_types_iter
    = call_arg_types->begin();

    while (n--) {
        ++call_args_iter;
        ++call_arg_types_iter;
    }
    while (call_args_iter != call_args->end()) {
        if ((*call_arg_types_iter)->base_type == BaseType::Float) {
            (*call_args_iter) =
                builder->CreateFPExt(
                    (*call_args_iter),
                    llvm::Type::getDoubleTy(llvm::getGlobalContext())
                );
            (*call_arg_types_iter) =
                ctx->tr->type_double;
        } else if ((*call_arg_types_iter)->isIntegerType()) {
            int real_size =
                ctx->nt->internalSizeToRealSize(
                    (*call_arg_types_iter)->getIntegerSize()
                );

            if (real_size < ctx->nt->getNativeIntSize()) {
                if ((*call_arg_types_iter)->isSignedIntegerType()) {
                    /* Target integer is signed - use sext. */
                    (*call_args_iter) =
                        builder->CreateSExt((*call_args_iter),
                                            ctx->toLLVMType(
                                                ctx->tr->type_int,
                                                            NULL, false));
                    (*call_arg_types_iter) = ctx->tr->type_int;
                } else {
                    /* Target integer is not signed - use zext. */
                    (*call_args_iter) =
                        builder->CreateZExt((*call_args_iter),
                                            ctx->toLLVMType(
                                                ctx->tr->type_uint,
                                                            NULL, false));
                    (*call_arg_types_iter) = ctx->tr->type_uint;
                }
            }
        }
        ++call_args_iter;
        ++call_arg_types_iter;
    }

    return true;
}

bool
addNotFoundError(std::vector<Type *> *call_arg_types,
                 const char *name,
                 Node *n,
                 Function *closest_fn,
                 bool has_others,
                 ErrorReporter *er)
{
    if (has_others) {
        std::vector<Type *>::iterator titer =
            call_arg_types->begin();

        std::string args;
        while (titer != call_arg_types->end()) {
            (*titer)->toString(&args);
            ++titer;
            if (titer != call_arg_types->end()) {
                args.append(" ");
            }
        }

        if (closest_fn) {
            std::string expected;
            std::vector<Variable *>::iterator viter;
            viter = closest_fn->parameter_types.begin();
            if (closest_fn->is_macro) {
                ++viter;
            }
            while (viter != closest_fn->parameter_types.end()) {
                (*viter)->type->toString(&expected);
                expected.append(" ");
                ++viter;
            }
            if (expected.size() > 0) {
                expected.erase(expected.size() - 1, 1);
            }
            Error *e = new Error(
                OverloadedFunctionOrMacroNotInScopeWithClosest,
                n,
                name, args.c_str(),
                expected.c_str()
            );
            er->addError(e);
            return false;
        } else {
            Error *e = new Error(
                OverloadedFunctionOrMacroNotInScope,
                n,
                name, args.c_str()
            );
            er->addError(e);
            return false;
        }
    } else {
        Error *e = new Error(NotInScope, n, name);
        er->addError(e);
        return false;
    }


}

bool
FunctionProcessor::parseFunctionCall(Function *dfn, llvm::BasicBlock *block,
                                     Node *n, const char *name,
                                     bool get_address, bool prefixed_with_core,
                                     Function **macro_to_call, ParseResult *pr)
{
    Context *ctx = units->top()->ctx;
    ErrorReporter *er = ctx->er;

    if (get_address) {
        Error *e = new Error(CannotTakeAddressOfNonLvalue, n);
        er->addError(e);
        return false;
    }

    symlist *lst = n->list;

    Node *nfn_name = (*lst)[0];

    if (!nfn_name->is_token) {
        Error *e = new Error(FirstListElementMustBeAtom, nfn_name);
        er->addError(e);
        return false;
    }

    Token *t = nfn_name->token;

    if (t->type != TokenType::String) {
        Error *e = new Error(FirstListElementMustBeSymbol, nfn_name);
        er->addError(e);
        return false;
    }

    /* Put all of the arguments into a list. */

    std::vector<Node *>::iterator symlist_iter;

    std::vector<llvm::Value *> call_args;
    std::vector<Node *> call_arg_nodes;
    std::vector<ParseResult> call_arg_prs;
    std::vector<Type *> call_arg_types;

    std::vector<llvm::Value *> call_args_newer;
    std::vector<Type *> call_arg_types_newer;

    symlist_iter = lst->begin();
    /* Skip the function name. */
    ++symlist_iter;

    /* The processing further down is only required when the
     * function/macro name is overloaded.  For now, short-circuit for
     * macros that are not overloaded, since those are most common,
     * and avoiding the later work in those cases makes things much
     * quicker. */

    if (!ctx->isOverloadedFunction(t->str_value.c_str())) {
        if (isUnoverloadedMacro(units, name, lst, macro_to_call)) {
            return false;
        }
    }

    std::vector<Error*> errors;

    /* Record the number of blocks and the instruction index in the
     * current block.  If the underlying Function to call is a
     * function, then there's no problem with using the modifications
     * caused by the repeated FormProcInstParse calls below.  If it's
     * a macro, however, anything that occurred needs to be 'rolled
     * back'.  Have to do the same thing for the context. */

    SavePoint sp(ctx, dfn, block);

    while (symlist_iter != lst->end()) {
        call_arg_nodes.push_back(*symlist_iter);
        int error_count = er->getErrorTypeCount(ErrorType::Error);

        ParseResult p;
        bool res =
            FormProcInstParse(units, dfn, block, (*symlist_iter),
                              false, false, NULL, &p, true);

        int diff = er->getErrorTypeCount(ErrorType::Error) - error_count;

        if (!res || diff) {
            /* May be a macro call (could be an unparseable
             * argument). Pop and store errors for the time being
             * and treat this argument as a (p DNode). */

            if (diff) {
                errors.insert(errors.end(),
                              er->errors.begin() + error_count,
                              er->errors.end());
                er->errors.erase(
                    er->errors.begin() + error_count,
                    er->errors.end()
                );
            }

            call_args.push_back(NULL);
            call_arg_types.push_back(ctx->tr->type_pdnode);
            ++symlist_iter;
            continue;
        }

        block = p.block;
        if (p.type->is_array) {
            p = ParseResult(block, p.type_of_address_of_value,
                            p.address_of_value);
        }
        call_args.push_back(p.getValue(ctx));
        call_arg_types.push_back(p.type);
        call_arg_prs.push_back(p);

        ++symlist_iter;
    }

    /* Now have all the argument types. Get the function out of
     * the context. */

    Function *closest_fn = NULL;

    Function *fn =
        ctx->getFunction(t->str_value.c_str(),
                         &call_arg_types,
                         &closest_fn,
                         0);

    /* If the function is a macro, set macro_to_call and return false.
     * (It's the caller's responsibility to handle processing of
     * macros.) */

    if (fn && fn->is_macro) {
        sp.restore();
        *macro_to_call = fn;
        return false;
    }

    /* If the function is not a macro, and errors were encountered
     * during argument processing, then this function has been
     * loaded in error (it will be a normal function taking a (p
     * DNode) argument, but the argument is not a real (p DNode)
     * value). Replace all the errors and return NULL. */

    if (errors.size() && fn && !fn->is_macro) {
        for (std::vector<Error*>::reverse_iterator b = errors.rbegin(),
                e = errors.rend();
                b != e;
                ++b) {
            er->addError(*b);
        }
        return false;
    }

    bool args_cast = false;

    if (!fn) {
        /* If no function was found, and there are errors related
         * to argument parsing, then push those errors back onto
         * the reporter and return. (May change this later to be a
         * bit more friendly - probably if there are any macros or
         * functions with the same name, this should show the
         * overload failure, rather than the parsing failure
         * errors). */
        if (errors.size()) {
            for (std::vector<Error*>::reverse_iterator b = errors.rbegin(),
                    e = errors.rend();
                    b != e;
                    ++b) {
                er->addError(*b);
            }
            return false;
        }

        if (ctx->existsExternCFunction(t->str_value.c_str())) {
            bool res = processExternCFunction(ctx, t->str_value.c_str(),
                                              n, &fn, block,
                                              &call_args,
                                              &call_arg_types,
                                              &args_cast);
            if (!res) {
                return false;
            }
        } else if (!t->str_value.compare("destroy")) {
            /* Return a no-op ParseResult if the function name is
             * 'destroy', because it's tedious to have to check in
             * generic code whether a particular value can be
             * destroyed or not. */
            pr->set(block, ctx->tr->type_void, NULL);
            return true;
        } else {
            bool has_others =
                ctx->existsNonExternCFunction(t->str_value.c_str());

            addNotFoundError(&call_arg_types,
                             t->str_value.c_str(), n,
                             closest_fn, has_others,
                             er);
            return false;
        }
    }

    llvm::IRBuilder<> builder(block);

    /* If this function is varargs, find the point at which the
     * varargs begin, and then promote any call_args floats to
     * doubles, and any integer types smaller than the native
     * integer size to native integer size. */

    if (fn->isVarArgs()) {
        args_cast = true;
        processVarArgsFunction(ctx, fn, &call_args, &call_arg_types,
                               &builder);
    }

    /* Iterate over the types of the found function. For the reference
     * types, replace the call argument with its address. */

    std::vector<Type *> parameter_types;
    for (std::vector<Variable *>::iterator b = fn->parameter_types.begin(),
                                           e = fn->parameter_types.end();
            b != e;
            ++b) {
        parameter_types.push_back((*b)->type);
    }
    std::vector<llvm::Value *> call_args_final = call_args;
    bool res = processReferenceTypes(&call_args, &call_args_final,
                                &call_arg_nodes, &call_arg_prs, dfn,
                                &parameter_types,
                                ctx, args_cast, 0);
    if (!res) {
        return false;
    }

    processRetval(fn->return_type, block, pr, &call_args_final);

    llvm::Value *call_res = builder.CreateCall(
                                fn->llvm_function,
                                llvm::ArrayRef<llvm::Value*>(call_args_final));

    pr->set(block, fn->return_type, call_res);

    /* If the return type of the function is one that should be
     * copied with an overridden setf, that will occur in the
     * function, so prevent the value from being re-copied here
     * (because no corresponding destructor call will occur). */

    pr->do_not_copy_with_setf = 1;

    return true;
}
}
