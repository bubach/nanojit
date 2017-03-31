#include <nanojitextra.h>

#include <string>
#include <map>
#include <iostream>

namespace nanojit {

	enum ReturnType {
		RT_INT = 1,
		RT_QUAD = 2,
		RT_DOUBLE = 4,
	};

	// We lump everything into a single access region for lirasm.
	static const AccSet ACCSET_OTHER = (1 << 0);
	static const uint8_t LIRASM_NUM_USED_ACCS = 1;

	typedef int32_t(FASTCALL *RetInt)();
	typedef int64_t(FASTCALL *RetQuad)();
	typedef double (FASTCALL *RetDouble)();

	struct Function {
		const char *name;
		struct nanojit::CallInfo callInfo;
	};

	class LirasmFragment {
	public:
		union {
			RetInt rint;
			RetQuad rquad;
			RetDouble rdouble;
		};
		ReturnType mReturnType;
		Fragment *fragptr;
	};

	typedef std::map<std::string, LirasmFragment> Fragments;

	// Equivalent to Lirasm
	class NanoJitContextImpl {
	public:

		bool verbose_;
		/**
		* A struct used to configure the assumptions that Assembler can make when
		* generating code. The ctor will fill in all fields with the most reasonable
		* values it can derive from compiler flags and/or runtime detection, but
		* the embedder is free to override any or all of them as it sees fit.
		* Using the ctor-provided default setup is guaranteed to provide a safe
		* runtime environment (though perhaps suboptimal in some cases), so an embedder
		* should replace these values with great care.
		*
		* Note that although many fields are used on only specific architecture(s),
		* this struct is deliberately declared without ifdef's for them, so (say) ARM-specific
		* fields are declared everywhere. This reduces build dependencies (so that this
		* files does not require nanojit.h to be included beforehand) and also reduces
		* clutter in this file; the extra storage space required is trivial since most
		* fields are single bits.
		*/
		Config config_;

		/**
		* Allocator is a bump-pointer allocator with an SPI for getting more
		* memory from embedder-implemented allocator, such as malloc()/free().
		*
		* alloc() never returns NULL.  The implementation of allocChunk()
		* is expected to perform a longjmp or exception when an allocation can't
		* proceed.
		*/
		Allocator alloc_;

		/**
		* Code memory allocator is a long lived manager for many code blocks that
		* manages interaction with an underlying code memory allocator,
		* sets page permissions.  CodeAlloc provides APIs for allocating and freeing
		* individual blocks of code memory (for methods, stubs, or compiled
		* traces), static functions for managing lists of allocated code, and has
		* a few pure virtual methods that embedders must implement to provide
		* memory to the allocator.
		*
		* A "chunk" is a region of memory obtained from allocCodeChunk; it must
		* be page aligned and be a multiple of the system page size.
		*
		* A "block" is a region of memory within a chunk.  It can be arbitrarily
		* sized and aligned, but is always contained within a single chunk.
		* class CodeList represents one block; the members of CodeList track the
		* extent of the block and support creating lists of blocks.
		*
		* The allocator coalesces free blocks when it can, in free(), but never
		* coalesces chunks.
		*/
		CodeAlloc code_alloc_;

		/**
		* All compiled fragments are saved in a map by fragment name
		*/
		Fragments fragments_;

		Assembler asm_;

		/**
		* LirBuffer object to hold LIR instructions
		*/
		LirBuffer *lirbuf_;

		// LogControl, a class for controlling and routing debug output
		LogControl logc_;

	public:
		NanoJitContextImpl(bool verbose, Config config);
		~NanoJitContextImpl();

	};

	NanoJitContext::NanoJitContext(bool verbose) {
		impl_ = new NanoJitContextImpl(verbose, Config());
	}

	NanoJitContext::~NanoJitContext() {
		delete impl_;
	}

	/**
	* Assembles a fragment - the fragment is saved in the parent Jit object by name.
	* A fragment can be thought of as a function, at least that is how we use it
	* for now.
	*/
	class FunctionBuilderImpl {
	private:

		NanoJitContextImpl &parent_;

		const std::string fragName_;

		/**
		* Once the instructions are in the LirBuffer, the application calls
		* parent_.asm_.compile() to produce machine code, which is stored in
		* the fragment. The result of compilation is a function that the
		* application can call from C via a pointer to the first instruction.
		*/
		Fragment *fragment_;

		bool optimize_;

		LirWriter *lir_;

		/**
		* LirBufWriter object to write instructions to the buffer
		*/
		LirBufWriter *bufWriter_;

		/*
		* The LirBufWriter is wrapped in zero or more other LirWriter objects,
		* all of which implement the same interface as LirBufWriter. This chain of
		* LirWriter objects forms a pipeline for the instructions to pass through.
		* Each LirWriter can perform an optimization or other task on the program
		* as it passes through the system and into the LirBuffer.
		*/

		LirWriter *cseFilter_;

		LirWriter *exprFilter_;

		LirWriter *verboseWriter_;

		LirWriter *validateWriter1_;

		LirWriter *validateWriter2_;

		char returnTypeBits_;

		int32_t paramCount_;

	private:
		static uint32_t sProfId;

	public:
		FunctionBuilderImpl(NanoJitContextImpl &parent, const std::string &fragmentName, bool optimize);
		~FunctionBuilderImpl();

		/**
		* Adds an integer return instruction.
		*/
		LIns *reti(LIns *result);

		/**
		* Adds a double return instruction.
		*/
		LIns *retd(LIns *result);

		/**
		* Adds a quad return instruction.
		*/
		LIns *retq(LIns *result);

		/**
		* Creates an int32 constant
		*/
		LIns *immi(int32_t i) { return lir_->insImmI(i); }

		/**
		* Creates an int64 constant
		*/
		LIns *immq(int64_t q) { return lir_->insImmQ(q); }

		/**
		* Creates a double constant
		*/
		LIns *immd(double d) { return lir_->insImmD(d); }

		/**
		* Adds a function parameter - the parameter size is always the
		* default register size I think - so on a 64-bit machine it will be
		* quads whereas on 32-bit machines it will be words. Caller must
		* handle this and convert to type needed.
		* This also means that only primitive values and pointers can be
		* used as function parameters.
		*/
		LIns *insertParameter() {
			return lir_->insParam(paramCount_++, 0);
		}

		/**
		* Integer add
		*/
		LIns *addi(LIns *lhs, LIns *rhs) {
			return lir_->ins2(LIR_addi, lhs, rhs);
		}

		LIns *q2i(LIns *q) {
#ifdef NANOJIT_64BIT
			return lir_->ins1(LIR_q2i, q);
#else
			return q;
#endif
		}

		/**
		* Completes the fragment, adds a guard record and if all ok, assembles the code.
		* If assembly is successful then the generated code is saved in the parent Jit object
		* by fragment name.
		*/
		void *finalize();

		SideExit* createSideExit();
		GuardRecord* createGuardRecord(SideExit *exit);

	private:
		// Prohibit copying.
		FunctionBuilderImpl(const FunctionBuilderImpl &) = delete;
		FunctionBuilderImpl & operator=(const FunctionBuilderImpl &) = delete;
	};

	uint32_t
		FunctionBuilderImpl::sProfId = 0;

	FunctionBuilder::FunctionBuilder(NanoJitContext& context, const std::string& name, bool optimize)
	{
		impl_ = new FunctionBuilderImpl(context.impl(), name, optimize);
	}

	FunctionBuilder::~FunctionBuilder() {
		delete impl_;
	}

	/**
	* Adds an integer return instruction.
	*/
	LIns *FunctionBuilder::reti(LIns *result) { return impl_->reti(result); }

	/**
	* Adds a double return instruction.
	*/
	LIns *FunctionBuilder::retd(LIns *result) { return impl_->retd(result); }

	/**
	* Adds a quad return instruction.
	*/
	LIns *FunctionBuilder::retq(LIns *result) { return impl_->retq(result); }

	/**
	* Creates an int32 constant
	*/
	LIns *FunctionBuilder::immi(int32_t i) { return impl_->immi(i); }

	/**
	* Creates an int64 constant
	*/
	LIns *FunctionBuilder::immq(int64_t q) { return impl_->immq(q); }

	/**
	* Creates a double constant
	*/
	LIns *FunctionBuilder::immd(double d) { return impl_->immd(d); }

	/**
	* Adds a function parameter - the parameter size is always the
	* default register size I think - so on a 64-bit machine it will be
	* quads whereas on 32-bit machines it will be words. Caller must
	* handle this and convert to type needed.
	* This also means that only primitive values and pointers can be
	* used as function parameters.
	*/
	LIns *FunctionBuilder::insertParameter() { return impl_->insertParameter(); }

	/**
	* Integer add
	*/
	LIns *FunctionBuilder::addi(LIns *lhs, LIns *rhs) { return impl_->addi(lhs, rhs); }

	/**
	* Converts a quad to an int
	*/
	LIns *FunctionBuilder::q2i(LIns *q) { return impl_->q2i(q); }

	/**
	* Completes the function, and assembles the code.
	* If assembly is successful then the generated code is saved in the parent Context object
	* by fragment name. The pointer to executable function is returned. Note that the pointer is
	* valid only until the NanoJitContext is valid, as all functions are destroyed when the
	* Context ends.
	*/
	void *FunctionBuilder::finalize() { return impl_->finalize(); }


	NanoJitContextImpl::NanoJitContextImpl(bool verbose, Config config) :
		verbose_(verbose),
		config_(config),
		code_alloc_(&config),
		asm_(code_alloc_, alloc_, alloc_, &logc_, config_)
	{
		verbose_ = verbose;
		logc_.lcbits = 0;

		lirbuf_ = new (alloc_) LirBuffer(alloc_);
#ifdef DEBUG
		if (verbose) {
			logc_.lcbits = LC_ReadLIR | LC_AfterDCE | LC_Native | LC_RegAlloc | LC_Activation | LC_Bytes;
			lirbuf_->printer = new (alloc_) LInsPrinter(alloc_, LIRASM_NUM_USED_ACCS);
		}
#endif

	}

	NanoJitContextImpl::~NanoJitContextImpl() {
		Fragments::iterator i;
		for (i = fragments_.begin(); i != fragments_.end(); ++i) {
			delete i->second.fragptr;
		}
	}

	FunctionBuilderImpl::FunctionBuilderImpl(NanoJitContextImpl &parent, const std::string &fragmentName, bool optimize)
		: parent_(parent), fragName_(fragmentName), optimize_(optimize),
		bufWriter_(nullptr), cseFilter_(nullptr), exprFilter_(nullptr), verboseWriter_(nullptr),
		validateWriter1_(nullptr), validateWriter2_(nullptr), paramCount_(0)
	{
		fragment_ = new Fragment(nullptr verbose_only(, (parent_.logc_.lcbits &
			nanojit::LC_FragProfile) ?
			sProfId++ : 0));
		fragment_->lirbuf = parent_.lirbuf_;
		parent_.fragments_[fragName_].fragptr = fragment_;

		lir_ = bufWriter_ = new LirBufWriter(parent_.lirbuf_, parent_.config_);
#ifdef DEBUG
		if (optimize) {     // don't re-validate if no optimization has taken place
			lir_ = validateWriter2_ =
				new ValidateWriter(lir_, fragment_->lirbuf->printer, "end of writer pipeline");
		}
#endif
#ifdef DEBUG
		if (parent_.verbose_) {
			lir_ = verboseWriter_ = new VerboseWriter(parent_.alloc_, lir_,
				parent_.lirbuf_->printer,
				&parent_.logc_);
		}
#endif
		if (optimize) {
			lir_ = cseFilter_ = new CseFilter(lir_, LIRASM_NUM_USED_ACCS, parent_.alloc_, parent_.config_);
		}
		if (optimize) {
			lir_ = exprFilter_ = new ExprFilter(lir_);
		}
#ifdef DEBUG
		lir_ = validateWriter1_ =
			new ValidateWriter(lir_, fragment_->lirbuf->printer, "start of writer pipeline");
#endif
		returnTypeBits_ = 0;
		lir_->ins0(LIR_start);
		for (int i = 0; i < nanojit::NumSavedRegs; ++i)
			lir_->insParam(i, 1);
	}

	FunctionBuilderImpl::~FunctionBuilderImpl()
	{
		delete validateWriter1_;
		delete validateWriter2_;
		delete verboseWriter_;
		delete exprFilter_;
		delete cseFilter_;
		delete bufWriter_;
	}

	LIns *
		FunctionBuilderImpl::reti(LIns *result)
	{
		returnTypeBits_ |= ReturnType::RT_INT;
		return lir_->ins1(LIR_reti, result);
	}

	LIns *
		FunctionBuilderImpl::retd(LIns *result)
	{
		returnTypeBits_ |= ReturnType::RT_DOUBLE;
		return lir_->ins1(LIR_retd, result);
	}

	LIns *
		FunctionBuilderImpl::retq(LIns *result)
	{
		returnTypeBits_ |= ReturnType::RT_QUAD;
		return lir_->ins1(LIR_retq, result);
	}


	SideExit*
		FunctionBuilderImpl::createSideExit()
	{
		SideExit* exit = new (parent_.alloc_) SideExit();
		memset(exit, 0, sizeof(SideExit));
		exit->from = fragment_;
		exit->target = nullptr;
		return exit;
	}

	GuardRecord*
		FunctionBuilderImpl::createGuardRecord(SideExit *exit)
	{
		GuardRecord *rec = new (parent_.alloc_) GuardRecord;
		memset(rec, 0, sizeof(GuardRecord));
		rec->exit = exit;
		exit->addGuard(rec);
		return rec;
	}

	void *
		FunctionBuilderImpl::finalize()
	{
		if (returnTypeBits_ == 0) {
			std::cerr << "warning: no return type in fragment '"
				<< fragName_ << "'" << std::endl;

		}
		else if (returnTypeBits_ != RT_INT &&
			returnTypeBits_ != RT_QUAD &&
			returnTypeBits_ != RT_DOUBLE)
		{
			std::cerr << "warning: multiple return types in fragment '"
				<< fragName_ << "'" << std::endl;
			return nullptr;
		}

		fragment_->lastIns =
			lir_->insGuard(LIR_x, NULL, createGuardRecord(createSideExit()));

		parent_.asm_.compile(fragment_, parent_.alloc_, optimize_
			verbose_only(, parent_.lirbuf_->printer));

		if (parent_.asm_.error() != nanojit::None) {
			std::cerr << "error during assembly: ";
			switch (parent_.asm_.error()) {
			case nanojit::BranchTooFar: std::cerr << "BranchTooFar"; break;
			case nanojit::StackFull: std::cerr << "StackFull"; break;
			case nanojit::UnknownBranch:  std::cerr << "UnknownBranch"; break;
			case nanojit::None: std::cerr << "None"; break;
			default: NanoAssert(0); break;
			}
			std::cerr << std::endl;
			std::exit(1);
		}

		LirasmFragment *f;
		f = &parent_.fragments_[fragName_];

		switch (returnTypeBits_) {
		case RT_INT:
			f->rint = (RetInt)((uintptr_t)fragment_->code());
			f->mReturnType = RT_INT;
			return f->rint;
		case RT_QUAD:
			f->rquad = (RetQuad)((uintptr_t)fragment_->code());
			f->mReturnType = RT_QUAD;
			return f->rquad;
			break;
		case RT_DOUBLE:
			f->rdouble = (RetDouble)((uintptr_t)fragment_->code());
			f->mReturnType = RT_DOUBLE;
			return f->rdouble;
			break;
		default:
			NanoAssert(0);
			std::cerr << "invalid return type\n";
			break;
		}
		return nullptr;
	}

}