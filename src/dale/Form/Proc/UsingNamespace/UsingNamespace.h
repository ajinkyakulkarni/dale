#ifndef DALE_FORM_PROC_USINGNAMESPACE
#define DALE_FORM_PROC_USINGNAMESPACE

namespace dale
{
bool
FormProcUsingNamespaceParse(Units *units,
           Function *fn,
           llvm::BasicBlock *block,
           Node *node,
           bool get_address,
           bool prefixed_with_core,
           ParseResult *pr);
}

#endif
