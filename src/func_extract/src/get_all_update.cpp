#include "get_all_update.h"

#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"

#include <sys/stat.h>

#include "ins_context_stack.h"
#include "parse_fill.h"
#include "check_regs.h"
#include "global_data_struct.h"
#include "helper.h"
#include "util.h"
#include "branch_mux.h"

#define toStr(a) std::to_string(a)
#define toCout(a) std::cout << a << std::endl

namespace funcExtract {

using namespace taintGen;


FuncExtractFlow::FuncExtractFlow(UFGenFactory& genFactory, ModuleInfo& info,
                                 bool innerLoopIsInstrs, bool reverseCycleOrder)
  : m_genFactory(genFactory), m_info(info),
    m_innerLoopIsInstrs(innerLoopIsInstrs),
    m_reverseCycleOrder(reverseCycleOrder)
{
}




// A file should be generated, including:
// 1. all the asvs and their bit numbers
// 2. For each instruction, what ASVs they write, and 
// for each of it update function, what are the arguments
void FuncExtractFlow::get_all_update() {
  toCout("### Begin get_all_update ");
  std::ofstream genTimeFile(g_path+"/up_gen_time.txt");
  genTimeFile << "\n===== Begin a new run!" << std::endl;
  genTimeFile.close();

  std::ofstream simplifyTimeFile(g_path+"/simplify_time.txt");
  simplifyTimeFile <<"\n===== Begin a new run!"  << std::endl;
  simplifyTimeFile.close();

  std::vector<std::string> outputs;
  m_info.get_module_outputs(outputs);
  for (auto out : outputs) {
    if(m_info.is_fifo_output(out)) continue;
    m_workSet.mtxInsert(out);
    uint32_t width = m_info.get_var_width_simp(out);
    m_asvSet.emplace(out, {width});  // Default value (empty set) for cycles.
  }


  std::ifstream addedWorkSetInFile(g_path+"/added_work_set.txt");
  std::string line;  
  while(std::getline(addedWorkSetInFile, line)) {
    m_workSet.mtxInsert(line);
  }
  addedWorkSetInFile.close();

  // insert regs in fifos
  std::vector<std::string> fifos;
  m_info.get_fifo_insts(fifos);
  for(auto insName: fifos) {
    std::string modName = m_info.get_module_of_inst(insName);

    // The data in g_fifo comes from instr.txt, not from the design.
    assert(g_fifo.find(modName) != g_fifo.end());
    uint32_t bound = g_fifo[modName];
    for(uint32_t i = 0; i < bound; i++) {
      std::string reg = insName+".r"+toStr(i);
      m_workSet.mtxInsert(reg);
      uint32_t width = m_info.get_var_width_simp(reg);
      m_asvSet.emplace(reg, {width});
    }
  }

  if(!g_allowedTgt.empty()) {
    m_workSet.mtxClear();    
    for(auto tgtDelayPair : g_allowedTgt) {
      std::string tgt = tgtDelayPair.first;    
      m_workSet.mtxInsert(tgt);
      uint32_t width = m_info.get_var_width_cmplx(tgt);
      m_asvSet.emplace(tgt, {width});
    }
  }
  else if(!g_allowedTgtVec.empty()) {
    m_workSet.mtxClear();
  }

  // declaration for llvm
  std::ofstream funcInfo(g_path+"/func_info.txt");
  std::ofstream asvInfo(g_path+"/asv_info.txt");
  //std::vector<std::string> m_fileNameVec;
  struct ThreadSafeVector_t m_fileNameVec;
  std::vector<std::thread> threadVec;

  if(!g_use_multi_thread && m_innerLoopIsInstrs) {
    // schedule 1: outer loop is workSet/target, inner loop is instructions,
    // suitable for design with many instructions
    std::map<std::string, TgtVec_t> allowedTgtVec = g_allowedTgtVec;    // Deep copy...
    while(!m_workSet.empty() || !allowedTgtVec.empty() ) {
      bool isVec;
      std::string target;
      std::vector<std::string> tgtVec;
      // work on single register target first
      if(!m_workSet.empty()) {
        isVec = false;
        auto targetIt = m_workSet.begin();
        target = *targetIt;
        m_workSet.mtxErase(targetIt);
        if(m_visitedTgt.mtxExist(target)
           || g_skippedOutput.find(target) != g_skippedOutput.end())
          continue;
      }
      else if(!allowedTgtVec.empty()){
        isVec = true;
        target = allowedTgtVec.begin()->first;  // Name of vector (previously auto-generated)
        tgtVec = allowedTgtVec.begin()->second.members;  // Deep copy
        allowedTgtVec.erase(allowedTgtVec.begin());
      }

      uint32_t instrIdx = 0;
      for(auto instrInfo : g_instrInfo) {
        instrIdx++;
        std::vector<uint32_t> delayBounds = get_delay_bounds(target, instrInfo);
        for (auto delayBound : delayBounds) {
          // If allowed_target.txt specifies multiple delays for a non-vector target,
          // generate update functions for each one.
          get_update_function(target, delayBound, isVec,
                            instrInfo, instrIdx);
        }
      }
      if(isVec) {
        for(auto reg: tgtVec) {
          m_visitedTgt.mtxInsert(reg);
        }
        tgtVec.clear();
      }
      else {
        m_visitedTgt.mtxInsert(target);
      }
    } // end of while loop
  }
  else {
    // schedule 2: outer loop is instructions, inner loop is workSet/target
    bool doneFirstRound = false;
    while(!m_workSet.empty() || (!doneFirstRound && !g_allowedTgtVec.empty())) {
      uint32_t instrIdx = 0;
      StrSet_t localWorkSet;
      StrSet_t oldWorkSet;
      m_workSet.copy(oldWorkSet);
      m_workSet.mtxClear();
      for(auto instrInfo : g_instrInfo) {
        localWorkSet = oldWorkSet;
        std::map<std::string, TgtVec_t> localWorkVec = g_allowedTgtVec;  // Deep copy...
        instrIdx++;
        threadVec.clear();
        while(!localWorkSet.empty() || !localWorkVec.empty()) {
          bool isVec;
          std::string target;

          if(!localWorkSet.empty()) {
            isVec = false;
            auto targetIt = localWorkSet.begin();
            target = *targetIt;
            localWorkSet.erase(targetIt);
            if(m_visitedTgt.mtxExist(target)
               || g_skippedOutput.find(target) != g_skippedOutput.end())
              continue;
          }
          else if(!localWorkVec.empty()){
            isVec = true;
            target = localWorkVec.begin()->first;
            localWorkVec.erase(localWorkVec.begin());
          }

          std::vector<uint32_t> delayBounds = get_delay_bounds(target, instrInfo);
          for (auto delayBound : delayBounds) {
            // If allowed_target.txt specifies multiple delays for a non-vector target,
            // generate update functions for each one.  Note the correct way
            // to use a pointer to a member function in this situation.
            if (g_use_multi_thread) {
              std::thread th(&FuncExtractFlow::get_update_function, this, target,
                              delayBound, isVec, instrInfo, instrIdx);
              threadVec.push_back(std::move(th));
            } else {
              // No thread created - threadVec remains empty.
              get_update_function(target, delayBound, isVec,
                                instrInfo, instrIdx);
            }
          }
        }
        // wait for update functions of all targets of this instruction to finish
        for(auto &th: threadVec) th.join();

      } // end of for-lopp: for each instruction

      for(auto pair: g_allowedTgtVec) {
        for(std::string reg: pair.second.members) {
          m_visitedTgt.mtxInsert(reg);
        }
      }
      for(std::string target: oldWorkSet) {
        m_visitedTgt.mtxInsert(target);
      }
      // targetVectors only executed for one round
      doneFirstRound = true;
    } // end of while loop
  }

  print_llvm_script(g_path+"/link.sh");
  print_func_info(funcInfo);
  print_asv_info(asvInfo);
}


void
FuncExtractFlow::get_update_function(std::string target,
                                         uint32_t delayBound,
                                         bool isVec,
                                         InstrInfo_t instrInfo,
                                         uint32_t instrIdx) {

  std::shared_ptr<UFGenerator> UFGen = m_genFactory.makeGenerator();

  time_t startTime = time(NULL);

  // set the destInfo according to the target
  DestInfo destInfo;
  destInfo.isMemVec = false;
  destInfo.isSingleMem = false;
  if(!isVec) {
    toCout("---  BEGIN Target: "+target+" ---");
    if(target.find("puregs[2]") != std::string::npos)
      toCoutVerb("Find it!");

    if(target.find(".") == std::string::npos 
       || target.substr(0, 1) == "\\") {
      uint32_t width = m_info.get_var_width_simp(target);
      destInfo.set_dest_and_slice(target, width);
    }
    else {
      auto pair = split_module_asv(target);
      std::string prefix = pair.first;
      std::string var = pair.second;
      if(m_info.is_module(prefix)) {
        uint32_t width = m_info.get_var_width_simp(var, prefix);
        destInfo.set_module_name(prefix);
        destInfo.set_dest_and_slice(var, width);
      }
      else {
        std::string modName = m_info.get_module_of_inst(prefix);
        assert(!modName.empty());
        destInfo.set_module_name(modName);
        destInfo.set_instance_name(prefix);
        uint32_t width = m_info.get_var_width_simp(var, modName);
        destInfo.set_dest_and_slice(var, width);
      }
    }
    destInfo.isVector = false;
    if(isMem(target))
      destInfo.isSingleMem = true;
  }
  else {
    // work on the vector of target registers defined by target
    destInfo.isVector = true;

    std::vector<std::string> vecWorkSet;
    for(auto pair : g_allowedTgtVec) {
      if (pair.first == target) {
        vecWorkSet = pair.second.members;  // Deep copy
      }
    }

    assert(vecWorkSet.size() > 0);  // A vector of size 1 is acceptable...

    toCout("---  BEGIN Vector Target: "+target+" ---");

    destInfo.set_dest_vec(vecWorkSet);
    std::string firstASV = vecWorkSet.front();
    if(isMem(firstASV)) destInfo.isMemVec = true;
    if(firstASV.find(".") != std::string::npos 
       && firstASV.substr(0, 1) != "\\") {
      auto pair = split_module_asv(firstASV);
      std::string prefix = pair.first;
      std::string var = pair.second;
      if(m_info.is_module(prefix)) {
        destInfo.set_module_name(prefix);
        uint32_t width = m_info.get_var_width_simp(var, prefix);
        destInfo.set_dest_and_slice(var, width);
      }
      else {
        std::string modName = m_info.get_module_of_inst(prefix);
        assert(!modName.empty());
        destInfo.set_module_name(modName);
        destInfo.set_instance_name(prefix);
        uint32_t width = m_info.get_var_width_simp(var, modName);
        destInfo.set_dest_and_slice(var, width);
      }
    }
    else destInfo.set_module_name(g_topModule);
  }


  std::string instrName = instrInfo.name;
  m_dependVarMapMtx.lock();
  if(m_dependVarMap.find(instrName) == m_dependVarMap.end())
    m_dependVarMap.emplace( instrName, std::map<std::string, ArgVec_t>());
  m_dependVarMapMtx.unlock();
  g_currInstrInfo = instrInfo;
  destInfo.set_instr_name(instrInfo.name);      
  assert(!instrInfo.name.empty());

  std::string destSimpleName = funcExtract::var_name_convert(destInfo.get_dest_name(), true);

  std::string funcName = destInfo.get_func_name();

  std::string fileName = UFGen->make_llvm_basename(destInfo, delayBound);
  std::string cleanOptoFileName = fileName+".clean-o3-ll";
  std::string rewriteFileName = fileName + ".rewrite-ll";
  std::string reoptFileName = fileName + ".reopt-ll";
  std::string llvmFileName = fileName+".ll";

  toCout("---  BEGIN INSTRUCTION #"+toStr(instrIdx)+": "+instrInfo.name+
         "  ASV: "+destSimpleName+"  delay bound: "+toStr(delayBound)+" ---");

  // Optionally avoid the time-consuming re-generation of an existing LLVM function.
  // This lets you incrementally add data to instr.txt and rerun this program to
  // generate just the missing update functions. 

  struct stat statbuf;
  if ((!g_overwrite_existing_llvm) && stat(cleanOptoFileName.c_str(), &statbuf) == 0) {
    toCout("Skipping re-generation of existing file "+cleanOptoFileName);
  } else {

    // generate update function
    UFGen->print_llvm_ir(destInfo, delayBound, instrIdx, fileName+".tmp-ll");

    time_t upGenEndTime = time(NULL);  

    // Default g_llvm_path is blank, which means the shell will use $PATH in the usual way.
    std::string optCmd = (g_llvm_path.length() ? g_llvm_path+"/" : "") + "opt";

    std::string clean(optCmd+" --instsimplify --deadargelim --instsimplify "+fileName+".tmp-ll -S -o="+fileName+".clean-ll");
    
    //std::string opto_cmd(optCmd+" -O1 "+fileName+".clean-ll -S -o="+fileName+".tmp-o3-ll; opt -passes=deadargelim "+fileName+".tmp-o3-ll -S -o="+cleanOptoFileName+"; rm "+fileName+".tmp-o3-ll");

    std::string opto_cmd(optCmd+" -O3 "+fileName+".clean-ll -S -o="+fileName+".tmp-o3-ll; opt -passes=deadargelim "+fileName+".tmp-o3-ll -S -o="+cleanOptoFileName+"; rm "+fileName+".tmp-o3-ll");

    // re-optimization commands
    std::string rewrite_cmd(optCmd + " -passes=rtl2ila " + cleanOptoFileName + " -S -o=" + rewriteFileName + ";");
    std::string reopt_cmd(optCmd + " -O3 " + rewriteFileName + " -S -o=" + reoptFileName + ";");

    toCout("** Begin clean update function");
    toCoutVerb(clean);
    system(clean.c_str());
    toCout("** Begin simplify update function");
    toCoutVerb(opto_cmd);
    system(opto_cmd.c_str());
    toCout("** End simplify update function");

    // perform re-optimization
    if (g_do_bitwise_opt)
    {
      toCout("** Performing bitwise optimization on LLVM IR file.");
      system(rewrite_cmd.c_str());
      toCout("** Re-optimizing the update function.");
      system(reopt_cmd.c_str());
      toCout("** Re-optimization ended.");
    }

    time_t simplifyEndTime = time(NULL);
    uint32_t upGenTime = upGenEndTime - startTime;
    uint32_t simplifyTime = simplifyEndTime - upGenEndTime;
    m_TimeFileMtx.lock();
    std::ofstream genTimeFile(g_path+"/up_gen_time.txt");
    genTimeFile << funcName+":\t"+toStr(upGenTime) << std::endl;
    genTimeFile.close();
    std::ofstream simplifyTimeFile(g_path+"/simplify_time.txt");
    simplifyTimeFile << funcName+":\t"+toStr(simplifyTime) << std::endl;
    simplifyTimeFile.close();
    m_TimeFileMtx.unlock();
  }

  ArgVec_t argVec;
  bool usefulFunc = false;


  // Load in the optimized LLVM file
  llvm::SMDiagnostic Err;
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> M;
  if (!g_do_bitwise_opt)
    M = llvm::parseIRFile(cleanOptoFileName, Err, Context);
  else
    M = llvm::parseIRFile(reoptFileName, Err, Context);
  
  if (!M) {
    Err.print("func_extract", llvm::errs());
  } else {
    usefulFunc = clean_main_func(*M, funcName);
    if (usefulFunc) {

      if (g_post_opto_mux_to_branch) {
        toCout("Converting muxes to branches...");
        BranchMux::convertSelectsToBranches(M.get(), g_post_opto_mux_to_branch_threshold);
      }
      
      // Add a C-compatible wrapper function that calls the main function.
      std::string wrapperFuncName = create_wrapper_func(*M, funcName);

      // Annotate the standard x86-64 Clang data layout to the module,
      // to prevent warnings when linking to C/C++ code.
      M->setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128");

      // Get the data needed to create func_info.txt
      gather_wrapper_func_args(*M, wrapperFuncName, target, delayBound, argVec);

      if ((!g_overwrite_existing_llvm) && stat(llvmFileName.c_str(), &statbuf) == 0) {
        toCout("Skipping re-generation of existing file "+llvmFileName);
      } else {
        // Write out the modified IR data to a new file.
        std::error_code EC;
        llvm::raw_fd_ostream OS(fileName+".clean-simp-ll", EC);
        OS << *M;
        OS.close();

        // Rename that file to be the final .ll file
        std::string move("mv " + fileName + ".clean-simp-ll " + llvmFileName);
        toCoutVerb(move);
        system(move.c_str());
      }
    }
  }

  if(usefulFunc) {
    m_fileNameVec.push_back(llvmFileName);        
    toCout("----- For instr "+instrInfo.name+", "+target+" is affected!");
    m_dependVarMapMtx.lock();
    if(m_dependVarMap[instrName].find(target) == m_dependVarMap[instrName].end())
      m_dependVarMap[instrName].emplace(target, argVec);
    else {
      toCout("Warning: for instruction "+instrInfo.name+", target: "+target+" is seen before");
      //abort();
    }
    m_dependVarMapMtx.unlock();
  }
  else {
    toCout("----- For instr "+instrInfo.name+", "+target+" is NOT affected!");
  }

  for(auto arg : argVec) {
    std::string reg = arg.name;
    int cycle = arg.cycle;
    if(g_push_new_target && !m_visitedTgt.mtxExist(reg)) {
      m_workSet.mtxInsert(reg);
    }

    uint32_t width = std::abs(arg.width);  // For pointers, we want the pointee width.

    // Add any discovered registers that had not already been identified as ASVs
    // or register arrays.
    // Ignore any special non-ASV/non-register function args, indicated by a 
    // reserved name (those go in func_info.txt).
    // And skip args already known to be in a register array.
    if (!is_special_arg_name(reg) && g_allowedTgt.count(reg) == 0 &&
        g_allowedTgtVec.count(reg) == 0 &&
        get_vector_of_target(reg, nullptr).empty()) {
      // If we have a specific clock cycle, we may need to update an existing entry in m_asvSet
      if(!m_asvSet.contains(reg)) {
        m_asvSet.emplace(reg, {width});
      } 
      WidthCycles_t& data = m_asvSet.at(reg);  // Possibly fetch what we just inserted.
      assert(data.width == width);  // Check for inconsistent bitwidth
      if (cycle > 0) {
        data.cycles.insert(cycle);  // Cycles will be empty if no specific cycle is used.
      }
    }
  }
}




// This returns a list of one or more delays to use for update function generation
// of the given scalar or vector ASV.
// Note priority of data sources for delays:
//
// 1: Multiple per-ASV delays from allowed_target.txt (not available for vector ASVs)
// 2: A per-instruction delay exception from instr.txt (not available for vector ASVs)
// 3: A single per-ASV delay from allowed_target.txt
// 4: A per-instruction delay from instr.txt
//
//  If no delays can be found, the program will fail.  Note that 0 is a legal delay value.

std::vector<uint32_t>
FuncExtractFlow::get_delay_bounds(std::string var, const InstrInfo_t &instrInfo) {

  assert(!var.empty());

  // Highest priority is multiple delays from allowed_target.txt
  if(g_allowedTgt.count(var) && g_allowedTgt[var].size() > 1) {
    return g_allowedTgt[var];  // Copies vector
  }

  // See if the given name represents a target vector. If so, use its delay
  if(g_allowedTgtVec.count(var)) {
    const TgtVec_t& vec = g_allowedTgtVec[var];
    uint32_t delay = (vec.delay > 0) ? vec.delay : instrInfo.delayBound;
    return std::vector<uint32_t>{delay};  // Return a vector of one delay value
  }

  // Default delay is per-instruction from instr.txt
  uint32_t delayBound = instrInfo.delayBound;

  // not array target
  auto pos = instrInfo.delayExceptions.find(var);
  if (pos != instrInfo.delayExceptions.end()) {
    // per-instruction delay exception from instr.txt
    delayBound = pos->second;
  }
  else if(g_allowedTgt.count(var) && !g_allowedTgt[var].empty()) {
    // Data from allowed_target.txt
    uint32_t dly = g_allowedTgt[var].front();
    delayBound = dly;
    if(g_allowedTgt[var].size() > 1) {
      toCout("Error: Target "+var+" has multiple delays. Only the first ("+toStr(dly)+") will be used.");
    }
  }

  return std::vector<uint32_t>{delayBound};  // Return a vector of one delay value
}



// Check if this type is too big to be passed in a register.
// Pointers and void are not considered big.
// Except in special cases, all parameters to update functions are integer types.
static bool
isBigType(const llvm::Type *type) {

  // Special case for LLVM Vector types.  Calculate total size.
  if (type->isVectorTy()) {
    const llvm::VectorType *vecTy = llvm::dyn_cast<const llvm::VectorType>(type);
    unsigned totalWidth = vecTy->getElementType()->getIntegerBitWidth() *
                          vecTy->getElementCount().getFixedValue();
    return totalWidth > 64;
  }
  
  // Normal scalar, or void.
  return (type->isIntegerTy() && type->getIntegerBitWidth() > 64);
}



// Make the wrapper function for C/C++ interfacing.
// Return its name
std::string
FuncExtractFlow::create_wrapper_func(llvm::Module& M,
                                         std::string mainFuncName) {

  llvm::Function *mainFunc = M.getFunction(mainFuncName);
  assert(mainFunc);

  llvm::LLVMContext& Context = mainFunc->getContext();

  // First build a FunctionType for the wrapper function: it has pointers for
  // every arg bigger than 64 bits.  If the return value is bigger than 64 bits,
  // one more pointer arg is added for it, and the wrapper function returns void.

  std::vector<llvm::Type *> wrapperArgTy;

  for (const llvm::Argument& arg : mainFunc->args()) {
    llvm::Type *type = arg.getType();
    if (isBigType(type)) {
      wrapperArgTy.push_back(llvm::PointerType::getUnqual(type));
    } else {
      // This handles small args, as well as pointers to register arrays.
      wrapperArgTy.push_back(type);
    }
  }

  llvm::Type* mainRetTy = mainFunc->getReturnType();

  // Deal with small vs large return values
  llvm::Type* wrapperRetTy = nullptr;

  if (isBigType(mainRetTy)) {
    // Add one more pointer argument for return value.
    wrapperArgTy.push_back(llvm::PointerType::getUnqual(mainRetTy));
    wrapperRetTy = llvm::Type::getVoidTy(Context);
  } else {
    wrapperRetTy = mainRetTy;
  }

  llvm::FunctionType *wrapperFT =
    llvm::FunctionType::get(wrapperRetTy, wrapperArgTy, false);

  llvm::Function *wrapperFunc =
    llvm::Function::Create(wrapperFT, llvm::Function::ExternalLinkage, 
                             mainFunc->getName()+"_wrapper", mainFunc->getParent());

  // This is what the LLVM optimization would do if it ran on the wrapper function.
  // But we need to be sure that it is consistent with the top-level C/C++ compiler.
  wrapperFunc->setCallingConv(mainFunc->getCallingConv());

  // Probably unnecessary, since all optimization has been done.
  wrapperFunc->addFnAttr(llvm::Attribute::NoInline);

  // Set the names of the wrapper function args, based on the main function arg names
  for (const llvm::Argument& mainArg : mainFunc->args()) {
    unsigned argNo = mainArg.getArgNo();
    llvm::Argument* wrapperArg = wrapperFunc->getArg(argNo);
    wrapperArg->setName(mainArg.getName());

    if (isBigType(mainArg.getType())) {
      // For big args, the wrapper arg is a pointer.
      // Specify that it can never be null (actually in C++, it will be a const reference).
      wrapperArg->addAttr(llvm::Attribute::NonNull);
    }
    // Copy parameter attributes (important for pointer args).
    llvm::AttrBuilder b(Context, mainFunc->getAttributes().getParamAttrs(argNo));
    wrapperFunc->addParamAttrs(argNo, b);
    // Remove the "returned" attribute, since it may no longer be correct.
    wrapperFunc->removeParamAttr(argNo, llvm::Attribute::Returned);

  }

  llvm::Argument* wrapperLastArg = wrapperFunc->arg_end()-1;

  // If needed, add an extra arg that handles big return types
  if (isBigType(mainRetTy)) {
    wrapperLastArg->setName(llvm::Twine(RETURN_VAL_PTR_ID));

    // Specify that it can never be null (actually in C++, it will be a (non-const) reference).
    wrapperLastArg->addAttr(llvm::Attribute::NonNull);
  }


  // Fill in the contents of wrapperFunc
  std::shared_ptr<llvm::IRBuilder<>> Builder = std::make_unique<llvm::IRBuilder<>>(Context);
  auto wrapperBB = llvm::BasicBlock::Create(Context, "wrapper_bb", wrapperFunc);
  Builder->SetInsertPoint(wrapperBB);  

  // Pass the wrapper args down to the main function. For ones that
  // are provided via pointers, dereference the pointers.
  std::vector<llvm::Value*> callArgs;
  for (llvm::Argument& mainArg : mainFunc->args()) {
    llvm::Type *mainArgType = mainArg.getType();
    llvm::Argument* wrapperArg = wrapperFunc->getArg(mainArg.getArgNo());

    llvm::Value *argVal = nullptr;
    if (isBigType(mainArgType)) {
      // Dereference the pointer
      argVal = Builder->CreateLoad(mainArgType, wrapperArg);
    } else {
      // Pass the arg by value
      argVal = wrapperArg;
    }
    callArgs.push_back(argVal);
  }
  
  // call mainFunc from wrapperFunc
  llvm::CallInst *call = Builder->CreateCall(mainFunc->getFunctionType(), mainFunc, callArgs);
  call->setCallingConv(mainFunc->getCallingConv());

  if (isBigType(mainRetTy)) {
    // Add store of return value to last pointer arg
    Builder->CreateStore(call, wrapperLastArg);
    Builder->CreateRetVoid();
  } else if (mainRetTy->isVoidTy()) {
    Builder->CreateRetVoid();
  } else {
    // Return the return value of the call to mainFunc
    Builder->CreateRet(call);
  }

  // Note that LLVM does its own output buffering, so we have to
  // make sure that all the output shows up in the correct order.
  llvm::outs() << "Verification of main function...\n";
  bool v2 = llvm::verifyFunction(*mainFunc, &llvm::outs());
  llvm::outs() << "Verification " << (v2 ? "failed!" : "passed.")  << "\n";
  llvm::outs() << "Verification of wrapper function...\n";
  bool v1 = llvm::verifyFunction(*wrapperFunc, &llvm::outs());
  llvm::outs() << "Verification " << (v1 ? "failed!" : "passed.")  << "\n";
  llvm::outs().flush();

  return wrapperFunc->getName().str();
}


llvm::Function *
FuncExtractFlow::remove_dead_args(llvm::Function *func) {
  // It is possible for there to be dead args in mainFunc, normally because topFunc
  // has been re-purposed as the mainFunc.  This is despite all the LLVM optimizations we do...
  // This function removes any unused args from the given function.
  //
  // This task is greatly simplified by the fact that the function is not being called, 
  // and it does not have any unusual or complex characteristics.
  // For a solution to the more general dead argument elimination problem, see:
  // llvm/lib/Transforms/IPO/DeadArgumentElimination.cpp

  // First build a new FunctionType that does not contain the dead args.

  std::vector<llvm::Type *> newArgTy;

  bool hasDeadArgs = false;

  for (const llvm::Argument& arg : func->args()) {
    if (arg.getNumUses() > 0) {
      llvm::Type *type = arg.getType();
      newArgTy.push_back(type);
    } else {
      hasDeadArgs = true; 
    }
  }

  if (!hasDeadArgs) {
    return func;  // No dead args, nothing to do
  }

  // We're commited to creating a new function to replace the original

  llvm::Type* retTy = func->getReturnType();
  llvm::FunctionType *newFT =
    llvm::FunctionType::get(retTy, newArgTy, false);

  // Create the new function body and insert it into the module...
  llvm::Function *newFunc =
    llvm::Function::Create(newFT, func->getLinkage(), func->getAddressSpace(),
                           "", func->getParent());
  newFunc->copyAttributesFrom(func);

  // Steal the orignal function's name
  newFunc->takeName(func);

  // Handled by copying of attributes?
  //newFunc->setCallingConv(func->getCallingConv());

  // Now move the contents of the original function into the new one.
  newFunc->getBasicBlockList().splice(newFunc->begin(), func->getBasicBlockList());


  // Remove all arg attributes created by above call to copyAttributesFrom(),
  // since that function may have created trash out-of-range parameter
  // attributes.
  llvm::AttributeList oldAttrs = newFunc->getAttributes();
  llvm::AttributeList newAttrs = llvm::AttributeList::get(
                           newFunc->getContext(),
                           oldAttrs.getFnAttrs(),
                           oldAttrs.getRetAttrs(),
                           llvm::ArrayRef<llvm::AttributeSet>());
  newFunc->setAttributes(newAttrs);

  // Set the names and attributes of the new function args, based on the original function args 
  int origArgNo = 0;
  int newArgNo = 0;
  for (llvm::Argument& origArg : func->args()) {
    if (origArg.getNumUses() > 0) {
      // Create corresponding arg in the new function.
      llvm::Argument* newArg = newFunc->getArg(newArgNo);
      // Make the usages of the original arg refer to the new one.
      origArg.replaceAllUsesWith(newArg);

      // Steal the original arg's name
      newArg->takeName(&origArg);

      // Copy arg attributes from the corresponding arg of the original func.
      llvm::AttrBuilder b(func->getContext(), func->getAttributes().getParamAttrs(origArgNo));
      newFunc->addParamAttrs(newArgNo, b);

      newArgNo++;
    }
    origArgNo++;
  }

  // Get rid of the stripped remains of the original function.
  func->eraseFromParent();

  return newFunc;

}




// Make sure the main function exists, and remove any unused args.
bool
FuncExtractFlow::clean_main_func(llvm::Module& M,
                                     std::string funcName) {

  llvm::Function *mainFunc = M.getFunction(funcName);

  if (!mainFunc) {
    toCout("Can't find main function!");
    return false;
  }

  // Newer algorithm in check_regs.cpp: don't create a top_function,
  // make mainFunc external, and depend on us to remove dead args.
  mainFunc = remove_dead_args(mainFunc);

  // See if any dead arg elimination actually worked.
  for (llvm::Argument& arg : mainFunc->args()) {
    if (arg.getNumUses() == 0) {
      std::string argname = arg.getName().str();
      toCout(funcName+" arg "+argname+" is unused!");
    }
  }

  return true;
}


// Push information about the wrapperFunc args to argVec, to be written out to func_info.txt

bool
FuncExtractFlow::gather_wrapper_func_args(llvm::Module& M,
                                              std::string wrapperFuncName,
                                              std::string target,
                                              int delayBound, ArgVec_t &argVec) {

  llvm::Function *wrapperFunc = M.getFunction(wrapperFuncName);
  assert(wrapperFunc);

  for (llvm::Argument& arg : wrapperFunc->args()) {
    std::string argname = arg.getName().str();

    llvm::Type *argType = arg.getType();
    bool isPointer = argType && argType->isPointerTy();

    // In LLVM version 15, all pointers are untyped, so it is not possible to
    // get the size of the object they point to.  This complicates things here.

    int size = 0;

    if (argname == RETURN_ARRAY_PTR_ID) {
      assert(isPointer);
      // Special arg name indicates a pointer to the register array that is the target of this function.
      if(g_allowedTgtVec.count(target)) {
        // Get the bitwidth of the first member of the register array (and negate it)
        std::string firstMember = g_allowedTgtVec[target].members[0];
        size = -m_info.get_var_width_cmplx(firstMember);
      } else {
        toCout("Function "+wrapperFuncName+" has arg "+argname+", but its target is not a vector!");
        size = 0;
        assert(false);
      }
      argVec.push_back({argname, size, 0});
    } else if (argname == RETURN_VAL_PTR_ID) {
      // Special arg name indicates a pointer a big scalar that is the target of this function
      assert(isPointer);
      if(m_asvSet.contains(target)) {
        size = -(m_asvSet.at(target).width);  // Instead get size from get_var_width_cmplx()?
      } else {
        toCout("Function "+wrapperFuncName+" has arg "+argname+", but its target is not a known ASV!");
        size = 0;
        assert(false);
      }
      argVec.push_back({argname, size, 0});
    } else {
      // Extract the ASV name from the argument name (by removing the cycle count).
      // Note that the name will not have quotes or backslashes, like you would see in the textual IR.
      // TODO: have the client provide a parsing function for the arg name.
      uint32_t pos = argname.find(DELIM, 0);
      std::string var = argname.substr(0, pos);

      int cycle = 0;
      if (pos != std::string::npos) {
        // Pick out the numeric portion of the arg name
        // Doug TODO: Do register array vars have a cycle number in their name?
        std::string cycleStr = argname.substr(pos + DELIM.size(), std::string::npos);
        if (cycleStr.size() > 0) {
          cycle = std::stoi(cycleStr);

          // If the internal cycle numbering starts at the cycle count and decreases
          // down to 0 at the final cycle, the cycle numbers must be mapped to the
          // convention used in instr.txt, where the cycle numbering starts at 0
          // and goes upwards as time passes.
          if (m_reverseCycleOrder) {
            cycle = delayBound - cycle;
          } else {
            cycle = cycle - 1;
          }
        }
      }

      if (!isPointer) {
        // A small thing (presumably a scalar ASV) passed by value
        size = arg.getType()->getPrimitiveSizeInBits();
      } else if (g_allowedTgtVec.count(var)) {
        // A pointer to a regster array
        // Get the bitwidth of the first member of the register array
        std::string firstMember = g_allowedTgtVec[var].members[0];
        size = -m_info.get_var_width_cmplx(firstMember);
      } else if (m_asvSet.contains(var)) {
        // A big scalar ASV passed by ref
        size = -(m_asvSet.at(var).width);  // Instead get size from get_var_width_cmplx()?
      } else if (!get_vector_of_target(var, nullptr).empty()) {
        // A reference to a big scalar ASV that is a member of a register array (and thus not in m_asvSet)
        size = m_info.get_var_width_cmplx(var);
        toCout("Function "+wrapperFuncName+" has arg "+var+
                  " of size "+toStr(size)+" which belongs to a register array");
      } else {
        // A big scalar, but not found in m_asvSet
        size = m_info.get_var_width_cmplx(var);
        toCout("Function "+wrapperFuncName+" has arg "+var+
                  " of size "+toStr(size)+" which is not a known ASV or register array!");
      }
      argVec.push_back({var, size, cycle});
    }
  }

  return true;
}


void
FuncExtractFlow::print_func_info(std::ofstream &output) {
  m_dependVarMapMtx.lock();
  for (const auto pair1 : m_dependVarMap) {
    output << "Instr:"+pair1.first << std::endl;
    for (const auto pair2 : pair1.second) {
      output << "Target:"+pair2.first << std::endl;
      for (const auto arg : pair2.second) {
        output << arg.name+":"+toStr(arg.width);
        if (arg.cycle != 0) {
          output << ":"+toStr(arg.cycle);
        }
        output << std::endl;
      }
      output << std::endl;
    }
    output << std::endl;
  }
  m_dependVarMapMtx.unlock();  
}


void
FuncExtractFlow::print_asv_info(std::ofstream &output) {
  for(auto it = m_asvSet.begin(); it != m_asvSet.end(); it++) {
    const std::string& reg = it->first;
    uint32_t width = it->second.width;

    assert(!is_special_arg_name(reg));

    if (!get_vector_of_target(reg, nullptr).empty()) {
      continue;  // Skip the ASVs in registers - handle below.
    }

    // Line format is: <name>:<width>[:<cycle1>,<cycle2>,...]
    output << reg << ":" << width; // Write name and width
    for (int cycle : it->second.cycles) {  // Write any clock cycles
      output << ":" << cycle;
    }
    output << std::endl;

  }

  // Write the contents of register arrays separately, including the array name
  for(auto pair: g_allowedTgtVec) {
    output << "[" << std::endl;
    for(std::string reg : pair.second.members) {
      uint32_t width = m_info.get_var_width_cmplx(reg);
      output << reg << ":" << width; // Write name and width
      output << std::endl;
    }
    output << "]:" << pair.first << std::endl;
  }
}


void
FuncExtractFlow::print_llvm_script( std::string fileName) {
  // Any command-line args (e.g. -O3) will be given to clang.
  std::ofstream output(fileName);
  output << "clang $* ila.cpp -emit-llvm -S -o main.ll" << std::endl;
  std::string line = "llvm-link -v main.ll \\";
  output << line << std::endl;
  for(auto it = m_fileNameVec.begin(); it != m_fileNameVec.end(); it++) {
    line = *it + " \\";
    output << line << std::endl;
  }
  line = "-S -o linked.ll";
  output << line << std::endl;
  output << "clang $* linked.ll" << std::endl;
  output.close();

  // Make the new file executable, to the extent that it is readable.
  struct stat statbuf;
  stat(fileName.c_str(), &statbuf);
  mode_t mode = statbuf.st_mode | S_IXUSR;
  if (mode | S_IRGRP) mode |= S_IXGRP;
  if (mode | S_IROTH) mode |= S_IXOTH;
  chmod(fileName.c_str(), mode);

}


void WorkSet_t::mtxInsert(std::string reg) {
  mtx.lock();
  workSet.insert(reg);
  mtx.unlock();
}


void WorkSet_t::mtxErase(std::set<std::string>::iterator it) {
  mtx.lock();
  workSet.erase(it);
  mtx.unlock();
}


void WorkSet_t::mtxAssign(std::set<std::string> &set) {
  mtx.lock();
  workSet = set;
  mtx.unlock();
}


void WorkSet_t::mtxClear() {
  mtx.lock();
  workSet.clear();
  mtx.unlock();
}


bool WorkSet_t::empty() {
  return workSet.empty();
}


std::set<std::string>::iterator WorkSet_t::begin() {
  return workSet.begin();
}


void WorkSet_t::copy(std::set<std::string> &copySet) {
  copySet.clear();
  copySet = workSet;
}


bool WorkSet_t::mtxExist(std::string reg) {
  mtx.lock();
  if(workSet.find(reg) != workSet.end()) {
    mtx.unlock();
    return true;
  }
  else {
    mtx.unlock();
    return false;
  }
}



void RunningThreadCnt_t::increase() {
  mtx.lock();
  cnt++;
  mtx.unlock();
}


void RunningThreadCnt_t::decrease() {
  mtx.lock();
  if(cnt == 0) {
    toCout("Error: thread count is already 0, cannot decrease!");
    abort();
  }
  cnt--;
  mtx.unlock();
}


uint32_t RunningThreadCnt_t::get() {
  uint32_t ret;
  mtx.lock();
  ret = cnt;
  mtx.unlock();
  return ret;
}


void ThreadSafeVector_t::push_back(std::string var) {
  mtx.lock();
  vec.push_back(var);
  mtx.unlock();
}


std::vector<std::string>::iterator ThreadSafeVector_t::begin() {
  return vec.begin();
}


std::vector<std::string>::iterator ThreadSafeVector_t::end() {
  return vec.end();
}


template <typename T>
void ThreadSafeMap_t<T>::emplace(const std::string& var, const T& data) {
  mtx.lock();
  mp.emplace(var, data);
  mtx.unlock();
}


template <typename T>
bool ThreadSafeMap_t<T>::contains(const std::string& var) {
  mtx.lock();
  bool ret = mp.count(var) > 0;
  mtx.unlock();
  return ret;
}


template <typename T>
T& ThreadSafeMap_t<T>::at(const std::string& var) {
  mtx.lock();
  auto pos = mp.find(var);
  assert(pos != mp.end());
  T& ret = pos->second;
  mtx.unlock();
  return ret;
}


template <typename T>
typename std::map<std::string, T>::iterator ThreadSafeMap_t<T>::begin() {
  return mp.begin();
}


template <typename T>
typename std::map<std::string, T>::iterator ThreadSafeMap_t<T>::end() {
  return mp.end();
}




} // end of namespace
