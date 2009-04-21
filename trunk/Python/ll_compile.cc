#include "Python/ll_compile.h"

#include "Python.h"
#include "code.h"
#include "opcode.h"
#include "frameobject.h"

#include "Python/global_llvm_data.h"
#include "Util/TypeBuilder.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constant.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Module.h"
#include "llvm/Type.h"

#include <vector>

using llvm::BasicBlock;
using llvm::Constant;
using llvm::ConstantInt;
using llvm::Function;
using llvm::FunctionType;
using llvm::IntegerType;
using llvm::Module;
using llvm::PointerType;
using llvm::Type;
using llvm::Value;
using llvm::array_endof;

namespace py {

// These have the same meanings as the why_code enum in ceval.cc.
enum UnwindReason {
    UNWIND_NOUNWIND,
    UNWIND_EXCEPTION,
    UNWIND_RERAISE,
    UNWIND_RETURN,
    UNWIND_BREAK,
    UNWIND_CONTINUE,
    UNWIND_YIELD
};

static ConstantInt *
get_signed_constant_int(const Type *type, int64_t v)
{
    // This is an LLVM idiom. It expects an unsigned integer but does
    // different conversions internally depending on whether it was
    // originally signed or not.
    return ConstantInt::get(type, static_cast<uint64_t>(v), true /* signed */);
}

template<> class TypeBuilder<PyObject> {
public:
    static const Type *cache(Module *module) {
        std::string pyobject_name("__pyobject");
        const Type *result = module->getTypeByName(pyobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with object.h.
        llvm::PATypeHolder object_ty = llvm::OpaqueType::get();
        Type *p_object_ty = PointerType::getUnqual(object_ty);
        llvm::StructType *temp_object_ty = llvm::StructType::get(
            // Fields from PyObject_HEAD.
#ifdef Py_TRACE_REFS
            // _ob_next, _ob_prev
            p_object_ty, p_object_ty,
#endif
            TypeBuilder<ssize_t>::cache(module),
            p_object_ty,
            NULL);
	// Unifies the OpaqueType fields with the whole structure.  We
	// couldn't do that originally because the type's recursive.
        llvm::cast<llvm::OpaqueType>(object_ty.get())
            ->refineAbstractTypeTo(temp_object_ty);
        module->addTypeName(pyobject_name, object_ty.get());
        return object_ty.get();
    }

    enum Fields {
#ifdef Py_TRACE_REFS
        FIELD_NEXT,
        FIELD_PREV,
#endif
        FIELD_REFCNT,
        FIELD_TYPE,
    };
};
typedef TypeBuilder<PyObject> ObjectTy;

template<> class TypeBuilder<PyTupleObject> {
public:
    static const Type *cache(Module *module) {
        std::string pytupleobject_name("__pytupleobject");
        const Type *result = module->getTypeByName(pytupleobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with tupleobject.h.
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From PyTupleObject
            TypeBuilder<PyObject*[]>::cache(module),  // ob_item
            NULL);

        module->addTypeName(pytupleobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_SIZE,
        FIELD_ITEM,
    };
};
typedef TypeBuilder<PyTupleObject> TupleTy;

template<> class TypeBuilder<PyListObject> {
public:
    static const Type *cache(Module *module) {
        std::string pylistobject_name("__pylistobject");
        const Type *result = module->getTypeByName(pylistobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with listobject.h.
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From PyListObject
            TypeBuilder<PyObject**>::cache(module),  // ob_item
            TypeBuilder<Py_ssize_t>::cache(module),  // allocated
            NULL);

        module->addTypeName(pylistobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_SIZE,
        FIELD_ITEM,
        FIELD_ALLOCATED,
    };
};
typedef TypeBuilder<PyListObject> ListTy;

template<> class TypeBuilder<PyTypeObject> {
public:
    static const Type *cache(Module *module) {
        std::string pytypeobject_name("__pytypeobject");
        const Type *result = module->getTypeByName(pytypeobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with code.h.
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From PyTYPEObject
            TypeBuilder<const char *>::cache(module),  // tp_name
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_basicsize
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_itemsize
            TypeBuilder<destructor>::cache(module),  // tp_dealloc
            // tp_print
            TypeBuilder<int (*)(PyObject*, char*, int)>::cache(module),
            TypeBuilder<getattrfunc>::cache(module),  // tp_getattr
            TypeBuilder<setattrfunc>::cache(module),  // tp_setattr
            TypeBuilder<cmpfunc>::cache(module),  // tp_compare
            TypeBuilder<reprfunc>::cache(module),  // tp_repr
            TypeBuilder<char *>::cache(module),  // tp_as_number
            TypeBuilder<char *>::cache(module),  // tp_as_sequence
            TypeBuilder<char *>::cache(module),  // tp_as_mapping
            TypeBuilder<hashfunc>::cache(module),  // tp_hash
            TypeBuilder<ternaryfunc>::cache(module),  // tp_call
            TypeBuilder<reprfunc>::cache(module),  // tp_str
            TypeBuilder<getattrofunc>::cache(module),  // tp_getattro
            TypeBuilder<setattrofunc>::cache(module),  // tp_setattro
            TypeBuilder<char *>::cache(module),  // tp_as_buffer
            TypeBuilder<long>::cache(module),  // tp_flags
            TypeBuilder<const char *>::cache(module),  // tp_doc
            TypeBuilder<traverseproc>::cache(module),  // tp_traverse
            TypeBuilder<inquiry>::cache(module),  // tp_clear
            TypeBuilder<richcmpfunc>::cache(module),  // tp_richcompare
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_weaklistoffset
            TypeBuilder<getiterfunc>::cache(module),  // tp_iter
            TypeBuilder<iternextfunc>::cache(module),  // tp_iternext
            TypeBuilder<char *>::cache(module),  // tp_methods
            TypeBuilder<char *>::cache(module),  // tp_members
            TypeBuilder<char *>::cache(module),  // tp_getset
            TypeBuilder<PyObject *>::cache(module),  // tp_base
            TypeBuilder<PyObject *>::cache(module),  // tp_dict
            TypeBuilder<descrgetfunc>::cache(module),  // tp_descr_get
            TypeBuilder<descrsetfunc>::cache(module),  // tp_descr_set
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_dictoffset
            TypeBuilder<initproc>::cache(module),  // tp_init
            // Can't use newfunc or allocfunc because they refer to
            // PyTypeObject.
            TypeBuilder<PyObject *(*)(PyObject *,
                                      Py_ssize_t)>::cache(module),  // tp_alloc
            TypeBuilder<PyObject *(*)(PyObject *, PyObject *,
                                      PyObject *)>::cache(module),  // tp_new
            TypeBuilder<freefunc>::cache(module),  // tp_free
            TypeBuilder<inquiry>::cache(module),  // tp_is_gc
            TypeBuilder<PyObject *>::cache(module),  // tp_bases
            TypeBuilder<PyObject *>::cache(module),  // tp_mro
            TypeBuilder<PyObject *>::cache(module),  // tp_cache
            TypeBuilder<PyObject *>::cache(module),  // tp_subclasses
            TypeBuilder<PyObject *>::cache(module),  // tp_weaklist
            TypeBuilder<destructor>::cache(module),  // tp_del
            TypeBuilder<unsigned int>::cache(module),  // tp_version_tag
#ifdef COUNT_ALLOCS
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_allocs
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_frees
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_maxalloc
            TypeBuilder<PyObject *>::cache(module),  // tp_prev
            TypeBuilder<PyObject *>::cache(module),  // tp_next
#endif
            NULL);

        module->addTypeName(pytypeobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_SIZE,
        FIELD_NAME,
        FIELD_BASICSIZE,
        FIELD_ITEMSIZE,
        FIELD_DEALLOC,
        FIELD_PRINT,
        FIELD_GETATTR,
        FIELD_SETATTR,
        FIELD_COMPARE,
        FIELD_REPR,
        FIELD_AS_NUMBER,
        FIELD_AS_SEQUENCE,
        FIELD_AS_MAPPING,
        FIELD_HASH,
        FIELD_CALL,
        FIELD_STR,
        FIELD_GETATTRO,
        FIELD_SETATTRO,
        FIELD_AS_BUFFER,
        FIELD_FLAGS,
        FIELD_DOC,
        FIELD_TRAVERSE,
        FIELD_CLEAR,
        FIELD_RICHCOMPARE,
        FIELD_WEAKLISTOFFSET,
        FIELD_ITER,
        FIELD_ITERNEXT,
        FIELD_METHODS,
        FIELD_MEMBERS,
        FIELD_GETSET,
        FIELD_BASE,
        FIELD_DICT,
        FIELD_DESCR_GET,
        FIELD_DESCR_SET,
        FIELD_DICTOFFSET,
        FIELD_INIT,
        FIELD_ALLOC,
        FIELD_NEW,
        FIELD_FREE,
        FIELD_IS_GC,
        FIELD_BASES,
        FIELD_MRO,
        FIELD_CACHE,
        FIELD_SUBCLASSES,
        FIELD_WEAKLIST,
        FIELD_DEL,
        FIELD_TP_VERSION_TAG,
#ifdef COUNT_ALLOCS
        FIELD_ALLOCS,
        FIELD_FREES,
        FIELD_MAXALLOC,
        FIELD_PREV,
        FIELD_NEXT,
#endif
    };
};
typedef TypeBuilder<PyTypeObject> TypeTy;

template<> class TypeBuilder<PyCodeObject> {
public:
    static const Type *cache(Module *module) {
        std::string pycodeobject_name("__pycodeobject");
        const Type *result = module->getTypeByName(pycodeobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with code.h.
        const Type *p_pyobject_type = TypeBuilder<PyObject*>::cache(module);
        const Type *int_type = TypeBuilder<int>::cache(module);
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyCodeObject
            int_type,  // co_argcount
            int_type,  // co_nlocals
            int_type,  // co_stacksize
            int_type,  // co_flags
            p_pyobject_type,  // co_code
            p_pyobject_type,  // co_consts
            p_pyobject_type,  // co_names
            p_pyobject_type,  // co_varnames
            p_pyobject_type,  // co_freevars
            p_pyobject_type,  // co_cellvars
            //  Not bothering with defining the Inst struct.
            TypeBuilder<char*>::cache(module),  // co_tcode
            p_pyobject_type,  // co_filename
            p_pyobject_type,  // co_name
            int_type,  // co_firstlineno
            p_pyobject_type,  // co_lnotab
            TypeBuilder<char*>::cache(module),  //co_zombieframe
            p_pyobject_type,  // co_llvm_function
            NULL);

        module->addTypeName(pycodeobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_ARGCOUNT,
        FIELD_NLOCALS,
        FIELD_STACKSIZE,
        FIELD_FLAGS,
        FIELD_CODE,
        FIELD_CONSTS,
        FIELD_NAMES,
        FIELD_VARNAMES,
        FIELD_FREEVARS,
        FIELD_CELLVARS,
        FIELD_TCODE,
        FIELD_FILENAME,
        FIELD_NAME,
        FIELD_FIRSTLINENO,
        FIELD_LNOTAB,
        FIELD_ZOMBIEFRAME,
        FIELD_LLVM_FUNCTION,
    };
};
typedef TypeBuilder<PyCodeObject> CodeTy;

template<> class TypeBuilder<PyTryBlock> {
public:
    static const Type *cache(Module *module) {
        const Type *int_type = TypeBuilder<int>::cache(module);
        return llvm::StructType::get(
            // b_type, b_handler, b_level
            int_type, int_type, int_type, NULL);
    }
    enum Fields {
        FIELD_TYPE,
        FIELD_HANDLER,
        FIELD_LEVEL,
    };
};

template<> class TypeBuilder<PyFrameObject> {
public:
    static const Type *cache(Module *module) {
        std::string pyframeobject_name("__pyframeobject");
        const Type *result = module->getTypeByName(pyframeobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with frameobject.h.
        const Type *p_pyobject_type = TypeBuilder<PyObject*>::cache(module);
        const Type *int_type = TypeBuilder<int>::cache(module);
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            ObjectTy::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From struct _frame
            p_pyobject_type,  // f_back
            TypeBuilder<PyCodeObject*>::cache(module),  // f_code
            p_pyobject_type,  // f_builtins
            p_pyobject_type,  // f_globals
            p_pyobject_type,  // f_locals
            TypeBuilder<PyObject**>::cache(module),  // f_valuestack
            TypeBuilder<PyObject**>::cache(module),  // f_stacktop
            p_pyobject_type,  // f_trace
            p_pyobject_type,  // f_exc_type
            p_pyobject_type,  // f_exc_value
            p_pyobject_type,  // f_exc_traceback
            // f_tstate; punt on the type:
            TypeBuilder<char*>::cache(module),
            int_type,  // f_lasti
            int_type,  // f_lineno
            int_type,  // f_iblock
            // f_blockstack:
            TypeBuilder<PyTryBlock[CO_MAXBLOCKS]>::cache(module),
            // f_localsplus, flexible array.
            TypeBuilder<PyObject*[]>::cache(module),
            NULL);

        module->addTypeName(pyframeobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT_HEAD,
        FIELD_OB_SIZE,
        FIELD_BACK,
        FIELD_CODE,
        FIELD_BUILTINS,
        FIELD_GLOBALS,
        FIELD_LOCALS,
        FIELD_VALUESTACK,
        FIELD_STACKTOP,
        FIELD_TRACE,
        FIELD_EXC_TYPE,
        FIELD_EXC_VALUE,
        FIELD_EXC_TRACEBACK,
        FIELD_TSTATE,
        FIELD_LASTI,
        FIELD_LINENO,
        FIELD_IBLOCK,
        FIELD_BLOCKSTACK,
        FIELD_LOCALSPLUS,
    };
};
typedef TypeBuilder<PyFrameObject> FrameTy;

// This type collects the set of three values that constitute an
// exception.  So far, it's only used for
// _PyLlvm_WrapEnterExceptOrFinally().
struct ExcInfo {
    PyObject *exc;
    PyObject *val;
    PyObject *tb;
};

template<> class TypeBuilder<ExcInfo> {
public:
    static const Type *cache(Module *module) {
        const Type *pyobject_p = TypeBuilder<PyObject *>::cache(module);
        return llvm::StructType::get(pyobject_p, pyobject_p, pyobject_p, NULL);
    }
    enum Fields {
        FIELD_EXC,
        FIELD_VAL,
        FIELD_TB,
    };
};

static const FunctionType *
get_function_type(Module *module)
{
    std::string function_type_name("__function_type");
    const FunctionType *result =
        llvm::cast_or_null<FunctionType>(
            module->getTypeByName(function_type_name));
    if (result != NULL)
        return result;

    result = TypeBuilder<PyObject*(PyFrameObject*)>::cache(module);
    module->addTypeName(function_type_name, result);
    return result;
}

LlvmFunctionBuilder::LlvmFunctionBuilder(
    PyGlobalLlvmData *llvm_data, const std::string& name)
    : llvm_data_(llvm_data),
      module_(this->llvm_data_->module()),
      function_(Function::Create(
                    get_function_type(this->module_),
                    llvm::GlobalValue::ExternalLinkage,
                    // Prefix names with "#u#" to avoid collisions
                    // with runtime functions.
                    "#u#" + name,
                    this->module_))
{
    Function::arg_iterator args = function()->arg_begin();
    this->frame_ = args++;
    assert(args == function()->arg_end() &&
           "Unexpected number of arguments");
    this->frame_->setName("frame");

    builder().SetInsertPoint(BasicBlock::Create("entry", function()));
    // CreateAllocaInEntryBlock will insert alloca's here, before
    // any other instructions in the 'entry' block.

    this->unwind_block_ = BasicBlock::Create("unwind_block", function());

    this->stack_pointer_addr_ = builder().CreateAlloca(
        TypeBuilder<PyObject**>::cache(this->module_),
        NULL, "stack_pointer_addr");
    this->retval_addr_ = builder().CreateAlloca(
        TypeBuilder<PyObject*>::cache(this->module_),
        NULL, "retval_addr");
    this->unwind_reason_addr_ = builder().CreateAlloca(
        Type::Int8Ty, NULL, "unwind_reason_addr");
    this->unwind_target_index_addr_ = builder().CreateAlloca(
        Type::Int32Ty, NULL, "unwind_target_index_addr");

    this->stack_bottom_ = builder().CreateLoad(
        builder().CreateStructGEP(this->frame_, FrameTy::FIELD_VALUESTACK),
        "stack_bottom");
    Value *initial_stack_pointer =
        builder().CreateLoad(
            builder().CreateStructGEP(this->frame_, FrameTy::FIELD_STACKTOP),
            "initial_stack_pointer");
    builder().CreateStore(initial_stack_pointer, this->stack_pointer_addr_);

    Value *code = builder().CreateLoad(
        builder().CreateStructGEP(this->frame_, FrameTy::FIELD_CODE), "co");
    this->varnames_ = builder().CreateLoad(
        builder().CreateStructGEP(code, CodeTy::FIELD_VARNAMES),
        "varnames");
    Value *consts_tuple =  // (PyTupleObject*)code->co_consts
        builder().CreateBitCast(
            builder().CreateLoad(
                builder().CreateStructGEP(code, CodeTy::FIELD_CONSTS)),
            TypeBuilder<PyTupleObject*>::cache(this->module_));
    // Get the address of the const_tuple's first item, for convenient
    // indexing of all its items.
    this->consts_ = GetTupleItemSlot(consts_tuple, 0);
    Value *names_tuple = builder().CreateBitCast(
        builder().CreateLoad(
            builder().CreateStructGEP(code, CodeTy::FIELD_NAMES)),
        TypeBuilder<PyTupleObject*>::cache(this->module_),
        "names");
    // Get the address of the names_tuple's first item as well.
    this->names_ = GetTupleItemSlot(names_tuple, 0);

    // The next GEP-magic assigns this->fastlocals_ to
    // &frame_[0].f_localsplus[0].
    Value* fastlocals_indices[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, FrameTy::FIELD_LOCALSPLUS),
        ConstantInt::get(Type::Int32Ty, 0),
    };
    this->fastlocals_ =
        builder().CreateGEP(this->frame_,
                            fastlocals_indices, array_endof(fastlocals_indices),
                            "fastlocals");
    Value *nlocals = builder().CreateLoad(
        builder().CreateStructGEP(code, CodeTy::FIELD_NLOCALS), "nlocals");

    this->freevars_ =
        builder().CreateGEP(this->fastlocals_, nlocals, "freevars");
    this->globals_ =
        builder().CreateBitCast(
            builder().CreateLoad(
                builder().CreateStructGEP(this->frame_,
                                          FrameTy::FIELD_GLOBALS)),
            TypeBuilder<PyObject *>::cache(this->module_));
    this->builtins_ =
        builder().CreateBitCast(
            builder().CreateLoad(
                builder().CreateStructGEP(this->frame_,
                                          FrameTy::FIELD_BUILTINS)),
            TypeBuilder<PyObject *>::cache(this->module_));

    FillUnwindBlock();
}

void
LlvmFunctionBuilder::FillUnwindBlock()
{
    BasicBlock *const orig_block = builder().GetInsertBlock();

    BasicBlock *unreachable_block =
        BasicBlock::Create("unreachable", function());
    builder().SetInsertPoint(unreachable_block);
    Function *abort_func = GetGlobalFunction<void()>("abort");
    builder().CreateCall(abort_func);
    builder().CreateUnreachable();

    // Handles, roughly, the ceval.cc JUMPTO macro.
    BasicBlock *goto_unwind_target_block =
        BasicBlock::Create("goto_unwind_target", function());
    builder().SetInsertPoint(goto_unwind_target_block);
    Value *unwind_target_index =
        builder().CreateLoad(this->unwind_target_index_addr_,
                             "unwind_target_index");
    // Each call to AddUnwindTarget() will add a new case to this
    // switch.  ceval.cc just assigns the new IP, allowing wild jumps,
    // but LLVM won't let us do that so we default to jumping to the
    // unreachable block.
    this->unwind_target_switch_ =
        builder().CreateSwitch(unwind_target_index, unreachable_block);

    // Code that needs to unwind the stack will jump here.
    // (e.g. returns, exceptions, breaks, and continues).
    builder().SetInsertPoint(this->unwind_block_);
    Value *unwind_reason =
        builder().CreateLoad(this->unwind_reason_addr_, "unwind_reason");

    BasicBlock *do_return =
        BasicBlock::Create("do_return", function());
    {  // Implements the fast_block_end loop toward the end of
       // PyEval_EvalFrameEx().  This pops blocks off the block-stack
       // and values off the value-stack until it finds a block that
       // wants to handle the current unwind reason.
        BasicBlock *unwind_loop_header =
            BasicBlock::Create("unwind_loop_header", function());
        BasicBlock *unwind_loop_body =
            BasicBlock::Create("unwind_loop_body", function());

        FallThroughTo(unwind_loop_header);
        // Continue looping if frame->f_iblock > 0.
        Value *blocks_left = builder().CreateLoad(
            builder().CreateStructGEP(this->frame_, FrameTy::FIELD_IBLOCK));
        builder().CreateCondBr(IsPositive(blocks_left),
                               unwind_loop_body, do_return);

        builder().SetInsertPoint(unwind_loop_body);
        Value *popped_block = builder().CreateLoad(
            builder().CreateCall(
                GetGlobalFunction<PyTryBlock *(PyFrameObject *)>(
                    "PyFrame_BlockPop"),
                this->frame_));
        Value *block_type =
            builder().CreateExtractValue(popped_block,
                                         TypeBuilder<PyTryBlock>::FIELD_TYPE,
                                         "block_type");

        // TODO(jyasskin): Handle SETUP_LOOP with UNWIND_CONTINUE.

        // Pop values back to where this block started.
        Value *pop_to_level =
            builder().CreateExtractValue(popped_block,
                                         TypeBuilder<PyTryBlock>::FIELD_LEVEL,
                                         "block_level");
        PopAndDecrefTo(builder().CreateGEP(this->stack_bottom_, pop_to_level));

        BasicBlock *handle_loop =
            BasicBlock::Create("handle_loop", function());
        BasicBlock *handle_except =
            BasicBlock::Create("handle_except", function());
        BasicBlock *handle_finally =
            BasicBlock::Create("handle_finally", function());
        BasicBlock *push_exception =
            BasicBlock::Create("push_exception", function());
        BasicBlock *goto_block_handler =
            BasicBlock::Create("goto_block_handler", function());

        llvm::SwitchInst *block_type_switch =
            builder().CreateSwitch(block_type, unreachable_block, 3);
        block_type_switch->addCase(
            ConstantInt::get(block_type->getType(), ::SETUP_LOOP),
            handle_loop);
        block_type_switch->addCase(
            ConstantInt::get(block_type->getType(), ::SETUP_EXCEPT),
            handle_except);
        block_type_switch->addCase(
            ConstantInt::get(block_type->getType(), ::SETUP_FINALLY),
            handle_finally);

        builder().SetInsertPoint(handle_loop);
        Value *unwinding_break = builder().CreateICmpEQ(
            unwind_reason, ConstantInt::get(Type::Int8Ty, UNWIND_BREAK),
            "currently_unwinding_break");
        builder().CreateCondBr(unwinding_break,
                               goto_block_handler, unwind_loop_header);

        builder().SetInsertPoint(handle_except);
        // We only consider visiting except blocks when an exception
        // is being unwound.
        Value *unwinding_exception = builder().CreateICmpEQ(
            unwind_reason, ConstantInt::get(Type::Int8Ty, UNWIND_EXCEPTION),
            "currently_unwinding_exception");
        builder().CreateCondBr(unwinding_exception,
                               push_exception, unwind_loop_header);

        builder().SetInsertPoint(push_exception);
        // We need an alloca here so _PyLlvm_WrapEnterExceptOrFinally
        // can return into it.  This alloca _won't_ be optimized by
        // mem2reg because its address is taken.
        Value *exc_info = CreateAllocaInEntryBlock(
            TypeBuilder<ExcInfo>::cache(this->module_), NULL, "exc_info");
        builder().CreateCall2(
            GetGlobalFunction<void(ExcInfo*, int)>(
                "_PyLlvm_WrapEnterExceptOrFinally"),
            exc_info,
            block_type);
        Push(builder().CreateLoad(
                 builder().CreateStructGEP(
                     exc_info, TypeBuilder<ExcInfo>::FIELD_TB)));
        Push(builder().CreateLoad(
                 builder().CreateStructGEP(
                     exc_info, TypeBuilder<ExcInfo>::FIELD_VAL)));
        Push(builder().CreateLoad(
                 builder().CreateStructGEP(
                     exc_info, TypeBuilder<ExcInfo>::FIELD_EXC)));
        builder().CreateBr(goto_block_handler);

        builder().SetInsertPoint(handle_finally);
        // Jump to the finally block, with the stack prepared for
        // END_FINALLY to continue unwinding.

        BasicBlock *push_retval = BasicBlock::Create("push_retval", function());
        BasicBlock *handle_finally_end =
            BasicBlock::Create("handle_finally_end", function());
        llvm::SwitchInst *should_push_retval = builder().CreateSwitch(
            unwind_reason, handle_finally_end, 2);
        // When unwinding for an exception, we have to save the
        // exception onto the stack.
        should_push_retval->addCase(
            ConstantInt::get(Type::Int8Ty, UNWIND_EXCEPTION), push_exception);
        // When unwinding for a return or continue, we have to save
        // the return value or continue target onto the stack.
        should_push_retval->addCase(
            ConstantInt::get(Type::Int8Ty, UNWIND_RETURN), push_retval);
        should_push_retval->addCase(
            ConstantInt::get(Type::Int8Ty, UNWIND_CONTINUE), push_retval);

        builder().SetInsertPoint(push_retval);
        Push(builder().CreateLoad(this->retval_addr_, "retval"));

        FallThroughTo(handle_finally_end);
        // END_FINALLY expects to find the unwind reason on the top of
        // the stack.  There's probably a way to do this that doesn't
        // involve allocating an int for every unwind through a
        // finally block, but imitating CPython is simpler.
        Value *unwind_reason_as_pyint = builder().CreateCall(
            GetGlobalFunction<PyObject *(long)>("PyInt_FromLong"),
            builder().CreateZExt(unwind_reason,
                                 TypeBuilder<long>::cache(this->module_)),
            "unwind_reason_as_pyint");
        Push(unwind_reason_as_pyint);

        FallThroughTo(goto_block_handler);
        // Clear the unwind reason while running through the block's
        // handler.  mem2reg should never actually decide to use this
        // value, but having it here should make such forgotten stores
        // more obvious.
        builder().CreateStore(ConstantInt::get(Type::Int8Ty, UNWIND_NOUNWIND),
                              this->unwind_reason_addr_);
        // The block's handler field holds the index of the block
        // defining this finally or except, or the tail of the loop we
        // just broke out of.  Jump to it through the unwind switch
        // statement defined above.
        Value *block_handler = builder().CreateExtractValue(
            popped_block, TypeBuilder<PyTryBlock>::FIELD_HANDLER,
            "block_handler");
        builder().CreateStore(block_handler,
                              this->unwind_target_index_addr_);
        builder().CreateBr(goto_unwind_target_block);
    }  // End unwind loop.

    // If we fall off the end of the unwind loop, there are no blocks
    // left and it's time to pop the rest of the value stack and
    // return.
    builder().SetInsertPoint(do_return);
    PopAndDecrefTo(this->stack_bottom_);
    Value *retval = builder().CreateLoad(this->retval_addr_, "retval");
    builder().CreateRet(retval);

    builder().SetInsertPoint(orig_block);
}

void
LlvmFunctionBuilder::PopAndDecrefTo(Value *target_stack_pointer)
{
    BasicBlock *pop_loop = BasicBlock::Create("pop_loop", function());
    BasicBlock *pop_block = BasicBlock::Create("pop_stack", function());
    BasicBlock *pop_done = BasicBlock::Create("pop_done", function());

    FallThroughTo(pop_loop);
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    Value *finished_popping = builder().CreateICmpULE(
        stack_pointer, target_stack_pointer);
    builder().CreateCondBr(finished_popping, pop_done, pop_block);

    builder().SetInsertPoint(pop_block);
    XDecRef(Pop());
    builder().CreateBr(pop_loop);

    builder().SetInsertPoint(pop_done);
}

Value *
LlvmFunctionBuilder::CreateAllocaInEntryBlock(
    const Type *alloca_type, Value *array_size, const char *name="")
{
    // In order for LLVM to optimize alloca's, we should emit alloca
    // instructions in the function entry block. We can get at the block
    // with function()->begin(), but it will already have a 'br' instruction
    // at the end. Instantiating the AllocaInst class directly, we pass it
    // the begin() iterator of the entry block, causing it to insert itself
    // right before the first instruction in the block.
    return new llvm::AllocaInst(alloca_type, array_size, name,
                                function()->begin()->begin());
}

void
LlvmFunctionBuilder::FallThroughTo(BasicBlock *next_block)
{
    if (builder().GetInsertBlock()->getTerminator() == NULL) {
        // If the block doesn't already end with a branch or
        // return, branch to the next block.
        builder().CreateBr(next_block);
    }
    builder().SetInsertPoint(next_block);
}

ConstantInt *
LlvmFunctionBuilder::AddUnwindTarget(llvm::BasicBlock *target)
{
    // The size of the switch instruction will give us a small unique
    // number for each target block.
    ConstantInt *target_index =
        ConstantInt::get(Type::Int32Ty,
                         this->unwind_target_switch_->getNumCases());
    this->unwind_target_switch_->addCase(target_index, target);
    return target_index;
}

void
LlvmFunctionBuilder::Return(Value *retval)
{
    builder().CreateStore(retval, this->retval_addr_);
    builder().CreateStore(ConstantInt::get(Type::Int8Ty, UNWIND_RETURN),
                          this->unwind_reason_addr_);
    builder().CreateBr(this->unwind_block_);
}

void
LlvmFunctionBuilder::PropagateException()
{
    builder().CreateStore(
        Constant::getNullValue(TypeBuilder<PyObject*>::cache(this->module_)),
        this->retval_addr_);
    builder().CreateStore(ConstantInt::get(Type::Int8Ty, UNWIND_EXCEPTION),
                          this->unwind_reason_addr_);
    // TODO(jyasskin): Arrange to call PyTraceBack_Here and, if
    // tracing is turned on, the appropriate trace function.
    builder().CreateBr(this->unwind_block_);
}

void
LlvmFunctionBuilder::LOAD_CONST(int index)
{
    Value *const_ = builder().CreateLoad(
        builder().CreateGEP(this->consts_,
                            ConstantInt::get(Type::Int32Ty, index)));
    IncRef(const_);
    Push(const_);
}

void
LlvmFunctionBuilder::LOAD_GLOBAL(int name_index)
{
    BasicBlock *global_missing = BasicBlock::Create(
        "LOAD_GLOBAL_global_missing", function());
    BasicBlock *global_success = BasicBlock::Create(
        "LOAD_GLOBAL_global_success", function());
    BasicBlock *builtin_missing = BasicBlock::Create(
        "LOAD_GLOBAL_builtin_missing", function());
    BasicBlock *builtin_success = BasicBlock::Create(
        "LOAD_GLOBAL_builtin_success", function());
    BasicBlock *done = BasicBlock::Create("LOAD_GLOBAL_done", function());
    Value *name = LookupName(name_index);
    Function *pydict_getitem = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyDict_GetItem");
    Value *global = builder().CreateCall2(
        pydict_getitem, this->globals_, name, "global_variable");
    builder().CreateCondBr(IsNull(global), global_missing, global_success);

    builder().SetInsertPoint(global_success);
    IncRef(global);
    Push(global);
    builder().CreateBr(done);

    builder().SetInsertPoint(global_missing);
    // This ignores any exception set by PyDict_GetItem (and similarly
    // for the builtins dict below,) but this is what ceval does too.
    Value *builtin = builder().CreateCall2(
        pydict_getitem, this->builtins_, name, "builtin_variable");
    builder().CreateCondBr(IsNull(builtin), builtin_missing, builtin_success);

    builder().SetInsertPoint(builtin_missing);
    Function *do_raise = GetGlobalFunction<
        void(PyObject *)>("_PyEval_RaiseForGlobalNameError");
    builder().CreateCall(do_raise, name);
    PropagateException();

    builder().SetInsertPoint(builtin_success);
    IncRef(builtin);
    Push(builtin);
    builder().CreateBr(done);

    builder().SetInsertPoint(done);
}

void
LlvmFunctionBuilder::STORE_GLOBAL(int name_index)
{
    Value *name = LookupName(name_index);
    Value *value = Pop();
    Function *pydict_setitem = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyDict_SetItem");
    Value *result = builder().CreateCall3(
        pydict_setitem, this->globals_, name, value,
        "STORE_GLOBAL_result");
    DecRef(value);
    PropagateExceptionOnNonZero(result);
}

void
LlvmFunctionBuilder::DELETE_GLOBAL(int name_index)
{
    BasicBlock *failure = BasicBlock::Create("DELETE_GLOBAL_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("DELETE_GLOBAL_success",
                                             function());
    Value *name = LookupName(name_index);
    Function *pydict_setitem = GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyDict_DelItem");
    Value *result = builder().CreateCall2(
        pydict_setitem, this->globals_, name, "STORE_GLOBAL_result");
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Function *do_raise = GetGlobalFunction<
        void(PyObject *)>("_PyEval_RaiseForGlobalNameError");
    builder().CreateCall(do_raise, name);
    PropagateException();

    builder().SetInsertPoint(success);
}

void
LlvmFunctionBuilder::LOAD_ATTR(int index)
{
    Value *attr = LookupName(index);
    Value *obj = Pop();
    Function *pyobj_getattr = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyObject_GetAttr");
    Value *result = builder().CreateCall2(
        pyobj_getattr, obj, attr, "LOAD_ATTR_result");
    DecRef(obj);
    PropagateExceptionOnNull(result);
    Push(result);
}

void
LlvmFunctionBuilder::STORE_ATTR(int index)
{
    Value *attr = LookupName(index);
    Value *obj = Pop();
    Value *value = Pop();
    Function *pyobj_setattr = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyObject_SetAttr");
    Value *result = builder().CreateCall3(
        pyobj_setattr, obj, attr, value, "STORE_ATTR_result");
    DecRef(obj);
    DecRef(value);
    PropagateExceptionOnNonZero(result);
}

void
LlvmFunctionBuilder::DELETE_ATTR(int index)
{
    Value *attr = LookupName(index);
    Value *obj = Pop();
    Value *value =
        Constant::getNullValue(TypeBuilder<PyObject*>::cache(this->module_));
    Function *pyobj_setattr = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyObject_SetAttr");
    Value *result = builder().CreateCall3(
        pyobj_setattr, obj, attr, value, "STORE_ATTR_result");
    DecRef(obj);
    PropagateExceptionOnNonZero(result);
}

void
LlvmFunctionBuilder::LOAD_FAST(int index)
{
    BasicBlock *unbound_local =
        BasicBlock::Create("LOAD_FAST_unbound", function());
    BasicBlock *success =
        BasicBlock::Create("LOAD_FAST_success", function());

    Value *local = builder().CreateLoad(
        builder().CreateGEP(this->fastlocals_,
                            ConstantInt::get(Type::Int32Ty, index)),
        "FAST_loaded");
    builder().CreateCondBr(IsNull(local), unbound_local, success);

    builder().SetInsertPoint(unbound_local);
    Function *do_raise =
        GetGlobalFunction<void(PyFrameObject*, int)>(
            "_PyEval_RaiseForUnboundLocal");
    builder().CreateCall2(
        do_raise, this->frame_,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_),
                         index, true /* signed */));
    PropagateException();

    builder().SetInsertPoint(success);
    IncRef(local);
    Push(local);
}

void
LlvmFunctionBuilder::CALL_FUNCTION(int num_args)
{
#ifdef WITH_TSC
// XXX(twouters): figure out how to support WITH_TSC in LLVM.
#error WITH_TSC builds are unsupported at this time.
#endif
    Function *call_function = GetGlobalFunction<
        PyObject *(PyObject ***, int)>("_PyEval_CallFunction");

    // ceval.cc passes a copy of the stack pointer to _PyEval_CallFunction,
    // so we do the same thing.
    // XXX(twouters): find out if this is really necessary; we just end up
    // using the stack pointer anyway, even in the error case.
    Value *temp_stack_pointer_addr = CreateAllocaInEntryBlock(
        TypeBuilder<PyObject**>::cache(this->module_),
        0, "CALL_FUNCTION_stack_pointer_addr");
    builder().CreateStore(
        builder().CreateLoad(this->stack_pointer_addr_),
        temp_stack_pointer_addr);
    Value *result = builder().CreateCall2(
        call_function,
        temp_stack_pointer_addr,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), num_args),
        "CALL_FUNCTION_result");
    builder().CreateStore(
        builder().CreateLoad(temp_stack_pointer_addr),
        this->stack_pointer_addr_);
    PropagateExceptionOnNull(result);
    Push(result);
}

void
LlvmFunctionBuilder::CALL_FUNCTION_VAR_KW(int num_args)
{
#ifdef WITH_TSC
// XXX(twouters): figure out how to support WITH_TSC in LLVM.
#error WITH_TSC builds are unsupported at this time.
#endif
    Function *call_function = GetGlobalFunction<
        PyObject *(PyObject ***, int)>("_PyEval_CallFunctionVarKw");
    Value *result = builder().CreateCall2(
        call_function,
        this->stack_pointer_addr_,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), num_args),
        "CALL_FUNCTION_VAR_KW_result");
    PropagateExceptionOnNonZero(result);
    // _PyEval_CallFunctionVarKw() already pushed the result onto our stack.
}

void
LlvmFunctionBuilder::JUMP_ABSOLUTE(llvm::BasicBlock *target,
                                   llvm::BasicBlock *fallthrough)
{
    builder().CreateBr(target);
}

void
LlvmFunctionBuilder::POP_JUMP_IF_FALSE(llvm::BasicBlock *target,
                                       llvm::BasicBlock *fallthrough)
{
    Value *test_value = Pop();
    Value *is_true = IsPythonTrue(test_value);
    builder().CreateCondBr(is_true, fallthrough, target);
}

void
LlvmFunctionBuilder::POP_JUMP_IF_TRUE(llvm::BasicBlock *target,
                                      llvm::BasicBlock *fallthrough)
{
    Value *test_value = Pop();
    Value *is_true = IsPythonTrue(test_value);
    builder().CreateCondBr(is_true, target, fallthrough);
}

void
LlvmFunctionBuilder::JUMP_IF_FALSE_OR_POP(llvm::BasicBlock *target,
                                          llvm::BasicBlock *fallthrough)
{
    BasicBlock *true_path =
        BasicBlock::Create("JUMP_IF_FALSE_OR_POP_pop", function());
    Value *test_value = Pop();
    Push(test_value);
    // IsPythonTrue() will steal the reference to test_value, so make sure
    // the stack owns one too.
    IncRef(test_value);
    Value *is_true = IsPythonTrue(test_value);
    builder().CreateCondBr(is_true, true_path, target);
    builder().SetInsertPoint(true_path);
    test_value = Pop();
    DecRef(test_value);
    builder().CreateBr(fallthrough);

}

void
LlvmFunctionBuilder::JUMP_IF_TRUE_OR_POP(llvm::BasicBlock *target,
                                         llvm::BasicBlock *fallthrough)
{
    BasicBlock *false_path =
        BasicBlock::Create("JUMP_IF_TRUE_OR_POP_pop", function());
    Value *test_value = Pop();
    Push(test_value);
    // IsPythonTrue() will steal the reference to test_value, so make sure
    // the stack owns one too.
    IncRef(test_value);
    Value *is_true = IsPythonTrue(test_value);
    builder().CreateCondBr(is_true, target, false_path);
    builder().SetInsertPoint(false_path);
    test_value = Pop();
    DecRef(test_value);
    builder().CreateBr(fallthrough);
}

void
LlvmFunctionBuilder::STORE_FAST(int index)
{
    SetLocal(index, Pop());
}

void
LlvmFunctionBuilder::DELETE_FAST(int index)
{
    BasicBlock *failure =
        BasicBlock::Create("DELETE_FAST_failure", function());
    BasicBlock *success =
        BasicBlock::Create("DELETE_FAST_success", function());
    Value *local_slot = builder().CreateGEP(
        this->fastlocals_, ConstantInt::get(Type::Int32Ty, index));
    Value *orig_value = builder().CreateLoad(
        local_slot, "DELETE_FAST_old_reference");
    builder().CreateCondBr(IsNull(orig_value), failure, success);

    builder().SetInsertPoint(failure);
    Function *do_raise = GetGlobalFunction<
        void(PyFrameObject *, int)>("_PyEval_RaiseForUnboundLocal");
    builder().CreateCall2(
        do_raise, this->frame_,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_),
                         index, true /* signed */));
    PropagateException();

    builder().SetInsertPoint(success);
    builder().CreateStore(
        Constant::getNullValue(TypeBuilder<PyObject *>::cache(this->module_)),
        local_slot);
    DecRef(orig_value);
}

void
LlvmFunctionBuilder::SETUP_LOOP(llvm::BasicBlock *target,
                                llvm::BasicBlock *fallthrough)
{
    CallBlockSetup(::SETUP_LOOP, target);
}

void
LlvmFunctionBuilder::GET_ITER()
{
    Value *obj = Pop();
    Function *pyobject_getiter = GetGlobalFunction<PyObject*(PyObject*)>(
        "PyObject_GetIter");
    Value *iter = builder().CreateCall(pyobject_getiter, obj);
    DecRef(obj);
    PropagateExceptionOnNull(iter);
    Push(iter);
}

void
LlvmFunctionBuilder::FOR_ITER(llvm::BasicBlock *target,
                              llvm::BasicBlock *fallthrough)
{
    Value *iter = Pop();
    Value *iter_tp = builder().CreateBitCast(
        builder().CreateLoad(
            builder().CreateStructGEP(iter, ObjectTy::FIELD_TYPE)),
        TypeBuilder<PyTypeObject *>::cache(this->module_),
        "iter_type");
    Value *iternext = builder().CreateLoad(
        builder().CreateStructGEP(iter_tp, TypeTy::FIELD_ITERNEXT),
        "iternext");
    Value *next = builder().CreateCall(iternext, iter, "next");
    BasicBlock *got_next = BasicBlock::Create("got_next", function());
    BasicBlock *next_null = BasicBlock::Create("next_null", function());
    builder().CreateCondBr(IsNull(next), next_null, got_next);

    builder().SetInsertPoint(next_null);
    Value *err_occurred = builder().CreateCall(
        GetGlobalFunction<PyObject*()>("PyErr_Occurred"));
    BasicBlock *iter_ended = BasicBlock::Create("iter_ended", function());
    BasicBlock *exception = BasicBlock::Create("exception", function());
    builder().CreateCondBr(IsNull(err_occurred), iter_ended, exception);

    builder().SetInsertPoint(exception);
    Value *exc_stopiteration = builder().CreateLoad(
        GetGlobalVariable<PyObject*>("PyExc_StopIteration"));
    Value *was_stopiteration = builder().CreateCall(
        GetGlobalFunction<int(PyObject *)>("PyErr_ExceptionMatches"),
        exc_stopiteration);
    BasicBlock *clear_err = BasicBlock::Create("clear_err", function());
    BasicBlock *propagate = BasicBlock::Create("propagate", function());
    builder().CreateCondBr(IsNonZero(was_stopiteration), clear_err, propagate);

    builder().SetInsertPoint(propagate);
    DecRef(iter);
    PropagateException();

    builder().SetInsertPoint(clear_err);
    builder().CreateCall(GetGlobalFunction<void()>("PyErr_Clear"));
    builder().CreateBr(iter_ended);

    builder().SetInsertPoint(iter_ended);
    DecRef(iter);
    builder().CreateBr(target);

    builder().SetInsertPoint(got_next);
    Push(iter);
    Push(next);
}

void
LlvmFunctionBuilder::POP_BLOCK()
{
    Value *block_info = builder().CreateCall(
        GetGlobalFunction<PyTryBlock *(PyFrameObject *)>("PyFrame_BlockPop"),
        this->frame_);
    Value *pop_to_level = builder().CreateLoad(
        builder().CreateStructGEP(block_info,
                                  TypeBuilder<PyTryBlock>::FIELD_LEVEL));
    Value *pop_to_addr = builder().CreateGEP(this->stack_bottom_, pop_to_level);
    PopAndDecrefTo(pop_to_addr);
}

void
LlvmFunctionBuilder::SETUP_EXCEPT(llvm::BasicBlock *target,
                                   llvm::BasicBlock *fallthrough)
{
    CallBlockSetup(::SETUP_EXCEPT, target);
}

void
LlvmFunctionBuilder::SETUP_FINALLY(llvm::BasicBlock *target,
                                   llvm::BasicBlock *fallthrough)
{
    CallBlockSetup(::SETUP_FINALLY, target);
}

void
LlvmFunctionBuilder::END_FINALLY()
{
    Value *finally_discriminator = Pop();
    // END_FINALLY is fairly complicated. It decides what to do based
    // on the top value in the stack.  If that value is an int, it's
    // interpreted as one of the unwind reasons.  If it's an exception
    // type, the next two stack values are the rest of the exception,
    // and it's re-raised.  Otherwise, it's supposed to be None,
    // indicating that the finally was entered through normal control
    // flow.

    BasicBlock *unwind_code = BasicBlock::Create("unwind_code", function());
    BasicBlock *test_exception =
        BasicBlock::Create("test_exception", function());
    BasicBlock *reraise_exception =
        BasicBlock::Create("reraise_exception", function());
    BasicBlock *check_none = BasicBlock::Create("check_none", function());
    BasicBlock *not_none = BasicBlock::Create("not_none", function());
    BasicBlock *finally_fallthrough =
        BasicBlock::Create("finally_fallthrough", function());

    builder().CreateCondBr(
        IsInstanceOfFlagClass(finally_discriminator, Py_TPFLAGS_INT_SUBCLASS),
        unwind_code, test_exception);

    builder().SetInsertPoint(unwind_code);
    // The top of the stack was an int, interpreted as an unwind code.
    // If we're resuming a return or continue, the return value or
    // loop target (respectively) is now on top of the stack and needs
    // to be popped off.
    Value *unwind_reason = builder().CreateTrunc(
        builder().CreateCall(
            GetGlobalFunction<long(PyObject *)>("PyInt_AsLong"),
            finally_discriminator),
        Type::Int8Ty,
        "unwind_reason");
    DecRef(finally_discriminator);
    // Save the unwind reason for when we jump to the unwind block.
    builder().CreateStore(unwind_reason, this->unwind_reason_addr_);
    // Check if we need to pop the return value or loop target.
    BasicBlock *pop_retval = BasicBlock::Create("pop_retval", function());
    llvm::SwitchInst *should_pop_retval =
        builder().CreateSwitch(unwind_reason, this->unwind_block_, 2);
    should_pop_retval->addCase(ConstantInt::get(Type::Int8Ty, UNWIND_RETURN),
                               pop_retval);
    should_pop_retval->addCase(ConstantInt::get(Type::Int8Ty, UNWIND_CONTINUE),
                               pop_retval);

    builder().SetInsertPoint(pop_retval);
    // We're continuing a return or continue.  Retrieve its argument.
    builder().CreateStore(Pop(), this->retval_addr_);
    builder().CreateBr(this->unwind_block_);

    builder().SetInsertPoint(test_exception);
    Value *is_exception_or_string = builder().CreateCall(
        GetGlobalFunction<int(PyObject *)>("_PyLlvm_WrapIsExceptionOrString"),
        finally_discriminator);
    builder().CreateCondBr(IsNonZero(is_exception_or_string),
                           reraise_exception, check_none);

    builder().SetInsertPoint(reraise_exception);
    Value *err_type = finally_discriminator;
    Value *err_value = Pop();
    Value *err_traceback = Pop();
    builder().CreateCall3(
        GetGlobalFunction<void(PyObject *, PyObject *, PyObject *)>(
            "PyErr_Restore"),
        err_type, err_value, err_traceback);
    // Logically this is an UNWIND_RERAISE, but all of the exception
    // handling logic expects UNWIND_EXCEPTION.  The only difference
    // in EvalFrameEx is that UNWIND_EXCEPTION calls
    // PyTraceBack_Here(frame) and traces the exception.
    builder().CreateStore(ConstantInt::get(Type::Int8Ty, UNWIND_EXCEPTION),
                          this->unwind_reason_addr_);
    builder().CreateBr(this->unwind_block_);

    builder().SetInsertPoint(check_none);
    // The contents of the try block push None onto the stack just
    // before falling through to the finally block.  If we didn't get
    // an unwind reason or an exception, we expect to fall through,
    // but for sanity we also double-check that the None is present.
    Value *is_none = builder().CreateICmpEQ(
        finally_discriminator, GetGlobalVariable<PyObject>("_Py_NoneStruct"));
    DecRef(finally_discriminator);
    builder().CreateCondBr(is_none, finally_fallthrough, not_none);

    builder().SetInsertPoint(not_none);
    // If we didn't get a None, raise a SystemError.
    Value *system_error = builder().CreateLoad(GetGlobalVariable<PyObject *>(
                                                   "PyExc_SystemError"));
    Value *err_msg =
        this->llvm_data_->GetGlobalStringPtr("'finally' pops bad exception");
    builder().CreateCall2(GetGlobalFunction<void(PyObject *, const char *)>(
                              "PyErr_SetString"),
                          system_error, err_msg);
    builder().CreateStore(ConstantInt::get(Type::Int8Ty, UNWIND_EXCEPTION),
                          this->unwind_reason_addr_);
    builder().CreateBr(this->unwind_block_);

    // After falling through into a finally block, we also fall
    // through out of the block.  This has the nice side-effect of
    // avoiding jumps and switch instructions in the common case,
    // although returning out of a finally may still be slower than
    // ideal.
    builder().SetInsertPoint(finally_fallthrough);
}

void
LlvmFunctionBuilder::BREAK_LOOP()
{
    // Accept code after a break statement, even though it's never executed.
    // Otherwise, CPython's willingness to insert code after block
    // terminators causes problems.
    BasicBlock *dead_code = BasicBlock::Create("dead_code", function());
    builder().CreateStore(ConstantInt::get(Type::Int8Ty, UNWIND_BREAK),
                          this->unwind_reason_addr_);
    builder().CreateBr(this->unwind_block_);

    builder().SetInsertPoint(dead_code);
}

void
LlvmFunctionBuilder::RETURN_VALUE()
{
    Value *retval = Pop();
    Return(retval);
}

void
LlvmFunctionBuilder::DoRaise(Value *exc_type, Value *exc_inst, Value *exc_tb)
{
    // Accept code after a raise statement, even though it's never executed.
    // Otherwise, CPython's willingness to insert code after block
    // terminators causes problems.
    BasicBlock *dead_code = BasicBlock::Create("dead_code", function());

    Function *do_raise = GetGlobalFunction<
        int(PyObject*, PyObject *, PyObject *)>("_PyEval_DoRaise");
    // _PyEval_DoRaise eats references.
    Value *is_reraise = builder().CreateCall3(
        do_raise, exc_type, exc_inst, exc_tb, "raise_is_reraise");
    // TODO(twouters): We should be handling re-raises and new exceptions
    // differently, but we don't, yet. If is_reraise is non-zero,
    // we should omit building a new traceback and jump to the correct
    // handler (and/or set the unwind reason to UNWIND_RERAISE).
    builder().CreateStore(
        ConstantInt::get(Type::Int8Ty, UNWIND_EXCEPTION),
        this->unwind_reason_addr_);
    builder().CreateStore(
        Constant::getNullValue(TypeBuilder<PyObject*>::cache(this->module_)),
        this->retval_addr_);
    builder().CreateBr(this->unwind_block_);

    builder().SetInsertPoint(dead_code);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_ZERO()
{
    Value *exc_tb = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_inst = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_type = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_ONE()
{
    Value *exc_tb = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_inst = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_type = Pop();
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_TWO()
{
    Value *exc_tb = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_inst = Pop();
    Value *exc_type = Pop();
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_THREE()
{
    Value *exc_tb = Pop();
    Value *exc_inst = Pop();
    Value *exc_type = Pop();
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::STORE_SUBSCR()
{
    // Performing obj[key] = val
    Value *key = Pop();
    Value *obj = Pop();
    Value *value = Pop();
    Function *setitem = GetGlobalFunction<
          int(PyObject *, PyObject *, PyObject *)>("PyObject_SetItem");
    Value *result = builder().CreateCall3(setitem, obj, key, value,
                                          "STORE_SUBSCR_result");
    DecRef(value);
    DecRef(obj);
    DecRef(key);
    PropagateExceptionOnNonZero(result);
}

void
LlvmFunctionBuilder::DELETE_SUBSCR()
{
    Value *key = Pop();
    Value *obj = Pop();
    Function *delitem = GetGlobalFunction<
          int(PyObject *, PyObject *)>("PyObject_DelItem");
    Value *result = builder().CreateCall2(delitem, obj, key,
                                          "DELETE_SUBSCR_result");
    DecRef(obj);
    DecRef(key);
    PropagateExceptionOnNonZero(result);
}

// Common code for almost all binary operations
void
LlvmFunctionBuilder::GenericBinOp(const char *apifunc)
{
    Value *rhs = Pop();
    Value *lhs = Pop();
    Function *op =
        GetGlobalFunction<PyObject*(PyObject*, PyObject*)>(apifunc);
    Value *result = builder().CreateCall2(op, lhs, rhs, "binop_result");
    DecRef(lhs);
    DecRef(rhs);
    PropagateExceptionOnNull(result);
    Push(result);
}

#define BINOP_METH(OPCODE, APIFUNC) 		\
void						\
LlvmFunctionBuilder::OPCODE()			\
{						\
    GenericBinOp(#APIFUNC);			\
}

BINOP_METH(BINARY_ADD, PyNumber_Add)
BINOP_METH(BINARY_SUBTRACT, PyNumber_Subtract)
BINOP_METH(BINARY_MULTIPLY, PyNumber_Multiply)
BINOP_METH(BINARY_TRUE_DIVIDE, PyNumber_TrueDivide)
BINOP_METH(BINARY_DIVIDE, PyNumber_Divide)
BINOP_METH(BINARY_MODULO, PyNumber_Remainder)
BINOP_METH(BINARY_LSHIFT, PyNumber_Lshift)
BINOP_METH(BINARY_RSHIFT, PyNumber_Rshift)
BINOP_METH(BINARY_OR, PyNumber_Or)
BINOP_METH(BINARY_XOR, PyNumber_Xor)
BINOP_METH(BINARY_AND, PyNumber_And)
BINOP_METH(BINARY_FLOOR_DIVIDE, PyNumber_FloorDivide)
BINOP_METH(BINARY_SUBSCR, PyObject_GetItem)

BINOP_METH(INPLACE_ADD, PyNumber_InPlaceAdd)
BINOP_METH(INPLACE_SUBTRACT, PyNumber_InPlaceSubtract)
BINOP_METH(INPLACE_MULTIPLY, PyNumber_InPlaceMultiply)
BINOP_METH(INPLACE_TRUE_DIVIDE, PyNumber_InPlaceTrueDivide)
BINOP_METH(INPLACE_DIVIDE, PyNumber_InPlaceDivide)
BINOP_METH(INPLACE_MODULO, PyNumber_InPlaceRemainder)
BINOP_METH(INPLACE_LSHIFT, PyNumber_InPlaceLshift)
BINOP_METH(INPLACE_RSHIFT, PyNumber_InPlaceRshift)
BINOP_METH(INPLACE_OR, PyNumber_InPlaceOr)
BINOP_METH(INPLACE_XOR, PyNumber_InPlaceXor)
BINOP_METH(INPLACE_AND, PyNumber_InPlaceAnd)
BINOP_METH(INPLACE_FLOOR_DIVIDE, PyNumber_InPlaceFloorDivide)

#undef BINOP_METH

// PyNumber_Power() and PyNumber_InPlacePower() take three arguments, the
// third should be Py_None when calling from BINARY_POWER/INPLACE_POWER.
void
LlvmFunctionBuilder::GenericPowOp(const char *apifunc)
{
    Value *rhs = Pop();
    Value *lhs = Pop();
    Function *op = GetGlobalFunction<PyObject*(PyObject*, PyObject*,
        PyObject *)>(apifunc);
    Value *pynone = GetGlobalVariable<PyObject>("_Py_NoneStruct");
    Value *result = builder().CreateCall3(op, lhs, rhs, pynone,
                                          "powop_result");
    DecRef(lhs);
    DecRef(rhs);
    PropagateExceptionOnNull(result);
    Push(result);
}

void
LlvmFunctionBuilder::BINARY_POWER()
{
    GenericPowOp("PyNumber_Power");
}

void
LlvmFunctionBuilder::INPLACE_POWER()
{
    GenericPowOp("PyNumber_InPlacePower");
}

// Implementation of almost all unary operations
void
LlvmFunctionBuilder::GenericUnaryOp(const char *apifunc)
{
    Value *value = Pop();
    Function *op = GetGlobalFunction<PyObject*(PyObject*)>(apifunc);
    Value *result = builder().CreateCall(op, value, "unaryop_result");
    DecRef(value);
    PropagateExceptionOnNull(result);
    Push(result);
}

#define UNARYOP_METH(NAME, APIFUNC)			\
void							\
LlvmFunctionBuilder::NAME()				\
{							\
    GenericUnaryOp(#APIFUNC);				\
}

UNARYOP_METH(UNARY_CONVERT, PyObject_Repr)
UNARYOP_METH(UNARY_INVERT, PyNumber_Invert)
UNARYOP_METH(UNARY_POSITIVE, PyNumber_Positive)
UNARYOP_METH(UNARY_NEGATIVE, PyNumber_Negative)

#undef UNARYOP_METH

void
LlvmFunctionBuilder::UNARY_NOT()
{
    Value *value = Pop();
    Value *retval = builder().CreateSelect(
        IsPythonTrue(value),
        GetGlobalVariable<PyObject>("_Py_ZeroStruct"),
        GetGlobalVariable<PyObject>("_Py_TrueStruct"),
        "UNARY_NOT_result");
    IncRef(retval);
    Push(retval);
}

void
LlvmFunctionBuilder::POP_TOP()
{
    DecRef(Pop());
}

void
LlvmFunctionBuilder::DUP_TOP()
{
    Value *first = Pop();
    IncRef(first);
    Push(first);
    Push(first);
}

void
LlvmFunctionBuilder::DUP_TOP_TWO()
{
    Value *first = Pop();
    Value *second = Pop();
    IncRef(first);
    IncRef(second);
    Push(second);
    Push(first);
    Push(second);
    Push(first);
}

void
LlvmFunctionBuilder::DUP_TOP_THREE()
{
    Value *first = Pop();
    Value *second = Pop();
    Value *third = Pop();
    IncRef(first);
    IncRef(second);
    IncRef(third);
    Push(third);
    Push(second);
    Push(first);
    Push(third);
    Push(second);
    Push(first);
}

void
LlvmFunctionBuilder::ROT_TWO()
{
    Value *first = Pop();
    Value *second = Pop();
    Push(first);
    Push(second);
}

void
LlvmFunctionBuilder::ROT_THREE()
{
    Value *first = Pop();
    Value *second = Pop();
    Value *third = Pop();
    Push(first);
    Push(third);
    Push(second);
}

void
LlvmFunctionBuilder::ROT_FOUR()
{
    Value *first = Pop();
    Value *second = Pop();
    Value *third = Pop();
    Value *fourth = Pop();
    Push(first);
    Push(fourth);
    Push(third);
    Push(second);
}

void
LlvmFunctionBuilder::RichCompare(Value *lhs, Value *rhs, int cmp_op)
{
    Function *pyobject_richcompare = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, int)>("PyObject_RichCompare");
    Value *result = builder().CreateCall3(
        pyobject_richcompare, lhs, rhs,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), cmp_op),
        "COMPARE_OP_RichCompare_result");
    DecRef(lhs);
    DecRef(rhs);
    PropagateExceptionOnNull(result);
    Push(result);
}

Value *
LlvmFunctionBuilder::ContainerContains(Value *container, Value *item)
{
    Function *contains =
        GetGlobalFunction<int(PyObject *, PyObject *)>("PySequence_Contains");
    Value *result = builder().CreateCall2(
        contains, container, item, "ContainerContains_result");
    DecRef(item);
    DecRef(container);
    PropagateExceptionOnNegative(result);
    return IsPositive(result);
}

// TODO(twouters): test this (used in exception handling.)
Value *
LlvmFunctionBuilder::ExceptionMatches(Value *exc, Value *exc_type)
{
    Function *exc_matches = GetGlobalFunction<
        int(PyObject *, PyObject *)>("_PyEval_CheckedExceptionMatches");
    Value *result = builder().CreateCall2(
        exc_matches, exc, exc_type, "ExceptionMatches_result");
    DecRef(exc_type);
    DecRef(exc);
    PropagateExceptionOnNegative(result);
    return IsPositive(result);
}

void
LlvmFunctionBuilder::COMPARE_OP(int cmp_op)
{
    Value *rhs = Pop();
    Value *lhs = Pop();
    Value *result;
    switch (cmp_op) {
    case PyCmp_IS:
        result = builder().CreateICmpEQ(lhs, rhs, "COMPARE_OP_is_same");
        DecRef(lhs);
        DecRef(rhs);
        break;
    case PyCmp_IS_NOT:
        result = builder().CreateICmpNE(lhs, rhs, "COMPARE_OP_is_not_same");
        DecRef(lhs);
        DecRef(rhs);
        break;
    case PyCmp_IN:
        // item in seq -> ContainerContains(seq, item)
        result = ContainerContains(rhs, lhs);
        break;
    case PyCmp_NOT_IN:
    {
        Value *inverted_result = ContainerContains(rhs, lhs);
        result = builder().CreateICmpEQ(
            inverted_result, ConstantInt::get(inverted_result->getType(), 0),
            "COMPARE_OP_not_in_result");
        break;
    }
    case PyCmp_EXC_MATCH:
        result = ExceptionMatches(lhs, rhs);
        break;
    case PyCmp_EQ:
    case PyCmp_NE:
    case PyCmp_LT:
    case PyCmp_LE:
    case PyCmp_GT:
    case PyCmp_GE:
        RichCompare(lhs, rhs, cmp_op);
        return;
    default:
        Py_FatalError("unknown COMPARE_OP oparg");;
    }
    Value *value = builder().CreateSelect(
        result,
        GetGlobalVariable<PyObject>("_Py_TrueStruct"),
        GetGlobalVariable<PyObject>("_Py_ZeroStruct"),
        "COMPARE_OP_result");
    IncRef(value);
    Push(value);
}

void
LlvmFunctionBuilder::LIST_APPEND()
{
    Value *item = Pop();
    Value *listobj = Pop();
    Function *list_append = GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyList_Append");
    Value *result = builder().CreateCall2(list_append, listobj, item,
                                          "LIST_APPEND_result");
    DecRef(listobj);
    DecRef(item);
    PropagateExceptionOnNonZero(result);
}

void
LlvmFunctionBuilder::STORE_MAP()
{
    Value *key = Pop();
    Value *value = Pop();
    Value *dict = Pop();
    Push(dict);
    // XXX(twouters): ceval does an assert(PyDict_CheckExact(dict);
    // add it here if/when it's convenient to do in IRBuilder.
    Function *setitem = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyDict_SetItem");
    Value *result = builder().CreateCall3(setitem, dict, key, value,
                                          "STORE_MAP_result");
    DecRef(value);
    DecRef(key);
    PropagateExceptionOnNonZero(result);
}

Value *
LlvmFunctionBuilder::GetListItemSlot(Value *lst, int idx)
{
    Value *listobj = builder().CreateBitCast(
        lst, TypeBuilder<PyListObject *>::cache(this->module_));
    // Load the target of the ob_item PyObject** into list_items.
    Value *list_items = builder().CreateLoad(
        builder().CreateStructGEP(listobj, ListTy::FIELD_ITEM));
    // GEP the list_items PyObject* up to the desired item
    return builder().CreateGEP(list_items,
                               ConstantInt::get(Type::Int32Ty, idx),
                               "list_item_slot");
}

Value *
LlvmFunctionBuilder::GetTupleItemSlot(Value *tup, int idx)
{
    Value *tupobj = builder().CreateBitCast(
        tup, TypeBuilder<PyTupleObject*>::cache(this->module_));
    // Make CreateGEP perform &tup_item_indices[0].ob_item[idx].
    Value *tup_item_indices[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, TupleTy::FIELD_ITEM),
        ConstantInt::get(Type::Int32Ty, idx),
    };
    return builder().CreateGEP(tupobj, tup_item_indices,
                               array_endof(tup_item_indices),
                               "tuple_item_slot");
}

void
LlvmFunctionBuilder::BuildSequenceLiteral(
    int size, const char *createname,
    Value *(LlvmFunctionBuilder::*getitemslot)(Value*, int))
{
    const Type *IntSsizeTy = TypeBuilder<Py_ssize_t>::cache(this->module_);
    Value *seqsize = ConstantInt::get(IntSsizeTy, size, true /* signed */);

    Function *create = GetGlobalFunction<PyObject *(Py_ssize_t)>(createname);
    Value *seq = builder().CreateCall(create, seqsize, "sequence_literal");
    PropagateExceptionOnNull(seq);

    // XXX(twouters): do this with a memcpy?
    while (--size >= 0) {
        Value *itemslot = (this->*getitemslot)(seq, size);
        builder().CreateStore(Pop(), itemslot);
    }
    Push(seq);
}

void
LlvmFunctionBuilder::BUILD_LIST(int size)
{
   BuildSequenceLiteral(size, "PyList_New",
                        &LlvmFunctionBuilder::GetListItemSlot);
}

void
LlvmFunctionBuilder::BUILD_TUPLE(int size)
{
   BuildSequenceLiteral(size, "PyTuple_New",
                        &LlvmFunctionBuilder::GetTupleItemSlot);
}

void
LlvmFunctionBuilder::BUILD_MAP(int size)
{
    Value *sizehint = ConstantInt::get(
        TypeBuilder<Py_ssize_t>::cache(this->module_),
        size, true /* signed */);
    Function *create_dict = GetGlobalFunction<
        PyObject *(Py_ssize_t)>("_PyDict_NewPresized");
    Value *result = builder().CreateCall(create_dict, sizehint,
                                         "BULD_MAP_result");
    PropagateExceptionOnNull(result);
    Push(result);
}

void
LlvmFunctionBuilder::ApplySlice(Value *seq, Value *start, Value *stop)
{
    Function *build_slice = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, PyObject *)>("_PyEval_ApplySlice");
    Value *result = builder().CreateCall3(
        build_slice, seq, start, stop, "ApplySlice_result");
    XDecRef(stop);
    XDecRef(start);
    DecRef(seq);
    PropagateExceptionOnNull(result);
    Push(result);
}

void
LlvmFunctionBuilder::SLICE_BOTH()
{
    Value *stop = Pop();
    Value *start = Pop();
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

void
LlvmFunctionBuilder::SLICE_LEFT()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Pop();
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

void
LlvmFunctionBuilder::SLICE_RIGHT()
{
    Value *stop = Pop();
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

void
LlvmFunctionBuilder::SLICE_NONE()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

void
LlvmFunctionBuilder::AssignSlice(Value *seq, Value *start, Value *stop,
                                 Value *source)
{
    Function *assign_slice = GetGlobalFunction<
        int (PyObject *, PyObject *, PyObject *, PyObject *)>(
            "_PyEval_AssignSlice");
    Value *result = builder().CreateCall4(
        assign_slice, seq, start, stop, source, "ApplySlice_result");
    XDecRef(source);
    XDecRef(stop);
    XDecRef(start);
    DecRef(seq);
    PropagateExceptionOnNonZero(result);
}

void
LlvmFunctionBuilder::STORE_SLICE_BOTH()
{
    Value *stop = Pop();
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::STORE_SLICE_LEFT()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::STORE_SLICE_RIGHT()
{
    Value *stop = Pop();
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::STORE_SLICE_NONE()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_BOTH()
{
    Value *stop = Pop();
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_LEFT()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_RIGHT()
{
    Value *stop = Pop();
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_NONE()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::BUILD_SLICE_TWO()
{
    Value *step = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *stop = Pop();
    Value *start = Pop();
    Function *build_slice = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, PyObject *)>("PySlice_New");
    Value *result = builder().CreateCall3(
        build_slice, start, stop, step, "BUILD_SLICE_result");
    DecRef(start);
    DecRef(stop);
    PropagateExceptionOnNull(result);
    Push(result);
}

void
LlvmFunctionBuilder::BUILD_SLICE_THREE()
{
    Value *step = Pop();
    Value *stop = Pop();
    Value *start = Pop();
    Function *build_slice = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, PyObject *)>("PySlice_New");
    Value *result = builder().CreateCall3(
        build_slice, start, stop, step, "BUILD_SLICE_result");
    DecRef(start);
    DecRef(stop);
    DecRef(step);
    PropagateExceptionOnNull(result);
    Push(result);
}

void
LlvmFunctionBuilder::UNPACK_SEQUENCE(int size)
{
    // TODO(twouters): we could speed up the common case quite a bit by
    // doing the unpacking inline, like ceval.cc does; that would allow
    // LLVM to optimize the heck out of it as well. Then again, we could
    // do even better by combining this opcode and the STORE_* ones that
    // follow into a single block of code circumventing the stack
    // altogether. And omitting the horrible external stack munging that
    // UnpackIterable does.
    Value *iterable = Pop();
    Function *unpack_iterable = GetGlobalFunction<
        int(PyObject *, int, PyObject **)>("_PyEval_UnpackIterable");
    Value *new_stack_pointer = builder().CreateGEP(
        builder().CreateLoad(this->stack_pointer_addr_),
        ConstantInt::get(TypeBuilder<Py_ssize_t>::cache(this->module_),
                         size, true /* signed */));
    Value *result = builder().CreateCall3(
        unpack_iterable, iterable,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), size, true),
        // _PyEval_UnpackIterable really takes the *new* stack pointer as
        // an argument, because it builds the result stack in reverse.
        new_stack_pointer);
    DecRef(iterable);
    PropagateExceptionOnNonZero(result);
    // Not setting the new stackpointer on failure does mean that if
    // _PyEval_UnpackIterable failed after pushing some values onto the
    // stack, and it didn't clean up after itself, we lose references.  This
    // is what ceval.cc does as well.
    builder().CreateStore(new_stack_pointer, this->stack_pointer_addr_);
}

// Adds delta to *addr, and returns the new value.
static Value *
increment_and_get(llvm::IRBuilder<>& builder, Value *addr, int64_t delta)
{
    Value *orig = builder.CreateLoad(addr);
    Value *new_ = builder.CreateAdd(
        orig,
        get_signed_constant_int(orig->getType(), delta));
    builder.CreateStore(new_, addr);
    return new_;
}

void
LlvmFunctionBuilder::IncRef(Value *value)
{
#ifdef Py_REF_DEBUG
    // Increment the global reference count.
    Value *reftotal_addr = GetGlobalVariable<Py_ssize_t>("_Py_RefTotal");
    increment_and_get(builder(), reftotal_addr, 1);
#endif

    Value *as_pyobject = builder().CreateBitCast(
        value, TypeBuilder<PyObject*>::cache(this->module_));
    Value *refcnt_addr =
        builder().CreateStructGEP(as_pyobject, ObjectTy::FIELD_REFCNT);
    increment_and_get(builder(), refcnt_addr, 1);
}

void
LlvmFunctionBuilder::DecRef(Value *value)
{
#ifdef Py_REF_DEBUG
    // Decrement the global reference count.
    Value *reftotal_addr = GetGlobalVariable<Py_ssize_t>("_Py_RefTotal");
    increment_and_get(builder(), reftotal_addr, -1);
#endif

    Value *as_pyobject = builder().CreateBitCast(
        value, TypeBuilder<PyObject*>::cache(this->module_));
    Value *refcnt_addr =
        builder().CreateStructGEP(as_pyobject, ObjectTy::FIELD_REFCNT);
    Value *new_refcnt = increment_and_get(builder(), refcnt_addr, -1);

    // Check if we need to deallocate the object.
    BasicBlock *block_dealloc = BasicBlock::Create("dealloc", this->function_);
    BasicBlock *block_tail = BasicBlock::Create("decref_tail", this->function_);
    BasicBlock *block_ref_ne_zero = block_tail;
#ifdef Py_REF_DEBUG
    block_ref_ne_zero = BasicBlock::Create("check_refcnt", this->function_);
#endif

    builder().CreateCondBr(IsNonZero(new_refcnt),
                           block_ref_ne_zero, block_dealloc);

#ifdef Py_REF_DEBUG
    builder().SetInsertPoint(block_ref_ne_zero);
    Value *less_zero = builder().CreateICmpSLT(
        new_refcnt, Constant::getNullValue(new_refcnt->getType()));
    BasicBlock *block_ref_lt_zero = BasicBlock::Create("negative_refcount",
                                                 this->function_);
    builder().CreateCondBr(less_zero, block_ref_lt_zero, block_tail);

    builder().SetInsertPoint(block_ref_lt_zero);
    Value *neg_refcount = GetGlobalFunction<void(const char*, int, PyObject*)>(
        "_Py_NegativeRefcount");
    // TODO: Well that __FILE__ and __LINE__ are going to be useless!
    builder().CreateCall3(
        neg_refcount,
        this->llvm_data_->GetGlobalStringPtr(__FILE__),
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), __LINE__),
        as_pyobject);
    builder().CreateBr(block_tail);
#endif

    builder().SetInsertPoint(block_dealloc);
    Value *dealloc = GetGlobalFunction<void(PyObject *)>("_PyLlvm_WrapDealloc");
    builder().CreateCall(dealloc, as_pyobject);
    builder().CreateBr(block_tail);

    builder().SetInsertPoint(block_tail);
}

void
LlvmFunctionBuilder::XDecRef(Value *value)
{
    BasicBlock *do_decref = BasicBlock::Create("decref", function());
    BasicBlock *decref_end = BasicBlock::Create("decref_end", function());
    builder().CreateCondBr(IsNull(value), decref_end, do_decref);

    builder().SetInsertPoint(do_decref);
    DecRef(value);
    builder().CreateBr(decref_end);

    builder().SetInsertPoint(decref_end);
}

void
LlvmFunctionBuilder::Push(Value *value)
{
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    builder().CreateStore(value, stack_pointer);
    Value *new_stack_pointer = builder().CreateGEP(
        stack_pointer, ConstantInt::get(Type::Int32Ty, 1));
    builder().CreateStore(new_stack_pointer, this->stack_pointer_addr_);
}

Value *
LlvmFunctionBuilder::Pop()
{
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    Value *new_stack_pointer = builder().CreateGEP(
        stack_pointer, get_signed_constant_int(Type::Int32Ty, -1));
    Value *former_top = builder().CreateLoad(new_stack_pointer);
    builder().CreateStore(new_stack_pointer, this->stack_pointer_addr_);
    return former_top;
}

Value *
LlvmFunctionBuilder::GetStackLevel()
{
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    Value *stack_pointer_int =
        builder().CreatePtrToInt(stack_pointer, Type::Int64Ty);
    Value *stack_bottom_int =
        builder().CreatePtrToInt(this->stack_bottom_, Type::Int64Ty);
    Value *difference =
        builder().CreateSub(stack_pointer_int, stack_bottom_int);
    Value *level64 = builder().CreateSDiv(
        difference,
        llvm::ConstantExpr::getSizeOf(
            TypeBuilder<PyObject*>::cache(this->module_)));
    // The stack level is stored as an int, not an int64.
    return builder().CreateTrunc(level64,
                                 TypeBuilder<int>::cache(this->module_),
                                 "stack_level");
}

void
LlvmFunctionBuilder::SetLocal(int locals_index, llvm::Value *new_value)
{
    Value *local_slot = builder().CreateGEP(
        this->fastlocals_, ConstantInt::get(Type::Int32Ty, locals_index));
    Value *orig_value = builder().CreateLoad(local_slot, "local_overwritten");
    builder().CreateStore(new_value, local_slot);
    XDecRef(orig_value);
}

void
LlvmFunctionBuilder::CallBlockSetup(int block_type, llvm::BasicBlock *handler) {
    Value *stack_level = GetStackLevel();
    Value *unwind_target_index = AddUnwindTarget(handler);
    Function *blocksetup =
        GetGlobalFunction<void(PyFrameObject *, int, int, int)>(
            "PyFrame_BlockSetup");
    builder().CreateCall4(
        blocksetup, this->frame_,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), block_type),
        unwind_target_index,
        stack_level);
}

void
LlvmFunctionBuilder::InsertAbort(const char *opcode_name)
{
    std::string message("Undefined opcode: ");
    message.append(opcode_name);
    builder().CreateCall(GetGlobalFunction<int(const char*)>("puts"),
                         this->llvm_data_->GetGlobalStringPtr(message));
    builder().CreateCall(GetGlobalFunction<void()>("abort"));
}

template<typename FunctionType> Function *
LlvmFunctionBuilder::GetGlobalFunction(const std::string &name)
{
    return llvm::cast<Function>(
        this->module_->getOrInsertFunction(
            name, TypeBuilder<FunctionType>::cache(this->module_)));
}

template<typename VariableType> Constant *
LlvmFunctionBuilder::GetGlobalVariable(const std::string &name)
{
    return this->module_->getOrInsertGlobal(
        name, TypeBuilder<VariableType>::cache(this->module_));
}

Value *
LlvmFunctionBuilder::IsNull(Value *value)
{
    return builder().CreateICmpEQ(
        value, Constant::getNullValue(value->getType()));
}

Value *
LlvmFunctionBuilder::IsNonZero(Value *value)
{
    return builder().CreateICmpNE(
        value, Constant::getNullValue(value->getType()));
}

Value *
LlvmFunctionBuilder::IsNegative(Value *value)
{
    return builder().CreateICmpSLT(
        value, ConstantInt::get(value->getType(), 0, true /* signed */));
}

Value *
LlvmFunctionBuilder::IsPositive(Value *value)
{
    return builder().CreateICmpSGT(
        value, ConstantInt::get(value->getType(), 0, true /* signed */));
}

Value *
LlvmFunctionBuilder::IsInstanceOfFlagClass(llvm::Value *value, int flag)
{
    Value *type = builder().CreateBitCast(
        builder().CreateLoad(
            builder().CreateStructGEP(value, ObjectTy::FIELD_TYPE),
            "type"),
        TypeBuilder<PyTypeObject *>::cache(this->module_));
    Value *type_flags = builder().CreateLoad(
        builder().CreateStructGEP(type, TypeTy::FIELD_FLAGS),
        "type_flags");
    Value *is_instance = builder().CreateAnd(
        type_flags,
        ConstantInt::get(type_flags->getType(), flag));
    return IsNonZero(is_instance);
}

void
LlvmFunctionBuilder::PropagateExceptionOnNull(Value *value)
{
    BasicBlock *propagate =
        BasicBlock::Create("PropagateExceptionOnNull_propagate", function());
    BasicBlock *pass =
        BasicBlock::Create("PropagateExceptionOnNull_pass", function());
    builder().CreateCondBr(IsNull(value), propagate, pass);

    builder().SetInsertPoint(propagate);
    PropagateException();

    builder().SetInsertPoint(pass);
}

void
LlvmFunctionBuilder::PropagateExceptionOnNegative(Value *value)
{
    BasicBlock *propagate =
        BasicBlock::Create("PropagateExceptionOnNegative_propagate",
        function());
    BasicBlock *pass =
        BasicBlock::Create("PropagateExceptionOnNegative_pass", function());
    builder().CreateCondBr(IsNegative(value), propagate, pass);

    builder().SetInsertPoint(propagate);
    PropagateException();

    builder().SetInsertPoint(pass);
}

void
LlvmFunctionBuilder::PropagateExceptionOnNonZero(Value *value)
{
    BasicBlock *propagate =
        BasicBlock::Create("PropagateExceptionOnNonZero_propagate",
        function());
    BasicBlock *pass =
        BasicBlock::Create("PropagateExceptionOnNonZero_pass", function());
    builder().CreateCondBr(IsNonZero(value), propagate, pass);

    builder().SetInsertPoint(propagate);
    PropagateException();

    builder().SetInsertPoint(pass);
}

Value *
LlvmFunctionBuilder::LookupName(int name_index)
{
    Value *name = builder().CreateLoad(
        builder().CreateGEP(
            this->names_, ConstantInt::get(Type::Int32Ty, name_index),
            "constant_name"));
    return name;
}

llvm::Value *
LlvmFunctionBuilder::IsPythonTrue(Value *value)
{
    BasicBlock *not_py_true =
        BasicBlock::Create("IsPythonTrue_is_not_PyTrue", function());
    BasicBlock *not_py_false =
        BasicBlock::Create("IsPythonTrue_is_not_PyFalse", function());
    BasicBlock *decref_value =
        BasicBlock::Create("IsPythonTrue_decref_value", function());
    BasicBlock *done =
        BasicBlock::Create("IsPythonTrue_done", function());

    Value *result_addr =
        CreateAllocaInEntryBlock(Type::Int1Ty, NULL, "IsPythonTrue_result");
    Value *py_false = GetGlobalVariable<PyObject>("_Py_ZeroStruct");
    Value *py_true = GetGlobalVariable<PyObject>("_Py_TrueStruct");

    Value *is_PyTrue = builder().CreateICmpEQ(
        py_true, value, "IsPythonTrue_is_PyTrue");
    builder().CreateStore(is_PyTrue, result_addr);
    builder().CreateCondBr(is_PyTrue, decref_value, not_py_true);

    builder().SetInsertPoint(not_py_true);
    Value *is_not_PyFalse = builder().CreateICmpNE(
        py_false, value, "IsPythonTrue_is_PyFalse");
    builder().CreateStore(is_not_PyFalse, result_addr);
    builder().CreateCondBr(is_not_PyFalse, not_py_false, decref_value);

    builder().SetInsertPoint(not_py_false);
    Function *pyobject_istrue =
        GetGlobalFunction<int(PyObject *)>("PyObject_IsTrue");
    Value *istrue_result = builder().CreateCall(
        pyobject_istrue, value, "PyObject_IsTrue_result");
    DecRef(value);
    PropagateExceptionOnNegative(istrue_result);
    builder().CreateStore(
        IsPositive(istrue_result),
        result_addr);
    builder().CreateBr(done);

    builder().SetInsertPoint(decref_value);
    DecRef(value);
    builder().CreateBr(done);

    builder().SetInsertPoint(done);
    return builder().CreateLoad(result_addr);
}

}  // namespace py


// Helper functions for the LLVM IR. These exist for
// non-speed-critical code that's easier to write in C, or for calls
// that are functions in pydebug mode and macros otherwise.
extern "C" {

void
_PyLlvm_WrapDealloc(PyObject *obj)
{
    _Py_Dealloc(obj);
}

int
_PyLlvm_WrapIsExceptionOrString(PyObject *obj)
{
    return PyExceptionClass_Check(obj) || PyString_Check(obj);
}

// Copied from the SETUP_FINALLY && WHY_EXCEPTION block in
// fast_block_end in PyEval_EvalFrameEx().
void
_PyLlvm_WrapEnterExceptOrFinally(py::ExcInfo *exc_info, int block_type)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyErr_Fetch(&exc_info->exc, &exc_info->val, &exc_info->tb);
    if (exc_info->val == NULL) {
        exc_info->val = Py_None;
        Py_INCREF(exc_info->val);
    }
    /* Make the raw exception data
       available to the handler,
       so a program can emulate the
       Python main loop.  Don't do
       this for 'finally'. */
    if (block_type == SETUP_EXCEPT) {
        PyErr_NormalizeException(
            &exc_info->exc, &exc_info->val, &exc_info->tb);
        _PyEval_SetExcInfo(tstate,
                           exc_info->exc, exc_info->val, exc_info->tb);
    }
    if (exc_info->tb == NULL) {
        Py_INCREF(Py_None);
        exc_info->tb = Py_None;
    }
    /* Within the except or finally block,
       PyErr_Occurred() should be false.
       END_FINALLY will restore the
       exception if necessary. */
    PyErr_Clear();
}

}
