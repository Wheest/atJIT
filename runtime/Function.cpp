#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Compat.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/LLVMHolderImpl.h>
#include <easy/runtime/Utils.h>
#include <easy/exceptions.h>

#include <tuner/optimizer.h>
#include <tuner/Feedback.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Support/Host.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/IR/LegacyPassManager.h>

#include <loguru.hpp>


using namespace easy;

namespace easy {
  DefineEasyException(ExecutionEngineCreateError, "Failed to create execution engine for:");
  DefineEasyException(CouldNotOpenFile, "Failed to file to dump intermediate representation.");
}

Function::Function(void* Addr, std::unique_ptr<LLVMHolder> H)
  : Address(Addr), Holder(std::move(H)) {
}

static std::unique_ptr<llvm::ExecutionEngine> GetEngine(std::unique_ptr<llvm::Module> M, const char *Name, llvm::CodeGenOpt::Level CGLevel, bool UseFastISel, bool UseIPRA) {
  llvm::EngineBuilder ebuilder(std::move(M));
  std::string eeError;

  llvm::TargetOptions TO;
  TO.EnableFastISel = UseFastISel;
  TO.EnableIPRA = UseIPRA;

  std::unique_ptr<llvm::ExecutionEngine> EE(ebuilder.setErrorStr(&eeError)
          .setMCPU(llvm::sys::getHostCPUName())
          .setEngineKind(llvm::EngineKind::JIT)
          .setOptLevel(CGLevel)
          .setTargetOptions(TO)
          .create());

  if(!EE) {
    throw easy::ExecutionEngineCreateError(Name);
  }

  return EE;
}

static void MapGlobals(llvm::ExecutionEngine& EE, GlobalMapping* Globals) {
  for(GlobalMapping *GM = Globals; GM->Name; ++GM) {
    EE.addGlobalMapping(GM->Name, (uint64_t)GM->Address);
  }
}

void Function::WriteOptimizedToFile(llvm::Module const &M, std::string const& File, bool Append) {
  if(File.empty())
    return;
  std::error_code Error;
  auto Mode = Append ? llvm::sys::fs::F_Append : llvm::sys::fs::F_None;
  llvm::raw_fd_ostream Out(File, Error, Mode);

  if(Error)
    throw CouldNotOpenFile(Error.message());

  DLOG_S(INFO) << "dumping to file...";

  Out << M;

  DLOG_S(INFO) << "done";
}

std::unique_ptr<Function>
Function::CompileAndWrap(const char*Name, GlobalMapping* Globals,
               std::unique_ptr<llvm::LLVMContext> LLVMCxt,
               std::unique_ptr<llvm::Module> M,
               llvm::CodeGenOpt::Level CGLevel,
               bool UseFastISel,
               bool UseIPRA) {

  llvm::Module* MPtr = M.get();
  std::unique_ptr<llvm::ExecutionEngine> EE = GetEngine(std::move(M), Name, CGLevel, UseFastISel, UseIPRA);

  if(Globals) {
    MapGlobals(*EE, Globals);
  }

  void *Address = (void*)EE->getFunctionAddress(Name);

  assert(Address != 0);

  std::unique_ptr<LLVMHolder> Holder(new easy::LLVMHolderImpl{std::move(EE), std::move(LLVMCxt), MPtr});
  return std::unique_ptr<Function>(new Function(Address, std::move(Holder)));
}

llvm::Module const& Function::getLLVMModule() const {
  return *static_cast<LLVMHolderImpl const&>(*this->Holder).M_;
}

void easy::Function::serialize(std::ostream& os) const {
  std::string buf;
  llvm::raw_string_ostream stream(buf);

  LLVMHolderImpl const *H = reinterpret_cast<LLVMHolderImpl const*>(Holder.get());
  llvm::WriteBitcodeToFile(PASS_MODULE_ARG(*(H->M_)), stream);
  stream.flush();

  os << buf;
}

std::unique_ptr<easy::Function> easy::Function::deserialize(std::istream& is) {

  auto &BT = BitcodeTracker::GetTracker();

  std::string buf(std::istreambuf_iterator<char>(is), {}); // read the entire istream
  auto MemBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(buf));

  std::unique_ptr<llvm::LLVMContext> Ctx(new llvm::LLVMContext());
  auto ModuleOrError = llvm::parseBitcodeFile(*MemBuf, *Ctx);
  if(ModuleOrError.takeError()) {
    return nullptr;
  }

  auto M = std::move(ModuleOrError.get());

  std::string FunName = easy::GetEntryFunctionName(*M);

  GlobalMapping* Globals = nullptr;
  if(void* OrigFunPtr = BT.getAddress(FunName)) {
    std::tie(std::ignore, Globals) = BT.getNameAndGlobalMapping(OrigFunPtr);
  }

  return
    CompileAndWrap(FunName.c_str(), Globals,
            std::move(Ctx), std::move(M), llvm::CodeGenOpt::Level::Aggressive,
            /*UseFastISel=*/ false,
           /*UseIPRA=*/ false);
}

bool Function::operator==(easy::Function const& other) const {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*this->Holder);
  LLVMHolderImpl& Other = static_cast<LLVMHolderImpl&>(*other.Holder);
  return This.M_ == Other.M_;
}

std::hash<easy::Function>::result_type
std::hash<easy::Function>::operator()(argument_type const& F) const noexcept {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*F.Holder);
  return std::hash<llvm::Module*>{}(This.M_);
}
