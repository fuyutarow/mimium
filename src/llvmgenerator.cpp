#include "llvmgenerator.hpp"

namespace mimium {
LLVMGenerator::LLVMGenerator(std::string filename, bool i_isjit)
    : isjit(i_isjit),
      taskfn_typeid(0),
      tasktype_list(),
      jitengine(std::make_unique<llvm::orc::MimiumJIT>()),
      ctx(jitengine->getContext()),
      mainentry(nullptr),
      currentblock(nullptr) {
  init(filename);
}
void LLVMGenerator::init(std::string filename) {
  builder = std::make_unique<llvm::IRBuilder<>>(ctx);
  module = std::make_unique<llvm::Module>(filename, ctx);
  builtinfn = std::make_shared<LLVMBuiltin>();
  module->setDataLayout(jitengine->getDataLayout());
}
void LLVMGenerator::reset(std::string filename) {
  dropAllReferences();
  init(filename);
}

void LLVMGenerator::initJit() {}

// LLVMGenerator::LLVMGenerator(llvm::LLVMContext& _ctx,std::string _filename){
//     // ctx.reset();
//     // ctx = std::move(&_ctx);
// }

LLVMGenerator::~LLVMGenerator() { dropAllReferences(); }
void LLVMGenerator::dropAllReferences() {
  namemap.clear();
  auto& flist = module->getFunctionList();
  auto f = flist.begin();
  while (!flist.empty()) {
    auto& bbs = f->getBasicBlockList();
    while (!bbs.empty()) {
      auto bb = bbs.begin();
      auto inst = bb->begin();
      while (!bb->getInstList().empty()) {
        inst->replaceAllUsesWith(llvm::UndefValue::get(inst->getType()));
        // inst->dropAllReferences();
        inst = inst->eraseFromParent();
      }
      bb->dropAllReferences();
      bb = bb->eraseFromParent();
      bb = bbs.begin();
    }
    f->dropAllReferences();
    flist.erase(f);
    f = flist.begin();
  }
}

auto LLVMGenerator::getRawStructType(const types::Value& type) -> llvm::Type* {
  types::Struct s = std::get<recursive_wrapper<types::Struct>>(type);
  std::vector<llvm::Type*> field;
  for (auto& a : s.arg_types) {
    field.push_back(getType(a));
  }

  llvm::Type* structtype = llvm::StructType::create(ctx, field, "fvtype");
  return structtype;
}
auto LLVMGenerator::getType(const types::Value& type) -> llvm::Type* {
  return std::visit(
      overloaded{[this](const types::Float& /*f*/) {
                   return llvm::Type::getDoubleTy(ctx);
                 },
                 [this](const recursive_wrapper<types::Function>& rf) {
                   auto f = types::Function(rf);
                   std::vector<llvm::Type*> args;
                   auto* rettype = getType(f.getReturnType());
                   for (auto& a : f.getArgTypes()) {
                     auto* atype = getType(a);
                     args.push_back(atype);
                   }
                   return llvm::cast<llvm::Type>(
                       llvm::FunctionType::get(rettype, args, false));
                 },
                 [this](const recursive_wrapper<types::Struct>& rs) {
                   auto s = types::Struct(rs);
                   std::vector<llvm::Type*> field;
                   for (auto& a : s.arg_types) {
                     field.push_back(llvm::PointerType::get(getType(a), 0));
                   }
                   llvm::Type* structtype = llvm::PointerType::get(
                       llvm::StructType::create(ctx, field, "fvtype"), 0);
                   return structtype;
                 },
                 [this](const types::Void& /* t */) {
                   return llvm::Type::getVoidTy(ctx);
                 },
                 [this](const recursive_wrapper<types::Time>& time) {
                   types::Time t = time;
                   return getOrCreateTimeStruct(t);
                 },
                 [this](auto& t) {  // NOLINT
                   throw std::logic_error("invalid type");
                   return llvm::Type::getVoidTy(ctx);
                   ;
                 }},
      type);
}

void LLVMGenerator::setBB(llvm::BasicBlock* newblock) {
  builder->SetInsertPoint(newblock);
}
void LLVMGenerator::createMiscDeclarations() {
  // create malloc
  auto* malloctype = llvm::FunctionType::get(builder->getInt8PtrTy(),
                                             {builder->getInt64Ty()}, false);
  auto res = module->getOrInsertFunction("malloc", malloctype).getCallee();
  namemap.emplace("malloc", res);
}
void LLVMGenerator::createMainFun() {
  auto* fntype = llvm::FunctionType::get(llvm::Type::getInt64Ty(ctx), false);
  auto* mainfun = llvm::Function::Create(
      fntype, llvm::Function::ExternalLinkage, "__mimium_main", module.get());
  mainfun->setCallingConv(llvm::CallingConv::C);
  using Akind = llvm::Attribute;
  std::vector<llvm::Attribute::AttrKind> attrs = {
      Akind::NoUnwind, Akind::NoInline, Akind::OptimizeNone};
  llvm::AttributeSet aset;
  for (auto& a : attrs) {
    aset = aset.addAttribute(ctx, a);
  }

  mainfun->addAttributes(llvm::AttributeList::FunctionIndex, aset);
  mainentry = llvm::BasicBlock::Create(ctx, "entry", mainfun);
}
void LLVMGenerator::createTaskRegister() {
  llvm::ArrayRef<llvm::Type*> argtypes = {
      builder->getDoubleTy(),   // time
      builder->getInt8PtrTy(),  // address to function
      // builder->getInt64Ty(), // tasktypeid
      builder->getDoubleTy(),  // argument(single)
      llvm::Type::getDoublePtrTy(
          ctx)  // address to target variable for assignment
  };
  auto* fntype = llvm::FunctionType::get(builder->getVoidTy(), argtypes, false);
  addtask = module->getOrInsertFunction("addTask", fntype);
  auto addtaskfun = llvm::cast<llvm::Function>(addtask.getCallee());
  addtaskfun->setCallingConv(llvm::CallingConv::C);
  using Akind = llvm::Attribute;
  std::vector<llvm::Attribute::AttrKind> attrs = {
      Akind::NoUnwind, Akind::NoInline, Akind::OptimizeNone};
  llvm::AttributeSet aset;
  for (auto& a : attrs) {
    aset = aset.addAttribute(ctx, a);
  }
  addtaskfun->addAttributes(llvm::AttributeList::FunctionIndex, aset);
  typemap.emplace("addTask", fntype);
}

llvm::Type* LLVMGenerator::getOrCreateTimeStruct(types::Time& t) {
  llvm::StringRef name = t.toString();
  llvm::Type* res = module->getTypeByName(name);
  if (res == nullptr) {
    llvm::Type* containtype = std::visit(
        overloaded{[&](types::Float& /*f*/) { return builder->getDoubleTy(); },
                   [&](auto& /*v*/) { return builder->getVoidTy(); }},
        t.val);

    res = llvm::StructType::create(ctx, {builder->getDoubleTy(), containtype},
                                   name);
  }
  return res;
}
void LLVMGenerator::preprocess() {
  createMiscDeclarations();
  createTaskRegister();
  createMainFun();
  setBB(mainentry);
}

void LLVMGenerator::generateCode(std::shared_ptr<MIRblock> mir) {
  preprocess();
  for (auto& inst : mir->instructions) {
    visitInstructions(inst, true);
  }
  if (mainentry->getTerminator() ==
      nullptr) {  // insert empty return if no return
    builder->CreateRet(llvm::ConstantInt::get(builder->getInt64Ty(), 0));
  }
}

// Creates Allocation instruction or call malloc function depends on context
llvm::Value* LLVMGenerator::createAllocation(bool isglobal, llvm::Type* type,
                                             llvm::Value* ArraySize = nullptr,
                                             const llvm::Twine& name = "") {
  llvm::Value* res = nullptr;
  if (isglobal) {
    auto size = module->getDataLayout().getTypeAllocSize(type);
    auto sizeinst = llvm::ConstantInt::get(ctx, llvm::APInt(64, size, false));
    auto rawres = builder->CreateCall(module->getFunction("malloc"), {sizeinst},
                                      "ptr_" + name + "_raw");
    res = builder->CreatePointerCast(rawres, llvm::PointerType::get(type, 0),
                                     "ptr_" + name);
  } else {
    res = builder->CreateAlloca(type, ArraySize, "ptr_" + name);
  }
  return res;
};
// Create StoreInst if storing to already allocated value
bool LLVMGenerator::createStoreOw(std::string varname,
                                  llvm::Value* val_to_store) {
  bool res = false;
  auto ptrname = "ptr_" + varname;
  auto it = namemap.find(varname);
  auto it2 = namemap.find(ptrname);
  if (it != namemap.cend() && it2 != namemap.cend()) {
    builder->CreateStore(val_to_store, namemap[ptrname]);
    res = true;
  }
  return res;
}

void LLVMGenerator::visitInstructions(const Instructions& inst, bool isglobal) {
  std::visit(
      overloaded{
          [](auto i) {},
          [&, this](const std::shared_ptr<AllocaInst>& i) {
            auto ptrname = "ptr_" + i->lv_name;
            auto* type = getType(i->type);
            auto* ptr = createAllocation(isglobal, type, nullptr, i->lv_name);
            typemap.emplace(ptrname, type);
            namemap.emplace(ptrname, ptr);
          },
          [&, this](const std::shared_ptr<NumberInst>& i) {
            auto* finst =
                llvm::ConstantFP::get(this->ctx, llvm::APFloat(i->val));
            namemap.emplace(i->lv_name, finst);
          },
          [&, this](const std::shared_ptr<TimeInst>& i) {
            auto ptrname = "ptr_" + i->lv_name;
            auto* type = typemap[ptrname];
            if (type == nullptr) {
              type = getType(i->type);
              typemap.try_emplace(ptrname, type);
            }
            auto* ptr = namemap[ptrname];
            if (ptr == nullptr) {
              ptr = createAllocation(isglobal, type, nullptr, i->lv_name);
              namemap[ptrname]=ptr;
            }
            auto* timepos = builder->CreateStructGEP(type, ptr, 0);
            auto* valpos = builder->CreateStructGEP(type, ptr, 1);

            auto* time =
                builder->CreateFPToUI(namemap[i->time], builder->getDoubleTy());

            builder->CreateStore(time, timepos);
            builder->CreateStore(namemap[i->val], valpos);
          },
          [&, this](const std::shared_ptr<RefInst>& i) {  // TODO
            auto ptrname = "ptr_" + i->lv_name;
            auto ptrptrname = "ptr_" + ptrname;
            auto* ptrtoptr = createAllocation(
                isglobal, llvm::PointerType::get(typemap[i->val], 0), nullptr,
                ptrname);
            builder->CreateStore(namemap[ptrname], ptrtoptr);
            auto* ptr = builder->CreateLoad(ptrtoptr, ptrptrname);
            namemap.emplace(ptrname, ptr);
            namemap.emplace(ptrptrname, ptrtoptr);
          },
          [&, this](const std::shared_ptr<AssignInst>& i) {
            if (std::holds_alternative<types::Float>(i->type)) {
              // copy assignment
              builder->CreateStore(namemap[i->val],
                                   namemap["ptr_" + i->lv_name]);
              // rename old register name
              namemap.extract(i->lv_name).key() += "_o";
              auto* newval =
                  builder->CreateLoad(namemap["ptr_" + i->lv_name], i->lv_name);
              namemap.emplace(i->lv_name, newval);
            }
          },
          [&, this](const std::shared_ptr<OpInst>& i) {
            llvm::Value* retvalue;
            auto* lhs = namemap[i->lhs];
            auto* rhs = namemap[i->rhs];
            switch (i->getOPid()) {
              case ADD:
                retvalue = builder->CreateFAdd(lhs, rhs, i->lv_name);
                break;
              case SUB:
                retvalue = builder->CreateFSub(lhs, rhs, i->lv_name);
                break;
              case MUL:
                retvalue = builder->CreateFMul(lhs, rhs, i->lv_name);
                break;
              case DIV:
                retvalue = builder->CreateFDiv(lhs, rhs, i->lv_name);
                break;
              default:
                retvalue = builder->CreateUnreachable();
                break;
            }
            namemap.emplace(i->lv_name, retvalue);
          },
          [&, this](const std::shared_ptr<FunInst>& i) {
            bool hasfv = !i->freevariables.empty();
            auto* ft =
                llvm::cast<llvm::FunctionType>(getType(i->type));  // NOLINT
            llvm::Function* f = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, i->lv_name, module.get());
            auto f_it = f->args().begin();

            std::for_each(i->args.begin(), i->args.end(),
                          [&](std::string s) { (f_it++)->setName(s); });
            if (hasfv) {
              auto it = f->args().end();
              (--it)->setName("clsarg_" + i->lv_name);
            }
            namemap.emplace(i->lv_name, f);

            auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
            builder->SetInsertPoint(bb);
            currentblock = bb;
            f_it = f->args().begin();
            std::for_each(i->args.begin(), i->args.end(),
                          [&](std::string s) { namemap.emplace(s, f_it++); });

            auto arg_end = f->arg_end();
            llvm::Value* lastarg = --arg_end;
            for (int id = 0; id < i->freevariables.size(); id++) {
              std::string newname = "fv_" + i->freevariables[id].name;
              llvm::Value* gep = builder->CreateStructGEP(lastarg, id, "fv");
              auto ptrname = "ptr_" + newname;
              llvm::Value* ptrload = builder->CreateLoad(gep, ptrname);
              llvm::Value* valload = builder->CreateLoad(ptrload, newname);
              namemap.try_emplace(ptrname, ptrload);
              namemap.try_emplace(newname, valload);
            }
            for (auto& cinsts : i->body->instructions) {
              visitInstructions(cinsts, false);
            }
            if (currentblock->getTerminator() == nullptr &&
                ft->getReturnType()->isVoidTy()) {
              builder->CreateRetVoid();
            }
            setBB(mainentry);
            currentblock = mainentry;
          },
          [&, this](const std::shared_ptr<MakeClosureInst>& i) {
            auto it = llvm::cast<llvm::Function>(namemap[i->fname])  // NOLINT
                          ->arg_end();
            auto ptrtype =
                llvm::cast<llvm::PointerType>((--it)->getType());  // NOLINT
            llvm::Type* structtype = ptrtype->getElementType();
            llvm::Value* cap_size =
                createAllocation(isglobal, structtype, nullptr, i->lv_name);
            int idx = 0;
            for (auto& cap : i->captures) {
              llvm::Value* gep =
                  builder->CreateStructGEP(structtype, cap_size, idx++, "");
              builder->CreateStore(namemap["ptr_" + cap.name], gep);
            }
            namemap.emplace(i->lv_name, cap_size);
          },
          [&, this](const std::shared_ptr<FcallInst>& i) {
            llvm::Value* res;
            std::vector<llvm::Value*> args;
            std::for_each(i->args.begin(), i->args.end(),
                          [&](auto& a) { args.emplace_back(namemap[a]); });
            if (i->ftype == CLOSURE) {
              args.emplace_back(namemap[i->fname + "_cls"]);
            }
            if (i->istimed) {  // if arguments is timed value, call addTask
                               // function,

              auto tvptr = namemap["ptr_" + i->args[0]];
              auto timeptr = builder->CreateStructGEP(
                  typemap["ptr_" + i->args[0]], tvptr, 0, "");
              auto valptr = builder->CreateStructGEP(
                  typemap["ptr_" + i->args[0]], tvptr, 1, "");
              auto timeval = builder->CreateLoad(timeptr);
              auto val = builder->CreateLoad(valptr);
              auto targetfn = llvm::cast<llvm::Function>(namemap[i->fname]);
              auto ptrtofn = llvm::ConstantExpr::getBitCast(
                  targetfn, builder->getInt8PtrTy());
              auto taskid = taskfn_typeid++;
              tasktype_list.emplace_back(i->type);
              auto taskfntype = llvm::FunctionType::get(
                  builder->getDoubleTy(), builder->getDoubleTy(), false);
              auto lvptrname = "ptr_" + i->lv_name;
              auto lvptr = createAllocation(
                  isglobal, taskfntype->getReturnType(), nullptr, i->lv_name);
              namemap.emplace(lvptrname, lvptr);

              // time,address to fun, arg(double), ptrtotarget,
              llvm::ArrayRef<llvm::Value*> args = {timeval, ptrtofn, val,
                                                   lvptr};

              auto* res = builder->CreateCall(addtask, args);
              namemap.emplace(i->lv_name, res);
            } else {
              if (LLVMBuiltin::isBuiltin(i->fname)) {
                auto it = LLVMBuiltin::builtin_fntable.find(i->fname);
                builtintype fn = it->second;
                res = fn(args, i->lv_name, this->shared_from_this());
              } else {
                llvm::Function* fun = module->getFunction(i->fname);
                if (fun == nullptr) {
                  throw std::logic_error("function could not be referenced");
                }
                res = builder->CreateCall(fun, args, i->lv_name);
              }
              namemap.emplace(i->lv_name, res);
            }
          },
          [&, this](const std::shared_ptr<ReturnInst>& i) {
            builder->CreateRet(namemap[i->val]);
          }},
      inst);
}

llvm::Error LLVMGenerator::doJit(const size_t opt_level) {
  return jitengine->addModule(
      std::move(module));  // do JIT compilation for module
}
int LLVMGenerator::execute() {
  llvm::Error err = doJit();
  Logger::debug_log(err, Logger::ERROR);
  auto mainfun = jitengine->lookup("__mimium_main");
  Logger::debug_log(mainfun, Logger::ERROR);
  auto fnptr =
      llvm::jitTargetAddressToPointer<int64_t (*)()>(mainfun->getAddress());

  int64_t res = fnptr();
  return res;
}
void LLVMGenerator::outputToStream(llvm::raw_ostream& stream) {
  module->print(stream, nullptr, false, true);
}

}  // namespace mimium