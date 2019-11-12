#pragma once
#include "ast.hpp"

namespace mimium {
enum FCALLTYPE { DIRECT, CLOSURE, EXTERNAL };
static std::map<FCALLTYPE, std::string> fcalltype_str={
    {DIRECT , ""}, {CLOSURE , "cls"}, {EXTERNAL , "ext"}};
static std::string join(std::deque<std::string> vec, std::string delim);
class MIRinstruction {  // base class for MIR instruction
 protected:
  virtual ~MIRinstruction(){};
  bool isFreeVariable(std::deque<std::string>& args, std::string str);
  void gatherFV_raw(std::deque<std::string>& fvlist, std::deque<std::string>& args,std::string str);
 public:
   std::string lv_name;
  virtual std::string toString() = 0;
  virtual void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args)=0;
};
class NumberInst;
class SymbolInst;
class TimeInst;
class OpInst;
class FunInst;
class FcallInst;
class MakeClosureInst;
class ArrayInst;
class ArrayAccessInst;
class IfInst;
class ReturnInst;

using Instructions = std::variant<std::shared_ptr<NumberInst>,std::shared_ptr<SymbolInst>,std::shared_ptr<TimeInst>,std::shared_ptr<OpInst>,std::shared_ptr<FunInst>,std::shared_ptr<FcallInst>,std::shared_ptr<MakeClosureInst>,std::shared_ptr<ArrayInst>,std::shared_ptr<ArrayAccessInst>,std::shared_ptr<IfInst>,std::shared_ptr<ReturnInst>>;

class MIRblock {
 public:
  MIRblock(std::string _label) : label(_label), prev(nullptr), next(nullptr){
    indent_level = 0;
  };
  ~MIRblock(){};
  void addInst(Instructions& inst) {
    instructions.push_back(inst);
  }
  void changeIndent(int level){
    indent_level += level;
  }
  std::string label;
  std::deque<Instructions>
      instructions;  // sequence of instructions
  std::shared_ptr<MIRblock> prev;
  std::shared_ptr<MIRblock> next;
  std::string toString();
  int indent_level;//shared between instances
};



class NumberInst : public MIRinstruction {
  double val;

 public:
  NumberInst(std::string _lv, double _val)
      : val(std::move(_val)) {
        lv_name = _lv;
      }
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;
};
class SymbolInst : public MIRinstruction {
  std::string val;

 public:
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class TimeInst: public MIRinstruction{
  public:
  std::string time;
  std::string val;
  TimeInst(std::string _lv, std::string _val,std::string _time):time(_time),val(_val){
    lv_name=_lv;
  }
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class OpInst : public MIRinstruction {
  std::string op;
  std::string lhs;
  std::string rhs;

 public:
  OpInst(std::string _lv, std::string _op, std::string _lhs, std::string _rhs)
      : op(std::move(_op)),
        lhs(std::move(_lhs)),
        rhs(std::move(_rhs)){
    lv_name=_lv;
        };
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

  ~OpInst(){};
};
class FunInst : public MIRinstruction {
 public:
  std::deque<std::string> args;
  std::shared_ptr<MIRblock> body;
  std::deque<std::string> freevariables; //introduced in closure conversion;
  FunInst(std::string name,std::deque<std::string> newargs):args(std::move(newargs)) { body = std::make_shared<MIRblock>(name);
   lv_name =name; };
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class FcallInst : public MIRinstruction {
 public:
  std::string fname;
  std::deque<std::string> args;
  FCALLTYPE type;
  FcallInst(std::string _lv, std::string _fname, std::deque<std::string> _args,FCALLTYPE ftype = DIRECT)
      :fname(_fname), args(std::move(_args)){
        lv_name=_lv;
      };
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class MakeClosureInst : public MIRinstruction {
  std::string fname;
  std::deque<std::string> captures;

 public:
  std::string toString() override;
  MakeClosureInst(std::string _lv,std::string _fname,std::deque<std::string> _captures):fname(std::move(_fname)),captures(std::move(_captures)){
            lv_name=_lv;
  };
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class ArrayInst : public MIRinstruction {
  std::string name;
  std::deque<std::string> args;
 public:
   ArrayInst(std::string _lv,std::deque<std::string> _args):args(std::move(_args))
   {
     lv_name=_lv;
   }

  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class ArrayAccessInst : public MIRinstruction {
  std::string name;
  std::string index;

 public:
  ArrayAccessInst(std::string _lv,std::string _name,std::string _index):name(_name),index(_index){
    lv_name = _lv;
  }
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class IfInst : public MIRinstruction {
 public:
  std::string cond;
  std::shared_ptr<MIRblock> thenblock;
  std::shared_ptr<MIRblock> elseblock;
  IfInst(std::string name, std::string _cond):cond(_cond) {
    thenblock = std::make_shared<MIRblock>(name + "$then");
    elseblock = std::make_shared<MIRblock>(name + "$else");
    lv_name=name;
  }
  std::string toString() override;
  void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
class ReturnInst: public MIRinstruction{
    public:
    std::string val;
    ReturnInst(std::string name,std::string _val):val(_val){
      lv_name = name;
    }
    std::string toString() override;
    void gatherFreeVariable(std::deque<std::string>& fvlist, std::deque<std::string>& args) override;

};
}  // namespace mimium