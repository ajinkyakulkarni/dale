#include "../../../Generator/Generator.h"
#include "../../../Node/Node.h"
#include "../../../ParseResult/ParseResult.h"
#include "../../../Element/Function/Function.h"
#include "llvm/Function.h"

namespace dale
{
namespace Form
{
namespace Proc
{
namespace GetDNodes
{
std::map<std::string, llvm::GlobalVariable*> string_cache;

llvm::Value *
IntNodeToStaticDNode(Generator *gen,
                     Node *node,
                     llvm::Value *next_node)
{
    if (!node) {
        fprintf(stderr, "Internal error: null node passed to "
                "IntNodeToStaticNode.\n");
        abort();
    }

    /* If it's one node, add the dnode. */
    std::string varname;
    gen->getUnusedVarname(&varname);

    /* Add the variable to the module. */

    llvm::Type *llvm_type = gen->llvm_type_dnode;
    llvm::Type *llvm_r_type = gen->llvm_type_pdnode;

    llvm::GlobalVariable *var =
        llvm::cast<llvm::GlobalVariable>(
            gen->mod->getOrInsertGlobal(varname.c_str(), llvm_type)
        );

    Context *ctx = gen->ctx;
    var->setLinkage(ctx->toLLVMLinkage(Linkage::Intern));

    std::vector<llvm::Constant *> constants;
    llvm::Constant *first =
        llvm::cast<llvm::Constant>(ctx->nt->getNativeInt(node->is_list));
    constants.push_back(first);

    if (!node->is_list) {
        Token *t = node->token;
        size_t pos = 0;
        while ((pos = t->str_value.find("\\n", pos)) != std::string::npos) {
            t->str_value.replace(pos, 2, "\n");
        }
        if (t->type == TokenType::StringLiteral) {
            t->str_value.insert(0, "\"");
            t->str_value.push_back('"');
        }

        /* If there is an entry in the cache for this string, and
         * the global variable in the cache belongs to the current
         * module, then use that global variable. */

        llvm::GlobalVariable *svar2 = NULL;

        std::map<std::string, llvm::GlobalVariable*>::iterator f
        = string_cache.find(t->str_value);
        if (f != string_cache.end()) {
            llvm::GlobalVariable *temp = f->second;
            if (temp->getParent() == gen->mod) {
                svar2 = temp;
            }
        }

        if (!svar2) {
            llvm::Constant *arr =
                llvm::ConstantArray::get(llvm::getGlobalContext(),
                                         t->str_value.c_str(),
                                         true);
            std::string varname2;
            gen->getUnusedVarname(&varname2);

            Element::Type *archar =
                ctx->tr->getArrayType(ctx->tr->type_char,
                                 t->str_value.size() + 1);

            svar2 =
                llvm::cast<llvm::GlobalVariable>(
                    gen->mod->getOrInsertGlobal(varname2.c_str(),
                                           ctx->toLLVMType(archar, NULL, false))
                );

            svar2->setInitializer(arr);
            svar2->setConstant(true);
            svar2->setLinkage(ctx->toLLVMLinkage(Linkage::Intern));

            string_cache.insert(std::pair<std::string,
                                llvm::GlobalVariable*>(
                                    t->str_value,
                                    svar2
                                ));
        }

        llvm::Value *temps[2];
        temps[0] = ctx->nt->getNativeInt(0);
        temps[1] = ctx->nt->getNativeInt(0);

        llvm::Constant *pce =
            llvm::ConstantExpr::getGetElementPtr(
                llvm::cast<llvm::Constant>(svar2),
                temps,
                2
            );

        constants.push_back(pce);
    } else {
        constants.push_back(
            llvm::cast<llvm::Constant>(
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(
                        ctx->toLLVMType(ctx->tr->type_pchar, NULL, false)
                    )
                )
            )
        );
    }

    if (node->is_list) {
        std::vector<Node *> *list = node->list;
        std::vector<Node *>::reverse_iterator list_iter = list->rbegin();
        llvm::Value *sub_next_node = NULL;

        while (list_iter != list->rend()) {
            llvm::Value *temp_value =
                IntNodeToStaticDNode(gen, (*list_iter), sub_next_node);
            sub_next_node = temp_value;
            ++list_iter;
        }

        constants.push_back(
            llvm::cast<llvm::Constant>(
                sub_next_node
            )
        );
    } else {
        constants.push_back(
            llvm::cast<llvm::Constant>(
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(
                        llvm_r_type
                    )
                )
            )
        );
    }

    if (next_node) {
        constants.push_back(
            llvm::cast<llvm::Constant>(
                next_node
            )
        );
    } else {
        constants.push_back(
            llvm::cast<llvm::Constant>(
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(
                        llvm_r_type
                    )
                )
            )
        );
    }

    int pos[8] = { node->getBeginPos()->line_number,
                   node->getBeginPos()->column_number,
                   node->getEndPos()->line_number,
                   node->getEndPos()->column_number,
                   node->macro_begin.line_number,
                   node->macro_begin.column_number,
                   node->macro_end.line_number,
                   node->macro_end.column_number };
    for (int i = 0; i < 8; i++) {
        constants.push_back(
            llvm::cast<llvm::Constant>(ctx->nt->getNativeInt(pos[i]))
        );
    }

    constants.push_back(
        llvm::cast<llvm::Constant>(
            llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(
                    ctx->toLLVMType(ctx->tr->type_pchar, NULL, false)
                )
            )
        )
    );
    llvm::StructType *st =
        llvm::cast<llvm::StructType>(llvm_type);
    llvm::Constant *init =
        llvm::ConstantStruct::get(
            st,
            constants
        );
    var->setInitializer(init);

    var->setConstant(true);

    return llvm::cast<llvm::Value>(var);
}

bool parse(Generator *gen,
           Element::Function *fn,
           llvm::BasicBlock *block,
           Node *node,
           bool get_address,
           bool prefixed_with_core,
           ParseResult *pr)
{
    Context *ctx = gen->ctx;

    if (!ctx->er->assertArgNums("get-dnodes", node, 1, 1)) {
        return false;
    }

    llvm::Value *v = IntNodeToStaticDNode(gen, node->list->at(1), NULL);

    llvm::IRBuilder<> builder(block);

    pr->set(block, gen->type_pdnode, v);
    return true;
}
}
}
}
}