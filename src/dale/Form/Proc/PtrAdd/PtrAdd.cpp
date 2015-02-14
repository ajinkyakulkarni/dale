#include "../../../Generator/Generator.h"
#include "../../../Node/Node.h"
#include "../../../ParseResult/ParseResult.h"
#include "../../../Function/Function.h"
#include "../../../Operation/Cast/Cast.h"
#include "../../../Operation/Sizeof/Sizeof.h"
#include "../../../Operation/Destruct/Destruct.h"
#include "../Inst/Inst.h"
#include "../../../llvm_Function.h"

namespace dale
{
bool
FormProcPtrAddParse(Units *units,
           Function *fn,
           llvm::BasicBlock *block,
           Node *node,
           bool get_address,
           bool prefixed_with_core,
           ParseResult *pr)
{
    Context *ctx = units->top()->ctx; 

    assert(node->list && "must receive a list!");

    if (!ctx->er->assertArgNums("p+", node, 2, 2)) {
        return false;
    }

    symlist *lst = node->list;

    ParseResult ptr;
    bool mres = FormProcInstParse(units, 
                         fn, block, (*lst)[1], get_address, false, NULL, &ptr
                     );
    if (!mres) {
        return false;
    }
    if (!ctx->er->assertIsPointerType("p+", (*lst)[1], ptr.type, "1")) {
        return false;
    }

    ParseResult val;
    mres = FormProcInstParse(units, 
                 fn, ptr.block, (*lst)[2], get_address, false, NULL, &val
          );
    if (!mres) {
        return false;
    }
    if (!ctx->er->assertIsPointerOrIntegerType("p+", (*lst)[2], val.type, "2")) {
        return false;
    }

    llvm::BasicBlock *mynextb;
    ParseResult temp2;

    if (val.type->isIntegerType()) {
        ParseResult size;
        mres = Operation::Sizeof(ctx, val.block, 
                                          ptr.type->points_to, &size);
        if (!mres) {
            return false;
        }
        mres = 
            Operation::Cast(ctx, size.block, size.value, size.type,
                     ctx->tr->type_intptr, (*lst)[2], 0, &temp2);
        if (!mres) {
            return false;
        }
        mynextb = temp2.block;
    } else {
        mynextb = val.block;
    }

    ParseResult cast1;
    ParseResult cast2;
    mres = Operation::Cast(ctx, mynextb, ptr.value, ptr.type,
                 ctx->tr->type_intptr, (*lst)[1], 0, &cast1);
    if (!mres) {
        return false;
    }
    mres = Operation::Cast(ctx, cast1.block, val.value, val.type,
                 ctx->tr->type_intptr, (*lst)[2], 0, &cast2);
    if (!mres) {
        return false;
    }

    llvm::Value *res;

    {
        llvm::IRBuilder<> builder(cast2.block);

        if (val.type->isIntegerType()) {
            res = llvm::cast<llvm::Value>(
                      builder.CreateAdd(cast1.value,
                                        builder.CreateMul(
                                            temp2.value, cast2.value))
                  );
        } else {
            res = llvm::cast<llvm::Value>(
                      builder.CreateAdd(cast1.value, cast2.value)
                  );
        }
    }

    ParseResult final_res;
    Operation::Cast(ctx, cast2.block, res, ctx->tr->type_intptr, 
                             ptr.type, node, 0, &final_res);

    ptr.block = final_res.block;
    ParseResult temp;
    Operation::Destruct(ctx, &ptr, &temp);
    val.block = temp.block;
    Operation::Destruct(ctx, &val, &temp);
    final_res.block = temp.block;
    pr->set(final_res.block, ptr.type, final_res.value);

    return true;
}
}
