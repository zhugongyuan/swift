//===--- swift-api-digester.cpp - API change detector -------===//
////
//// This source file is part of the Swift.org open source project
////
//// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
//// Licensed under Apache License v2.0 with Runtime Library Exception
////
//// See http://swift.org/LICENSE.txt for license information
//// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
////
////===----------------------------------------------------------------------===//

// swift-api-digester is a test utility to detect source-breaking API changes
// during the evolution of a swift library. The tool works on two phases:
// (1) dumping library contents as a json file, and (2) comparing two json
// files textually to report interesting changes.
//
// During phase (1), the api-digester looks up every declarations inside
// a module and outputs a singly-rooted tree that encloses interesting
// details of the API level.
//
// During phase (2), api-digester applies structure-information comparision
// algorithms on two given singly root trees, trying to figure out, as
// precise as possible, the branches/leaves in the trees that differ from
// each other. Further analysis decides whether the changed leaves/branches
// can be reflected as source-breaking changes for API users. If they are,
// the output of api-digester will include such changes.

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include "swift/AST/Decl.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/ColorUtils.h"
#include "swift/Basic/JSONSerialization.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/Version.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/Utils.h"
#include <functional>

using namespace swift;
using namespace ide;

namespace  {
  enum class ActionType {
    None,
    DumpSDK,
    DumpSwiftModules,
    CompareSDKs,
    DiagnoseSDKs,
  };
}

namespace options {

static llvm::cl::opt<bool>
IncludeAllModules("include-all", llvm::cl::desc("Include all modules from the SDK"));

static llvm::cl::list<std::string>
ModuleNames("module", llvm::cl::ZeroOrMore, llvm::cl::desc("Names of modules"));

static llvm::cl::opt<std::string>
ModuleList("module-list-file",
           llvm::cl::desc("File containing new-line separated list of modules"));

static llvm::cl::opt<std::string>
OutputFile("o", llvm::cl::desc("Output file"));

static llvm::cl::opt<std::string>
SDK("sdk", llvm::cl::desc("path to the SDK to build against"));

static llvm::cl::opt<std::string>
Triple("target", llvm::cl::desc("target triple"));

static llvm::cl::opt<std::string>
ModuleCachePath("module-cache-path", llvm::cl::desc("Clang module cache path"));

static llvm::cl::opt<std::string>
ResourceDir("resource-dir",
            llvm::cl::desc("The directory that holds the compiler resource files"));

static llvm::cl::list<std::string>
FrameworkPaths("F", llvm::cl::desc("add a directory to the framework search path"));

static llvm::cl::opt<bool>
AbortOnModuleLoadFailure("abort-on-module-fail",
                        llvm::cl::desc("Abort if a module failed to load"));

static llvm::cl::opt<bool>
Verbose("v", llvm::cl::desc("Verbose"));

static llvm::cl::opt<ActionType>
Action(llvm::cl::desc("Mode:"), llvm::cl::init(ActionType::None),
      llvm::cl::values(
          clEnumValN(ActionType::DumpSDK,
                     "dump-sdk",
                     "Dump SDK content to json file"),
          clEnumValN(ActionType::DumpSwiftModules,
                     "dump-swift",
                     "dump swift modules in SDK"),
          clEnumValN(ActionType::CompareSDKs,
                     "compare-sdk",
                     "Compare SDK content in json file"),
          clEnumValN(ActionType::DiagnoseSDKs,
                     "diagnose-sdk",
                     "Diagnose SDK content in json file"),
          clEnumValEnd));

static llvm::cl::list<std::string>
SDKJsonPaths("input-paths",
            llvm::cl::desc("The SDK contents under conparison"));

static llvm::cl::list<std::string>
ApisPrintUsrs("api-usrs",
              llvm::cl::desc("The name of APIs to print their usrs, "
                             "e.g. Type::Function"));

static llvm::cl::opt<std::string>
IgnoreRemovedDeclUSRs("ignored-usrs",
                      llvm::cl::desc("the file containing USRs of removed decls "
                                     "that the digester should ignore"));

static llvm::cl::opt<std::string>
SwiftVersion("swift-version",
             llvm::cl::desc("The swift compiler version to invoke"));
}

namespace {

StringRef InsertToBuffer(StringRef S) {
  static llvm::StringSet<> Data;
  return Data.insert(S).first->getKey();
}

template<typename T>
bool contains(std::vector<T> container, T instance) {
  return std::find(container.begin(), container.end(), instance) != container.end();
}

template<typename T>
bool contains(ArrayRef<T> container, T instance) {
  return std::find(container.begin(), container.end(), instance) != container.end();
}

class SDKNode;
typedef SDKNode* NodePtr;
typedef std::unique_ptr<SDKNode> NodeUniquePtr;
typedef std::map<NodePtr, NodePtr> ParentMap;
typedef std::vector<NodeUniquePtr> NodeOwnedVector;
typedef std::vector<NodePtr> NodeVector;

// The interface used to visit the SDK tree.
class SDKNodeVisitor {
  friend SDKNode;
protected:
  NodeVector Ancestors;
  virtual void visit(NodePtr Node) = 0;

  NodePtr parent() {
    if (Ancestors.empty())
      return nullptr;
    return Ancestors.back();
  }

  int depth() {
    return Ancestors.size() + 1;
  }
public:
  virtual ~SDKNodeVisitor() = default;
};

// During the matching phase, any matched node will be reported using this API.
// For update Node left = {Node before change} Right = {Node after change};
// For added Node left = {NilNode} Right = {Node after change};
// For removed Node left = {Node before change} Right = {NilNode}
struct MatchedNodeListener {
  virtual void foundMatch(NodePtr Left, NodePtr Right) = 0;
  virtual void foundRemoveAddMatch(NodePtr Removed, NodePtr Added) {}
  virtual bool isMatched(NodePtr Left, NodePtr Right) {return false;};
  virtual ~MatchedNodeListener() = default;
};

// A node matcher will traverse two trees of SDKNode and find matched nodes
struct NodeMatcher {
  virtual void match() = 0;
  virtual ~NodeMatcher() = default;
};

#define KEY(NAME) static const char* Key_##NAME = #NAME;
#include "DigesterEnums.def"

// The node kind apparing in the tree that describes the content of the SDK
enum class SDKNodeKind {
#define NODE_KIND(NAME) NAME,
#include "DigesterEnums.def"
};

enum class NodeAnnotation {
#define NODE_ANNOTATION(NAME) NAME,
#include "DigesterEnums.def"
};

enum class KnownTypeKind {
#define KNOWN_TYPE(NAME) NAME,
#include "DigesterEnums.def"
  Unknown,
};

enum class SDKDeclAttrKind {
#define DECL_ATTR(Name) DAK_##Name,
#include "DigesterEnums.def"
};

// Redefine << so that we can output the name of the node kind.
static raw_ostream &operator<<(raw_ostream &Out, const SDKNodeKind Value) {
  switch (Value) {
#define NODE_KIND(Name) case SDKNodeKind::Name: return Out << #Name;
#include "DigesterEnums.def"
  }
  llvm_unreachable("Undefined SDK node kind.");
}

// Redefine << so that we can output the name of the annotation kind.
static raw_ostream &operator<<(raw_ostream &Out, const NodeAnnotation Value) {
#define NODE_ANNOTATION(X) if (Value == NodeAnnotation::X) { return Out << #X; }
#include "DigesterEnums.def"
  llvm_unreachable("Undefined SDK node kind.");
}
  // Redefine << so that we can output the name of decl kind.
static raw_ostream &operator<<(raw_ostream &Out, const DeclKind Value) {
  switch (Value) {
#define DECL(X, PARENT) case DeclKind::X: return Out << #X;
#include "swift/AST/DeclNodes.def"
  }
}

struct SDKNodeInitInfo {
  StringRef Name;
  StringRef PrintedName;
  DeclKind DKind;
  StringRef USR;
  StringRef Location;
  StringRef ModuleName;
  bool IsThrowing = false;
  Optional<uint8_t> SelfIndex;
  std::vector<SDKDeclAttrKind> DeclAttrs;
  std::vector<TypeAttrKind> TypeAttrs;
  SDKNodeInitInfo() = default;
  SDKNodeInitInfo(ValueDecl *VD);
  SDKNodeInitInfo(Type Ty);
  NodeUniquePtr createSDKNode(SDKNodeKind Kind);
};

class SDKNode {
  typedef std::vector<NodeUniquePtr>::const_iterator ChildIt;
  StringRef Name;
  StringRef PrintedName;
  unsigned TheKind : 4;
  NodeOwnedVector Children;
  std::set<NodeAnnotation> Annotations;
  std::map<NodeAnnotation, StringRef> AnnotateComments;
  NodePtr Parent = nullptr;

protected:
  SDKNode(SDKNodeInitInfo Info, SDKNodeKind Kind) : Name(Info.Name),
    PrintedName(Info.PrintedName), TheKind(unsigned(Kind)) {}

public:
  static NodeUniquePtr constructSDKNode(llvm::yaml::MappingNode *Node);
  static void preorderVisit(NodePtr Root, SDKNodeVisitor &Visitor);
  static void postorderVisit(NodePtr Root, SDKNodeVisitor &Visitor);

  bool operator==(const SDKNode &Other) const;
  bool operator!=(const SDKNode &Other) const { return !((*this) == Other); }

  bool isLeaf() const { return Children.empty(); }
  SDKNodeKind getKind() const { return SDKNodeKind(TheKind); }
  StringRef getName() const { return Name; }
  bool isNameValid() const { return Name != "_"; }
  StringRef getPrintedName() const { return PrintedName; }
  void removeChild(ChildIt CI) { Children.erase(CI); }
  ChildIt getChildBegin() const { return Children.begin(); }
  ChildIt getChildEnd() const { return Children.end(); }
  void annotate(NodeAnnotation Anno) { Annotations.insert(Anno); }
  bool isName(StringRef N) const { return getName() == N; }
  NodePtr getParent() const { return Parent; };
  unsigned getChildrenCount() const { return Children.size(); }
  NodePtr childAt(unsigned I) const;
  void removeChild(NodePtr C);
  void addAnnotateComment(NodeAnnotation Anno, StringRef Comment);
  StringRef getAnnotateComment(NodeAnnotation Anno) const;
  bool isAnnotatedAs(NodeAnnotation Anno) const;
  void addChild(NodeUniquePtr Child);
  ArrayRef<NodeUniquePtr> getChildren() const;
  void collectChildren(NodeVector &Bucket) const;
  unsigned getChildIndex(NodePtr Child) const;
  NodePtr getOnlyChild() const;
  template <typename T> const T *getAs() const;
  template <typename T> T *getAs();
};

class SDKNodeDecl : public SDKNode {
  DeclKind DKind;
  StringRef Usr;
  StringRef Location;
  StringRef ModuleName;
  std::vector<SDKDeclAttrKind> DeclAttributes;
  bool hasDeclAttribute(SDKDeclAttrKind DAKind) const;

protected:
  SDKNodeDecl(SDKNodeInitInfo Info, SDKNodeKind Kind) : SDKNode(Info, Kind),
                      DKind(Info.DKind), Usr(Info.USR), Location(Info.Location),
                  ModuleName(Info.ModuleName), DeclAttributes(Info.DeclAttrs) {}

public:
  StringRef getUsr() const { return Usr; }
  StringRef getLocation() const { return Location; }
  StringRef getModuleName() const {return ModuleName;}
  void addDeclAttribute(SDKDeclAttrKind DAKind);
  ArrayRef<SDKDeclAttrKind> getDeclAttributes() const;
  bool isObjc() const { return Usr.startswith("c:"); }
  static bool classof(const SDKNode *N);
  DeclKind getDeclKind() const { return DKind; }
  void printFullyQualifiedName(llvm::raw_ostream &OS);
  StringRef getFullyQualifiedName();
  bool isSDKPrivate();
  bool isDeprecated();
};

class SDKNodeType : public SDKNode {
  std::vector<TypeAttrKind> TypeAttributes;

protected:
  bool hasTypeAttribute(TypeAttrKind DAKind) const;
  SDKNodeType(SDKNodeInitInfo Info, SDKNodeKind Kind) : SDKNode(Info, Kind),
    TypeAttributes(Info.TypeAttrs) {}

public:
  KnownTypeKind getTypeKind() const;
  void addTypeAttribute(TypeAttrKind AttrKind);
  ArrayRef<TypeAttrKind> getTypeAttributes() const;
  SDKNodeDecl *getClosestParentDecl() const;
  static bool classof(const SDKNode *N);
  static bool areTypesSame(const SDKNodeType *Left, const SDKNodeType *Right);
};

bool SDKNodeType::classof(const SDKNode *N) {
  switch (N->getKind()) {
  case SDKNodeKind::TypeNominal:
  case SDKNodeKind::TypeFunc:
    return true;
  default:
    return false;
  }
}

class SDKNodeTypeNominal : public SDKNodeType {
public:
  SDKNodeTypeNominal(SDKNodeInitInfo Info) : SDKNodeType(Info,
    SDKNodeKind::TypeNominal) {}
  static bool classof(const SDKNode *N);
};

class SDKNodeTypeFunc : public SDKNodeType {
public:
  SDKNodeTypeFunc(SDKNodeInitInfo Info) : SDKNodeType(Info, SDKNodeKind::TypeFunc) {}
  bool isEscaping() const { return !hasTypeAttribute(TypeAttrKind::TAK_noescape); }
  static bool classof(const SDKNode *N);
};

template <typename T> const T *
SDKNode::getAs() const {
  if (T::classof(this))
    return static_cast<const T*>(this);
  return nullptr;
}

template <typename T> T *
SDKNode::getAs() {
  if (T::classof(this))
    return static_cast<T*>(this);
  return nullptr;
}

unsigned SDKNode::getChildIndex(NodePtr Child) const {
  return std::find_if(Children.begin(), Children.end(),
    [&](const NodeUniquePtr &P) { return P.get() == Child; }) - Children.begin();
}

NodePtr SDKNode::getOnlyChild() const {
    assert(Children.size() == 1 && "more that one child.");
    return (*Children.begin()).get();
  }

void SDKNode::addChild(NodeUniquePtr Child) {
  Child->Parent = this;
  Children.push_back(std::move(Child));
}

ArrayRef<NodeUniquePtr> SDKNode::getChildren() const {
  return llvm::makeArrayRef(Children.data(), Children.size());
}

void SDKNode::collectChildren(NodeVector &Bucket) const {
  for (auto &C : getChildren())
    Bucket.push_back(C.get());
}

NodePtr SDKNode::childAt(unsigned I) const {
  assert(I < getChildrenCount());
  return getChildren()[I].get();
}

void SDKNode::removeChild(NodePtr C) {
  Children.erase(std::find_if(Children.begin(), Children.end(),
                              [&](NodeUniquePtr &N) { return N.get() == C; }));
}

void SDKNode::addAnnotateComment(NodeAnnotation Anno, StringRef Comment) {
  assert(isAnnotatedAs(Anno) && "Cannot find annotation");
  AnnotateComments[Anno] = Comment;
}

StringRef SDKNode::getAnnotateComment(NodeAnnotation Anno) const {
  return AnnotateComments.find(Anno)->second;
}

bool SDKNode::isAnnotatedAs(NodeAnnotation Anno) const {
  return Annotations.find(Anno) != Annotations.end();;
}

void SDKNode::preorderVisit(NodePtr Root, SDKNodeVisitor &Visitor) {
  Visitor.visit(Root);
  Visitor.Ancestors.push_back(Root);
  for (auto &Child : Root->Children)
    preorderVisit(Child.get(), Visitor);
  Visitor.Ancestors.pop_back();
}

void SDKNode::postorderVisit(NodePtr Root, SDKNodeVisitor &Visitor) {
  Visitor.Ancestors.push_back(Root);
  for (auto &Child : Root->Children)
    postorderVisit(Child.get(), Visitor);
  Visitor.Ancestors.pop_back();
  Visitor.visit(Root);
}

bool SDKNode::operator==(const SDKNode &Other) const {
  if (getKind() != Other.getKind())
    return false;

  // Using type's equal algorithm
  if (auto *T1 = dyn_cast<SDKNodeType>(this)) {
    return SDKNodeType::areTypesSame(T1, (&Other)->getAs<SDKNodeType>());
  }

  if (getPrintedName() == Other.getPrintedName() &&
      Children.size() == Other.Children.size()) {
    for (unsigned long I = 0; I < Children.size(); I ++) {
      if ((*Children[I]) == (*Other.Children[I]))
        continue;
      return false;
    }
    return true;
  }
  return false;
}

class SDKNodeVectorViewer {
  ArrayRef<SDKNode*> Collection;
  llvm::function_ref<bool(NodePtr)> Selector;
  typedef ArrayRef<SDKNode*>::const_iterator VectorIt;
  VectorIt getNext(VectorIt Start);
  class ViwerIterator;

public:
  SDKNodeVectorViewer(ArrayRef<SDKNode*> Collection,
                      llvm::function_ref<bool(NodePtr)> Selector) :
                        Collection(Collection),
                        Selector(Selector) {}
  ViwerIterator begin();
  ViwerIterator end();
};

class SDKNodeVectorViewer::ViwerIterator :
    public std::iterator<std::input_iterator_tag, VectorIt> {
  SDKNodeVectorViewer &Viewer;
  VectorIt P;
public:
  ViwerIterator(SDKNodeVectorViewer &Viewer, VectorIt P) : Viewer(Viewer), P(P) {}
  ViwerIterator(const ViwerIterator& mit) : Viewer(mit.Viewer), P(mit.P) {}
  ViwerIterator& operator++();
  ViwerIterator operator++(int) {ViwerIterator tmp(*this); operator++(); return tmp;}
  bool operator==(const ViwerIterator& rhs) {return P==rhs.P;}
  bool operator!=(const ViwerIterator& rhs) {return P!=rhs.P;}
  const NodePtr& operator*() {return *P;}
};

SDKNodeVectorViewer::VectorIt
SDKNodeVectorViewer::getNext(VectorIt Start) {
  for (auto It = Start; It != Collection.end(); ++ It)
    if (Selector(*It))
      return It;
  return Collection.end();
}

SDKNodeVectorViewer::ViwerIterator&
SDKNodeVectorViewer::ViwerIterator::operator++() {
  P = Viewer.getNext(P + 1);
  return *this;
}

SDKNodeVectorViewer::ViwerIterator SDKNodeVectorViewer::begin() {
  return ViwerIterator(*this, getNext(Collection.begin()));
}

SDKNodeVectorViewer::ViwerIterator SDKNodeVectorViewer::end() {
  return ViwerIterator(*this, Collection.end());
}

class SDKNodeDecl;

KnownTypeKind SDKNodeType::getTypeKind() const {
#define KNOWN_TYPE(NAME) if (getName() == #NAME) return KnownTypeKind::NAME;
#include "DigesterEnums.def"
  return KnownTypeKind::Unknown;
}

ArrayRef<TypeAttrKind> SDKNodeType::getTypeAttributes() const {
  return llvm::makeArrayRef(TypeAttributes.data(), TypeAttributes.size());
}

void SDKNodeType::addTypeAttribute(TypeAttrKind AttrKind) {
  TypeAttributes.push_back(AttrKind);
}

bool SDKNodeType::hasTypeAttribute(TypeAttrKind DAKind) const {
  return std::find(TypeAttributes.begin(), TypeAttributes.end(), DAKind) !=
    TypeAttributes.end();
}

class SDKNodeRoot : public SDKNode {
public:
  SDKNodeRoot(SDKNodeInitInfo Info) : SDKNode(Info, SDKNodeKind::Root) {}
  static NodeUniquePtr getInstance();
  static bool classof(const SDKNode *N);
};

NodeUniquePtr SDKNodeRoot::getInstance() {
  SDKNodeInitInfo Info;
  Info.Name = InsertToBuffer("TopLevel");
  Info.PrintedName = InsertToBuffer("TopLevel");
  return Info.createSDKNode(SDKNodeKind::Root);
}

class SDKNodeNil : public SDKNode {
  friend struct SDKNodeInitInfo;
  SDKNodeNil(SDKNodeInitInfo Info) : SDKNode(Info, SDKNodeKind::Nil) {}
public:
  static SDKNode *getInstance();
  static bool classof(const SDKNode *N);
};

SDKNode* SDKNodeNil::getInstance() {
  static NodeUniquePtr Instance(new SDKNodeNil(SDKNodeInitInfo()));
  return Instance.get();
}

bool SDKNodeDecl::isDeprecated() {
  return hasDeclAttribute(SDKDeclAttrKind::DAK_deprecated);
}

bool SDKNodeDecl::isSDKPrivate() {
  if (getName().startswith("__"))
    return true;
  if (auto *PD = dyn_cast<SDKNodeDecl>(getParent()))
    return PD->isSDKPrivate();
  return false;
}

void SDKNodeDecl::printFullyQualifiedName(llvm::raw_ostream &OS) {
  std::vector<NodePtr> Parent;
  for (auto *P = getParent(); isa<SDKNodeDecl>(P); P = P->getParent())
    Parent.push_back(P);
  for (auto It = Parent.rbegin(); It != Parent.rend(); ++ It)
    OS << (*It)->getPrintedName() << ".";
  OS << getPrintedName();
}

StringRef SDKNodeDecl::getFullyQualifiedName() {
  llvm::SmallString<32> Buffer;
  llvm::raw_svector_ostream OS(Buffer);
  printFullyQualifiedName(OS);
  return InsertToBuffer(OS.str());
}

bool SDKNodeDecl::classof(const SDKNode *N) {
  switch (N->getKind()) {
    case SDKNodeKind::Constructor:
    case SDKNodeKind::Function:
    case SDKNodeKind::Getter:
    case SDKNodeKind::Setter:
    case SDKNodeKind::TypeAlias:
    case SDKNodeKind::TypeDecl:
    case SDKNodeKind::Var:
      return true;
    case SDKNodeKind::Nil:
    case SDKNodeKind::Root:
    case SDKNodeKind::TypeNominal:
    case SDKNodeKind::TypeFunc:
      return false;
  }
}

void SDKNodeDecl::addDeclAttribute(SDKDeclAttrKind DAKind) {
  DeclAttributes.push_back(DAKind);
}

bool SDKNodeDecl::hasDeclAttribute(SDKDeclAttrKind DAKind) const {
  return std::find(DeclAttributes.begin(), DeclAttributes.end(), DAKind) !=
    DeclAttributes.end();
}

ArrayRef<SDKDeclAttrKind> SDKNodeDecl::getDeclAttributes() const {
  return llvm::makeArrayRef(DeclAttributes.data(), DeclAttributes.size());
}

SDKNodeDecl *SDKNodeType::getClosestParentDecl() const {
  auto *Result = getParent();
  for (; !isa<SDKNodeDecl>(Result); Result = Result->getParent());
  return Result->getAs<SDKNodeDecl>();
}

bool SDKNodeType::areTypesSame(const SDKNodeType *Left,
                               const SDKNodeType *Right) {
  if (Left->getTypeAttributes().size() != Right->getTypeAttributes().size())
    return false;
  for (auto L : Left->getTypeAttributes()) {
    if (!contains(Right->getTypeAttributes(), L))
      return false;
  }
  return Left->getPrintedName() == Right->getPrintedName();
}

class SDKNodeTypeDecl : public SDKNodeDecl {
public:
  SDKNodeTypeDecl(SDKNodeInitInfo Info) : SDKNodeDecl(Info,
                                                      SDKNodeKind::TypeDecl) {}
  static bool classof(const SDKNode *N);
};

class SDKNodeTypeAlias : public SDKNodeDecl {
public:
  SDKNodeTypeAlias(SDKNodeInitInfo Info) : SDKNodeDecl(Info,
                                                       SDKNodeKind::TypeAlias) {}
  static bool classof(const SDKNode *N);
};

class SDKNodeVar : public SDKNodeDecl {
public:
  SDKNodeVar(SDKNodeInitInfo Info) : SDKNodeDecl(Info, SDKNodeKind::Var) {}
  static bool classof(const SDKNode *N);
};

class SDKNodeAbstractFunc : public SDKNodeDecl {
  const bool IsThrowing;
  const Optional<uint8_t> SelfIndex;

protected:
  SDKNodeAbstractFunc(SDKNodeInitInfo Info, SDKNodeKind Kind) :
                                                    SDKNodeDecl(Info, Kind),
                                                    IsThrowing(Info.IsThrowing),
                                                    SelfIndex(Info.SelfIndex){}
public:
  bool isThrowing() const { return IsThrowing; }
  uint8_t getSelfIndex() const { return SelfIndex.getValue(); }
  Optional<uint8_t> getSelfIndexOptional() const { return SelfIndex; }
  bool hasSelfIndex() const { return SelfIndex.hasValue(); }
  static bool classof(const SDKNode *N);
};

bool SDKNodeAbstractFunc::classof(const SDKNode *N) {
  switch (N->getKind()) {
    case SDKNodeKind::Function:
    case SDKNodeKind::Setter:
    case SDKNodeKind::Getter:
    case SDKNodeKind::Constructor:
      return true;

    default:
      return false;
  }
}

class SDKNodeFunction : public SDKNodeAbstractFunc {
public:
  SDKNodeFunction(SDKNodeInitInfo Info) : SDKNodeAbstractFunc(Info,
                                                       SDKNodeKind::Function) {}
  SDKNode *getReturnType() { return (*getChildBegin()).get(); }
  static bool classof(const SDKNode *N);
  static StringRef getTypeRoleDescription(unsigned Index);
};

StringRef SDKNodeFunction::getTypeRoleDescription(unsigned Index) {
  if (Index == 0) {
    return InsertToBuffer("return");
  } else if (Index == 1) {
    return InsertToBuffer("1st parameter");
  } else if (Index == 2) {
    return InsertToBuffer("2nd parameter");
  } else if (Index == 3) {
    return InsertToBuffer("3rd parameter");
  } else {
    llvm::SmallString<4> Buffer;
    Buffer += std::to_string(Index);
    Buffer += "th parameter";
    return InsertToBuffer(Buffer.str());
  }
}

class SDKNodeConstructor : public SDKNodeAbstractFunc {
public:
  SDKNodeConstructor(SDKNodeInitInfo Info) : SDKNodeAbstractFunc(Info,
                                                    SDKNodeKind::Constructor) {}
  static bool classof(const SDKNode *N);
};

class SDKNodeGetter : public SDKNodeAbstractFunc {
public:
  SDKNodeGetter(SDKNodeInitInfo Info) : SDKNodeAbstractFunc(Info,
                                                         SDKNodeKind::Getter) {}
  static bool classof(const SDKNode *N);
};

class SDKNodeSetter : public SDKNodeAbstractFunc {
public:
  SDKNodeSetter(SDKNodeInitInfo Info) : SDKNodeAbstractFunc(Info,
                                                         SDKNodeKind::Setter) {}
  static bool classof(const SDKNode *N);
};

#define NODE_KIND(X)                                                       \
  bool SDKNode##X::classof(const SDKNode *N) {                             \
    return N->getKind() == SDKNodeKind::X;                                 \
  }
#include "DigesterEnums.def"


NodeUniquePtr SDKNode::constructSDKNode(llvm::yaml::MappingNode *Node) {
  static auto GetScalarString = [&](llvm::yaml::Node *N) -> StringRef {
    auto WithQuote = cast<llvm::yaml::ScalarNode>(N)->getRawValue();
    return WithQuote.substr(1, WithQuote.size() - 2);
  };
  SDKNodeKind Kind;
  SDKNodeInitInfo Info;
  NodeOwnedVector Children;

  for (auto Pair : *Node) {
    auto Key = GetScalarString(Pair.getKey());
    if (Key == Key_kind) {
      Kind = llvm::StringSwitch<SDKNodeKind>(GetScalarString(Pair.getValue()))
#define NODE_KIND(NAME) .Case(#NAME, SDKNodeKind::NAME)
#include "DigesterEnums.def"
      ;
    } else if (Key == Key_name) {
      Info.Name = GetScalarString(Pair.getValue());
    } else if (Key == Key_selfIndex) {
      Info.SelfIndex = std::stoi(cast<llvm::yaml::ScalarNode>(Pair.getValue())->
                                 getRawValue());
    } else if (Key == Key_usr) {
      Info.USR = GetScalarString(Pair.getValue());
    } else if (Key == Key_location) {
      Info.Location = GetScalarString(Pair.getValue());
    } else if (Key == Key_children) {
      for (auto &Mapping : *cast<llvm::yaml::SequenceNode>(Pair.getValue())) {
        Children.push_back(constructSDKNode(cast<llvm::yaml::MappingNode>(&Mapping)));
      }
    } else if (Key == Key_printedName) {
      Info.PrintedName = GetScalarString(Pair.getValue());
    } else if (Key == Key_moduleName) {
      Info.ModuleName = GetScalarString(Pair.getValue());
    } else if (Key == Key_throwing) {
      Info.IsThrowing = true;
    } else if (Key == Key_typeAttributes) {
      auto *Seq = cast<llvm::yaml::SequenceNode>(Pair.getValue());
      for (auto It = Seq->begin(); It != Seq->end(); ++ It) {
        Info.TypeAttrs.push_back(
          llvm::StringSwitch<TypeAttrKind>(GetScalarString(&*It))
#define TYPE_ATTR(X) .Case(#X, TypeAttrKind::TAK_##X)
#include "swift/AST/Attr.def"
          .Case("Count", TypeAttrKind::TAK_Count));
      }
    } else if (Key == Key_declAttributes) {
      auto *Seq = cast<llvm::yaml::SequenceNode>(Pair.getValue());
      for (auto It = Seq->begin(); It != Seq->end(); ++ It) {
        Info.DeclAttrs.push_back(
          llvm::StringSwitch<SDKDeclAttrKind>(GetScalarString(&*It))
#define DECL_ATTR(X) .Case(#X, SDKDeclAttrKind::DAK_##X)
#include "DigesterEnums.def"
          );
      }
    } else if (Key == Key_declKind) {
      Info.DKind = llvm::StringSwitch<DeclKind>(GetScalarString(Pair.getValue()))
#define DECL(X, PARENT) .Case(#X, DeclKind::X)
#include "swift/AST/DeclNodes.def"
      ;
    } else {
      llvm_unreachable("Cannot parse key.");
    }
  };
  NodeUniquePtr Result = Info.createSDKNode(Kind);
  for (auto &C : Children) {
    Result->addChild(std::move(C));
  }
  return Result;
}

// The pretty printer of a tree of SDKNode
class SDKNodeDumpVisitor : public SDKNodeVisitor {
  void dumpSpace(int Num) {
    while (Num != 0) {
      llvm::outs() << "\t";
      Num --;
    }
  }
  void visit(NodePtr Node) override {
    dumpSpace(depth());
    llvm::outs() << "[" << Node->getKind() << "]" << Node->getName() << "\n";
  }
public:
  SDKNodeDumpVisitor() {};
};

class DumpMatchListener : public MatchedNodeListener {
  void foundMatch(NodePtr Left, NodePtr Right) override {
    llvm::outs() << Left->getName() << "->" << Right->getName() << "\n";
  };
  bool isMatched(NodePtr Left, NodePtr Right) override {
    return true;
  };
};

static StringRef getPrintedName(Type Ty) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  PrintOptions PO;
  PO.SkipAttributes = true;
  Ty.print(OS, PO);
  return InsertToBuffer(OS.str());
}

static StringRef getTypeName(Type Ty) {
  if (Ty->getKind() == TypeKind::Paren) {
    return InsertToBuffer("Paren");
  }
  if (Ty->isVoid()) {
    return InsertToBuffer("Void");
  }
  if (NameAliasType *NAT = dyn_cast<NameAliasType>(Ty.getPointer())) {
    return NAT->getDecl()->getNameStr();
  }
  if (Ty->getAnyNominal()) {
    return Ty->getAnyNominal()->getNameStr();
  }
#define TYPE(id, parent)                                                      \
  if (Ty->getKind() == TypeKind::id) {                                        \
    return InsertToBuffer(#id);                                               \
  }
#include "swift/AST/TypeNodes.def"
  llvm_unreachable("Unhandled type name.");
}

static StringRef calculateUsr(ValueDecl *VD) {
  llvm::SmallString<64> SS;
  llvm::raw_svector_ostream OS(SS);
  if (!ide::printDeclUSR(VD, OS)) {
    return InsertToBuffer(SS.str());
  }
  return StringRef();
}

static StringRef calculateLocation(ValueDecl *VD) {
  auto &Ctx = VD->getASTContext();
  auto &Importer = static_cast<ClangImporter &>(*Ctx.getClangModuleLoader());

  clang::SourceManager &SM = Importer.getClangPreprocessor().getSourceManager();
  if (ClangNode CN = VD->getClangNode()) {
    clang::SourceLocation Loc = CN.getLocation();
    Loc = SM.getFileLoc(Loc);
    if (Loc.isValid())
      return InsertToBuffer(Loc.printToString(SM));
  }

  return StringRef();
}

static bool isFunctionTypeNoEscape(Type Ty) {
  if (auto *AFT = Ty->getAs<AnyFunctionType>()) {
    return AFT->getExtInfo().isNoEscape();
  }
  return false;
}

static StringRef getPrintedName(ValueDecl *VD) {
  llvm::SmallString<32> Result;
  if (auto FD = dyn_cast<AbstractFunctionDecl>(VD)) {
    auto DM = FD->getFullName();

    Result.append(DM.getBaseName().empty() ? "_" : DM.getBaseName().str());
    Result.append("(");
    for (auto Arg : DM.getArgumentNames()) {
      Result.append(Arg.empty() ? "_" : Arg.str());
      Result.append(":");
    }
    Result.append(")");
    return InsertToBuffer(Result.str());
  }
  auto DM = VD->getFullName();
  Result.append(DM.getBaseName().str());
  return InsertToBuffer(Result.str());
}

static bool isFuncThrowing(ValueDecl *VD) {
  if (auto AF = dyn_cast<AbstractFunctionDecl>(VD)) {
    return AF->hasThrows();
  }
  return false;
}

static Optional<uint8_t> getSelfIndex(ValueDecl *VD) {
  if (auto AF = dyn_cast<AbstractFunctionDecl>(VD)) {
    if (AF->isImportAsInstanceMember())
      return AF->getSelfIndex();
  }
  return None;
}

SDKNodeInitInfo::SDKNodeInitInfo(Type Ty) : Name(getTypeName(Ty)),
                                            PrintedName(getPrintedName(Ty)) {
  if (isFunctionTypeNoEscape(Ty))
    TypeAttrs.push_back(TypeAttrKind::TAK_noescape);
}

SDKNodeInitInfo::SDKNodeInitInfo(ValueDecl *VD) :
    Name(VD->hasName() ? VD->getName().str() : InsertToBuffer("_")),
    PrintedName(getPrintedName(VD)), DKind(VD->getKind()),
    USR(calculateUsr(VD)), Location(calculateLocation(VD)),
    ModuleName(VD->getModuleContext()->getName().str()),
    IsThrowing(isFuncThrowing(VD)), SelfIndex(getSelfIndex(VD)) {
  if (VD->getAttrs().getDeprecated(VD->getASTContext()))
    DeclAttrs.push_back(SDKDeclAttrKind::DAK_deprecated);
}

NodeUniquePtr SDKNodeInitInfo::createSDKNode(SDKNodeKind Kind) {
  NodeUniquePtr Result;
  switch(Kind) {
#define NODE_KIND(X)                                                           \
case SDKNodeKind::X:                                                           \
  Result.reset(static_cast<SDKNode*>(new SDKNode##X(*this)));                  \
  break;
#include "DigesterEnums.def"
  }
  return Result;
}

// Recursively construct a node that represents a type, for instance,
// representing the the return value type of a function decl.
static NodeUniquePtr constructTypeNode(Type T) {
  NodeUniquePtr Root = SDKNodeInitInfo(T).createSDKNode(SDKNodeKind::TypeNominal);

  if (isa<NameAliasType>(T.getPointer()))
    return Root;

  if (auto Fun = T->getAs<AnyFunctionType>()) {
    NodeUniquePtr Root = SDKNodeInitInfo(T).createSDKNode(SDKNodeKind::TypeFunc);

    // Still, return type first
    Root->addChild(constructTypeNode(Fun->getResult()));
    Root->addChild(constructTypeNode(Fun->getInput()));
    return Root;
  }

  // Keep paren type as a stand-alone level.
  if (auto *PT = dyn_cast<ParenType>(T.getPointer())) {
    Root->addChild(constructTypeNode(PT->getSinglyDesugaredType()));
    return Root;
  }

  // Handle the case where Type has sub-types.
  if (auto BGT = T->getAs<BoundGenericType>()) {
    for (auto Arg : BGT->getGenericArgs()) {
      Root->addChild(constructTypeNode(Arg));
    }
  } else if (auto Tup = T->getAs<TupleType>()) {
    for (auto Elt : Tup->getElementTypes())
      Root->addChild(constructTypeNode(Elt));
  } else if (auto MTT = T->getAs<AnyMetatypeType>()) {
    Root->addChild(constructTypeNode(MTT->getInstanceType()));
  } else if (auto ATT = T->getAs<ArchetypeType>()) {
    for (auto Pro : ATT->getConformsTo()) {
      Root->addChild(constructTypeNode(Pro->getDeclaredType()));
    }
  }
  return Root;
}

// Construct a node for a function decl. The first child of the function decl
// is guaranteed to be the return value type of this function.
// We sometimes skip the first parameter because it can be metatype of dynamic
// this if the function is a member function.
static NodeUniquePtr constructFunctionNode(FuncDecl* FD, SDKNodeKind Kind,
                                           bool SkipFirst) {
  auto Func = SDKNodeInitInfo(FD).createSDKNode(Kind);
  Func->addChild(constructTypeNode(FD->getResultType()));
  for (auto *paramList : FD->getParameterLists()) {
    for (auto param : *paramList)
      Func->addChild(constructTypeNode(param->getInterfaceType()));
  }
  if (SkipFirst) {
    Func->removeChild(Func->getChildBegin() + 1);
  }
  return Func;
}

static NodeUniquePtr constructInitNode(ConstructorDecl *CD) {
  auto Func = SDKNodeInitInfo(CD).createSDKNode(SDKNodeKind::Constructor);
  Func->addChild(constructTypeNode(CD->getResultType()));
  for (auto *paramList : CD->getParameterLists()) {
    for (auto param : *paramList)
      Func->addChild(constructTypeNode(param->getInterfaceType()));
  }

  // Always remove the first parameter in init.
  Func->removeChild(Func->getChildBegin() + 1);
  return Func;
}

static bool shouldIgnore(Decl *D) {
  if (D->isPrivateStdlibDecl(false))
    return true;
  if (AvailableAttr::isUnavailable(D))
    return true;
  if (isa<ConstructorDecl>(D))
    return false;
  if (isa<OperatorDecl>(D))
    return true;
  if (auto VD = dyn_cast<ValueDecl>(D)) {
    if (VD->isOperator())
      return true;
    if (VD->getName().empty())
      return true;
  }

  if (auto *ClangD = D->getClangDecl()) {
    if (isa<clang::ObjCIvarDecl>(ClangD))
      return true;
    if (isa<clang::FieldDecl>(ClangD))
      return true;
    if (ClangD->hasAttr<clang::SwiftPrivateAttr>())
      return true;
  }
  return false;
}

static void addMembersToRoot(NodeUniquePtr &Root, IterableDeclContext *Context);

static NodeUniquePtr constructTypeDeclNode(NominalTypeDecl *NTD) {
  auto TypeNode = SDKNodeInitInfo(NTD).createSDKNode(SDKNodeKind::TypeDecl);
  addMembersToRoot(TypeNode, NTD);
  for (auto Ext : NTD->getExtensions()) {
    addMembersToRoot(TypeNode, Ext);
  }
  return TypeNode;
}

static NodeUniquePtr constructVarNode(ValueDecl *VD) {
  auto Var = SDKNodeInitInfo(VD).createSDKNode(SDKNodeKind::Var);
  Var->addChild(constructTypeNode(VD->getInterfaceType()));
  if (auto VAD = dyn_cast<AbstractStorageDecl>(VD)) {
    if (auto Getter = VAD->getGetter())
      Var->addChild(constructFunctionNode(Getter, SDKNodeKind::Getter, false));
    if (auto Setter = VAD->getSetter())
      Var->addChild(constructFunctionNode(Setter, SDKNodeKind::Setter, false));
  }
  return Var;
}

static NodeUniquePtr constructTypeAliasNode(TypeAliasDecl *TAD) {
  auto Alias = SDKNodeInitInfo(TAD).createSDKNode(SDKNodeKind::TypeAlias);
  Alias->addChild(constructTypeNode(TAD->getUnderlyingType()));
  return Alias;
}

static void addMembersToRoot(NodeUniquePtr &Root, IterableDeclContext *Context) {
  for (auto *Member : Context->getMembers()) {
    if (shouldIgnore(Member))
      continue;
    if (auto Func = dyn_cast<FuncDecl>(Member)) {
      Root->addChild(constructFunctionNode(Func, SDKNodeKind::Function, true));
    } else if (auto CD = dyn_cast<ConstructorDecl>(Member)) {
      Root->addChild(constructInitNode(CD));
    } else if (auto VD = dyn_cast<VarDecl>(Member)) {
      Root->addChild(constructVarNode(VD));
    } else if (auto TAD = dyn_cast<TypeAliasDecl>(Member)) {
      Root->addChild(constructTypeAliasNode(TAD));
    } else if (auto EED = dyn_cast<EnumElementDecl>(Member)) {
      Root->addChild(constructVarNode(EED));
    } else if (auto NTD = dyn_cast<NominalTypeDecl>(Member)) {
      Root->addChild(constructTypeDeclNode(NTD));
    }
  }
}

static void emitSDKNodeRoot(raw_ostream&, NodeUniquePtr&);

static std::pair<std::unique_ptr<llvm::MemoryBuffer>, NodeUniquePtr>
parseJsonEmit(StringRef);

class SwiftDeclCollector : public VisibleDeclConsumer {

  std::vector<std::unique_ptr<llvm::MemoryBuffer>> OwnedBuffers;
  NodeUniquePtr RootNode;
  llvm::DenseSet<ValueDecl*> KnownDecls;
  // Collected and sorted after we get all of them.
  std::vector<ValueDecl *> ClangMacros;

public:
  void visitAllRoots(SDKNodeVisitor &Visitor) {
    SDKNode::preorderVisit(RootNode.get(), Visitor);
  }
  SwiftDeclCollector() : RootNode(SDKNodeRoot::getInstance()) {}

  // Construct all roots vector from a given file where a forest was
  // previously dumped.
  void deSerialize(StringRef Filename) {
    auto Pair = parseJsonEmit(Filename);
    OwnedBuffers.push_back(std::move(Pair.first));
    RootNode = std::move(Pair.second);
  }

  // Serialize the content of all roots to a given file using json format.
  void serialize(StringRef Filename) {
    std::error_code EC;
    llvm::raw_fd_ostream fs(Filename, EC, llvm::sys::fs::F_None);
    emitSDKNodeRoot(fs, RootNode);
  }

  // After collecting decls, either from imported modules or from a previously
  // serialized json file, using this function to get the root of the SDK.
  NodePtr getSDKRoot() {
    return RootNode.get();
  }

  void printTopLevelNames() {
    for (auto &Node : RootNode->getChildren()) {
      llvm::outs() << Node->getKind() << ": " << Node->getName() << '\n';
    }
  }

public:
  void lookupVisibleDecls(ArrayRef<Module *> Modules) {
    for (auto M : Modules) {
      llvm::SmallVector<Decl*, 512> Decls;
      M->getDisplayDecls(Decls);
      for (auto D : Decls) {
        if (auto VD = dyn_cast<ValueDecl>(D))
          foundDecl(VD, DeclVisibilityKind::DynamicLookup);
      }
    }

    // Now sort the macros before processing so that we can have deterministic
    // output.
    llvm::array_pod_sort(ClangMacros.begin(), ClangMacros.end(),
       [](ValueDecl * const *lhs,
          ValueDecl * const *rhs) -> int {
         return (*lhs)->getNameStr().compare((*rhs)->getNameStr());
       });

    for (auto *VD : ClangMacros)
      processDecl(VD);
  }

  void processDecl(ValueDecl *VD) {
    if (shouldIgnore(VD))
      return;

    if (auto FD = dyn_cast<FuncDecl>(VD)) {
      RootNode->addChild(constructFunctionNode(FD, SDKNodeKind::Function, false));
    } else if (auto NTD = dyn_cast<NominalTypeDecl>(VD)) {
      RootNode->addChild(constructTypeDeclNode(NTD));
    }
    if (auto VAD = dyn_cast<VarDecl>(VD)) {
      RootNode->addChild(constructVarNode(VAD));
    }
    if (auto TAD = dyn_cast<TypeAliasDecl>(VD)) {
      RootNode->addChild(constructTypeAliasNode(TAD));
    }
  }

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason) override {
    if (KnownDecls.count(VD))
      return;
    KnownDecls.insert(VD);

    if (VD->getClangMacro()) {
      // Collect macros, we will sort them afterwards.
      ClangMacros.push_back(VD);
      return;
    }

    processDecl(VD);
  }
};
} // End of anonymous namespace.


namespace swift {
  namespace json {
  // In the namespace of swift::json, we define several functions so that the
  // json serializer will know how to interpret and dump types defined in this
  // file.
  template<>
    struct ScalarEnumerationTraits<SDKNodeKind> {
      static void enumeration(Output &out, SDKNodeKind &value) {
#define NODE_KIND(X) out.enumCase(value, #X, SDKNodeKind::X);
#include "DigesterEnums.def"
      }
    };

    template<>
    struct ScalarEnumerationTraits<TypeAttrKind> {
      static void enumeration(Output &out, TypeAttrKind &value) {
#define TYPE_ATTR(X) out.enumCase(value, #X, TypeAttrKind::TAK_##X);
#include "swift/AST/Attr.def"
        out.enumCase(value, "Count", TypeAttrKind::TAK_Count);
      }
    };

    template<>
    struct ScalarEnumerationTraits<SDKDeclAttrKind> {
      static void enumeration(Output &out, SDKDeclAttrKind &value) {
#define DECL_ATTR(X) out.enumCase(value, #X, SDKDeclAttrKind::DAK_##X);
#include "DigesterEnums.def"
      }
    };

    template<>
    struct ScalarEnumerationTraits<DeclKind> {
      static void enumeration(Output &out, DeclKind &value) {
#define DECL(X, PARENT) out.enumCase(value, #X, DeclKind::X);
#include "swift/AST/DeclNodes.def"
      }
    };

    template<>
    struct ObjectTraits<NodeUniquePtr> {
      static void mapping(Output &out, NodeUniquePtr &value) {
        auto Kind = value->getKind();
        auto Name = value->getName();
        auto PrintedName = value->getPrintedName();

        out.mapRequired(Key_kind, Kind);
        out.mapRequired(Key_name, Name);
        out.mapRequired(Key_printedName, PrintedName);

        if (auto D = dyn_cast<SDKNodeDecl>(value.get())) {
          DeclKind DK = D->getDeclKind();
          StringRef Usr = D->getUsr();
          StringRef Location = D->getLocation();
          StringRef ModuleName = D->getModuleName();

          out.mapRequired(Key_declKind, DK);
          out.mapRequired(Key_usr, Usr);
          out.mapRequired(Key_location, Location);
          out.mapRequired(Key_moduleName, ModuleName);

          if (auto F = dyn_cast<SDKNodeAbstractFunc>(value.get())) {
            if (bool isThrowing = F->isThrowing())
              out.mapRequired(Key_throwing, isThrowing);
            if (F->hasSelfIndex()) {
              auto Index = F->getSelfIndex();
              out.mapRequired(Key_selfIndex, Index);
            }
          }
          auto Attributes = D->getDeclAttributes();
          if (!Attributes.empty())
            out.mapRequired(Key_declAttributes, Attributes);
        } else if (auto T = dyn_cast<SDKNodeType>(value.get())) {
          auto Attributes = T->getTypeAttributes();
          if (!Attributes.empty())
            out.mapRequired(Key_typeAttributes, Attributes);
        }
        if (!value->isLeaf()) {
          ArrayRef<NodeUniquePtr> Children = value->getChildren();
          out.mapRequired(Key_children, Children);
        }
      }
    };

    template<>
    struct ArrayTraits<ArrayRef<NodeUniquePtr>> {
      static size_t size(Output &out, ArrayRef<NodeUniquePtr> &seq) {
        return seq.size();
      }
      static NodeUniquePtr &element(Output &, ArrayRef<NodeUniquePtr> &seq,
                                    size_t index) {
        return const_cast<NodeUniquePtr&>(seq[index]);
      }
    };

    template<>
    struct ArrayTraits<ArrayRef<TypeAttrKind>> {
      static size_t size(Output &out, ArrayRef<TypeAttrKind> &seq) {
        return seq.size();
      }
      static TypeAttrKind& element(Output &, ArrayRef<TypeAttrKind> &seq,
                                   size_t index) {
        return const_cast<TypeAttrKind&>(seq[index]);
      }
    };

    template<>
    struct ArrayTraits<ArrayRef<SDKDeclAttrKind>> {
      static size_t size(Output &out, ArrayRef<SDKDeclAttrKind> &seq) {
        return seq.size();
      }
      static SDKDeclAttrKind& element(Output &, ArrayRef<SDKDeclAttrKind> &seq,
                                   size_t index) {
        return const_cast<SDKDeclAttrKind&>(seq[index]);
      }
    };
  }
}

namespace  {// Anonymous namespace resumes.

// Serialize a forest of SDKNode trees to the given stream.
static void emitSDKNodeRoot(llvm::raw_ostream &os, NodeUniquePtr &Root) {
  json::Output yout(os);
  yout << Root;
}

// Deserialize an SDKNode tree.
std::pair<std::unique_ptr<llvm::MemoryBuffer>, NodeUniquePtr>
parseJsonEmit(StringRef FileName) {
  namespace yaml = llvm::yaml;

  // Load the input file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
  llvm::MemoryBuffer::getFileOrSTDIN(FileName);
  if (!FileBufOrErr) {
    llvm_unreachable("Failed to read json file");
  }
  StringRef Buffer = FileBufOrErr->get()->getBuffer();
  llvm::SourceMgr SM;
  yaml::Stream Stream(Buffer, SM);
  NodeUniquePtr Result;
  for (auto DI = Stream.begin(); DI != Stream.end(); ++ DI) {
    assert(DI != Stream.end() && "Failed to read a document");
    yaml::Node *N = DI->getRoot();
    assert(N && "Failed to find a root");
    Result = SDKNode::constructSDKNode(cast<yaml::MappingNode>(N));
  }
  return {std::move(FileBufOrErr.get()), std::move(Result)};
}

// Given two NodeVector, this matches SDKNode by the order of their appearance
// in the respective NodeVector. We use this in the order-sensitive cases, such
// as parameters in a function decl.
class SequentialNodeMatcher : public NodeMatcher {
  ArrayRef<NodeUniquePtr> Left;
  ArrayRef<NodeUniquePtr> Right;
  MatchedNodeListener &Listener;
public:
  SequentialNodeMatcher(ArrayRef<NodeUniquePtr> Left,
                        ArrayRef<NodeUniquePtr> Right,
                        MatchedNodeListener &Listener) :
                          Left(Left), Right(Right), Listener(Listener) {}

  virtual void match() override {
    for (unsigned long i = 0; i < std::max(Left.size(), Right.size()); i ++) {
      auto L = i < Left.size() ? Left[i].get() : SDKNodeNil::getInstance();
      auto R = i < Right.size() ? Right[i].get() : SDKNodeNil::getInstance();
      if (*L == *R)
        continue;
      if (L->getKind() == SDKNodeKind::Nil || R->getKind() == SDKNodeKind::Nil)
        Listener.foundRemoveAddMatch(L, R);
      else
        Listener.foundMatch(L, R);
    }
  }
};
struct NodeMatch {
  NodePtr Left;
  NodePtr Right;
};

class BestMatchMatcher : public NodeMatcher {
  NodeVector &Left;
  NodeVector &Right;
  llvm::function_ref<bool(NodePtr, NodePtr)> CanMatch;
  llvm::function_ref<bool(NodeMatch, NodeMatch)> IsFirstMatchBetter;
  MatchedNodeListener &Listener;
  llvm::SmallPtrSet<NodePtr, 16> MatchedRight;

  bool internalCanMatch(NodePtr L, NodePtr R) {
    return MatchedRight.count(R) == 0 && CanMatch(L, R);
  }

  Optional<NodePtr> findBestMatch(NodePtr Pin, NodeVector& Candidates) {
    Optional<NodePtr> Best;
    for (auto Can : Candidates) {
      if (!internalCanMatch(Pin, Can))
        continue;
      if (!Best.hasValue() ||
          IsFirstMatchBetter({Pin, Can}, {Pin, Best.getValue()}))
        Best = Can;
    }
    return Best;
  }

public:
  BestMatchMatcher(NodeVector &Left, NodeVector &Right,
                   llvm::function_ref<bool(NodePtr, NodePtr)> CanMatch,
                   llvm::function_ref<bool(NodeMatch, NodeMatch)> IsFirstMatchBetter,
                   MatchedNodeListener &Listener) : Left(Left), Right(Right),
  CanMatch(CanMatch),
  IsFirstMatchBetter(IsFirstMatchBetter),
  Listener(Listener){}

  virtual void match() {
    for (auto L : Left) {
      if (auto Best = findBestMatch(L, Right)) {
        MatchedRight.insert(Best.getValue());
        Listener.foundMatch(L, Best.getValue());
      }
    }
  }
};

class RemovedAddedNodeMatcher : public NodeMatcher, public MatchedNodeListener {
  NodeVector &Removed;
  NodeVector &Added;
  MatchedNodeListener &Listener;

  NodeVector RemovedMatched;
  NodeVector AddedMatched;

  void handleUnmatch(NodeVector &Matched, NodeVector &All, bool Left) {
    for (auto A : SDKNodeVectorViewer(All,
                            [&](SDKNode *N) { return !contains(Matched, N);})) {
      if (Left)
        Listener.foundRemoveAddMatch(A, SDKNodeNil::getInstance());
      else
        Listener.foundRemoveAddMatch(SDKNodeNil::getInstance(), A);
    }
  }

  bool detectFuncToProperty(SDKNode *R, SDKNode *A) {
    if (R->getKind() == SDKNodeKind::Function) {
      if (A->getKind() == SDKNodeKind::Var) {
        if (A->getName().compare_lower(R->getName()) == 0) {
          R->annotate(NodeAnnotation::GetterToProperty);
        } else if (R->getName().startswith("get") &&
                   R->getName().substr(3).compare_lower(A->getName()) == 0) {
          R->annotate(NodeAnnotation::GetterToProperty);
        } else if (R->getName().startswith("set") &&
                   R->getName().substr(3).compare_lower(A->getName()) == 0) {
          R->annotate(NodeAnnotation::SetterToProperty);
        } else {
          return false;
        }
        foundMatch(R, A);
        return true;
      }
    }
    return false;
  }

  static bool isAnonymousEnum(SDKNodeDecl *N) {
    return N->getKind() == SDKNodeKind::Var &&
      N->getUsr().startswith("c:@Ea@");
  }

  static bool isNominalEnum(SDKNodeDecl *N) {
    return N->getKind() == SDKNodeKind::TypeDecl &&
    N->getUsr().startswith("c:@E@");
  }

  static Optional<StringRef> getLastPartOfUsr(SDKNodeDecl *N) {
    auto LastPartIndex = N->getUsr().find_last_of('@');
    if (LastPartIndex == StringRef::npos)
      return None;
    return N->getUsr().substr(LastPartIndex + 1);
  }

  bool detectModernizeEnum(SDKNodeDecl *R, SDKNodeDecl *A) {
    if (!isAnonymousEnum(R) || !isNominalEnum(A))
      return false;

    auto LastPartOfR = getLastPartOfUsr(R);
    if (!LastPartOfR)
      return false;

    for (auto &Child : A->getChildren()) {
      if (auto VC = dyn_cast<SDKNodeVar>(Child.get())) {
      auto LastPartOfA = getLastPartOfUsr(VC);
        if (LastPartOfA && LastPartOfR.getValue() == LastPartOfA.getValue()) {
          R->annotate(NodeAnnotation::ModernizeEnum);
          llvm::Twine FullName = llvm::Twine().concat(A->getName()).concat(".").
            concat(Child->getName());
          R->addAnnotateComment(NodeAnnotation::ModernizeEnum,
                                InsertToBuffer(FullName.str()));
          foundMatch(R, A);
          return true;
        }
      }
    }
    return false;
  }

  bool detectSameAnonymousEnum(SDKNodeDecl *R, SDKNodeDecl *A) {
    if (!isAnonymousEnum(R) || !isAnonymousEnum(A))
      return false;
    auto LastR = getLastPartOfUsr(R);
    auto LastA = getLastPartOfUsr(A);
    if (LastR && LastA && LastR.getValue() == LastA.getValue()) {
      foundMatch(R, A);
      return true;
    }
    return false;
  }

  static bool isNameTooSimple(StringRef N) {
    static std::vector<std::string> SimpleNames = {"unit", "data", "log", "coding",
      "url", "name", "date", "datecomponents", "notification", "urlrequest",
      "personnamecomponents", "measurement", "dateinterval", "indexset"};
    return std::find(SimpleNames.begin(), SimpleNames.end(), N) !=
      SimpleNames.end();
  }

  static bool isSimilarName(StringRef L, StringRef R) {
    auto LL = L.lower();
    auto RR = R.lower();
    if (isNameTooSimple(LL) || isNameTooSimple(RR))
      return false;
    if (((StringRef)LL).startswith(RR) || ((StringRef)RR).startswith(LL))
      return true;
    if (((StringRef)LL).startswith((llvm::Twine("ns") + RR).str()) ||
        ((StringRef)RR).startswith((llvm::Twine("ns") + LL).str()))
      return true;
    if (((StringRef)LL).endswith(RR) || ((StringRef)RR).endswith(LL))
      return true;
    return false;
  }

  /// Whether two decls of different decl kinds can be considered as rename.
  static bool isDeclKindCrossable(DeclKind DK1, DeclKind DK2, bool First) {
    if (DK1 == DK2)
      return true;
    if (DK1 == DeclKind::Var && DK2 == DeclKind::EnumElement)
      return true;
    return First && isDeclKindCrossable(DK2, DK1, false);
  }

  static bool isRename(NodePtr L, NodePtr R) {
    if (L->getKind() != R->getKind())
      return false;
    if (isa<SDKNodeConstructor>(L))
      return false;
    if (auto LD = dyn_cast<SDKNodeDecl>(L)) {
      auto *RD = R->getAs<SDKNodeDecl>();
      return isDeclKindCrossable(LD->getDeclKind(), RD->getDeclKind(), true) &&
        isSimilarName(LD->getName(), RD->getName());
    }
    return false;
  }

  static bool isBetterMatch(NodeMatch Match1, NodeMatch Match2) {
    assert(Match1.Left == Match2.Left);
    auto Left = Match1.Left;
    auto *M1Right = Match1.Right->getAs<SDKNodeDecl>();
    auto *M2Right = Match2.Right->getAs<SDKNodeDecl>();

    // Consider non-deprecated nodes better matches.
    auto Dep1 = M1Right->isDeprecated();
    auto Dep2 = M2Right->isDeprecated();
    if (Dep1 ^ Dep2) {
      return Dep2;
    }

    // If two names are identical, measure whose printed names is closer.
    if (M1Right->getName() == M2Right->getName()) {
      return
        M1Right->getPrintedName().edit_distance(Left->getPrintedName()) <
        M2Right->getPrintedName().edit_distance(Left->getPrintedName());
    }

#define DIST(A, B) (std::max(A, B) - std::min(A, B))
    return
      DIST(Left->getName().size(), Match1.Right->getName().size()) <
      DIST(Left->getName().size(), Match2.Right->getName().size());
#undef DIST
  }

  void foundMatch(NodePtr R, NodePtr A) override {
    Listener.foundRemoveAddMatch(R, A);
    RemovedMatched.push_back(R);
    AddedMatched.push_back(A);
  }

public:
  RemovedAddedNodeMatcher(NodeVector &Removed, NodeVector &Added,
                          MatchedNodeListener &Listener) : Removed(Removed),
                            Added(Added), Listener(Listener) {}

  virtual void match() override {
    auto IsDecl = [](NodePtr P) { return isa<SDKNodeDecl>(P); };
    for (auto R : SDKNodeVectorViewer(Removed, IsDecl)) {
      for (auto A : SDKNodeVectorViewer(Added, IsDecl)) {
        auto RD = R->getAs<SDKNodeDecl>();
        auto AD = A->getAs<SDKNodeDecl>();
        if (detectFuncToProperty(RD, AD) || detectModernizeEnum(RD, AD) ||
            detectSameAnonymousEnum(RD, AD)) {
          break;
        }
      }
    }

    // Rename detection starts.
    NodeVector RenameLeft;
    NodeVector RenameRight;


    for (auto Remain : SDKNodeVectorViewer(Removed, [&](SDKNode *N)
                                 { return !contains(RemovedMatched, N); })) {
      RenameLeft.push_back(Remain);
    }

    for (auto Remain : SDKNodeVectorViewer(Added, [&](SDKNode *N)
                                      { return !contains(AddedMatched, N); })) {
      RenameRight.push_back(Remain);
    }

    BestMatchMatcher RenameMatcher(RenameLeft, RenameRight, isRename,
                                   isBetterMatch, *this);
    RenameMatcher.match();
    // Rename detection ends.

    handleUnmatch(RemovedMatched, Removed, true);
    handleUnmatch(AddedMatched, Added, false);
  }
};

// Given two NodeVector, this matches SDKNode by the their names; only Nodes with
// the identical names will be matched. We use this in name-sensitive but
// order-insensitive cases, such as matching types in a module.
class SameNameNodeMatcher : public NodeMatcher {
  NodeVector &Left;
  NodeVector &Right;
  MatchedNodeListener &Listener;

  enum class NameMatchKind {
    USR,
    PrintedName,
    PrintedNameAndUSR,
  };

  // Given two sdk nodes, figure out the reason for why they have the same name.
  Optional<NameMatchKind> getNameMatchKind(SDKNode *L, SDKNode *R) {
    if (L->getKind() != R->getKind())
      return None;
    auto *LD = L->getAs<SDKNodeDecl>();
    auto *RD = R->getAs<SDKNodeDecl>();
    assert(LD && RD);
    auto NameEqual = LD->getPrintedName() == RD->getPrintedName();
    auto UsrEqual = LD->getUsr() == RD->getUsr();
    if (NameEqual && UsrEqual)
      return NameMatchKind::PrintedNameAndUSR;
    else if (NameEqual)
      return NameMatchKind::PrintedName;
    else if (UsrEqual)
      return NameMatchKind::USR;
    else
      return None;
  }

  struct NameMatchCandidate {
    SDKNode *Node;
    NameMatchKind Kind;
  };

  // Get the priority for the favored name match kind. Favored name match kind
  // locats before less favored ones.
  ArrayRef<NameMatchKind> getNameMatchKindPriority(SDKNodeKind Kind) {
    if (Kind == SDKNodeKind::Function) {
      static NameMatchKind FuncPriority[] = { NameMatchKind::PrintedNameAndUSR,
                                              NameMatchKind::USR,
                                              NameMatchKind::PrintedName };
      return FuncPriority;
    } else {
      static NameMatchKind OtherPriority[] = { NameMatchKind::PrintedNameAndUSR,
                                               NameMatchKind::PrintedName };
      return OtherPriority;
    }
  }

  // Given a list and a priority, find the best matched candidate sdk node.
  SDKNode* findBestNameMatch(ArrayRef<NameMatchCandidate> Candidates,
                             ArrayRef<NameMatchKind> Kinds) {
    for (auto Kind : Kinds)
      for (auto &Can : Candidates)
        if (Kind == Can.Kind)
          return Can.Node;
    return nullptr;
  }

public:
  SameNameNodeMatcher(NodeVector &Left, NodeVector &Right,
                      MatchedNodeListener &Listener) : Left(Left), Right(Right),
                                                       Listener(Listener) {}
  void match() override ;
};

void SameNameNodeMatcher::match() {
  NodeVector MatchedRight;
  NodeVector Removed;
  NodeVector Added;

  for (auto *LN : Left) {

    // This collects all the candidates that can match with LN.
    std::vector<NameMatchCandidate> Candidates;
    for (auto *RN : Right) {

      // If RN has matched before, ignore it.
      if (contains(MatchedRight, RN))
        continue;

      // If LN and RN have the same name for some reason, keep track of RN.
      if (auto Kind = getNameMatchKind(LN, RN))
        Candidates.push_back({RN, Kind.getValue()});
    }

    // Try to find the best match among all the candidates by the priority name
    // match kind list.
    if (auto Match = findBestNameMatch(Candidates,
                                    getNameMatchKindPriority(LN->getKind()))) {
      Listener.foundMatch(LN, Match);
      MatchedRight.push_back(Match);
    } else {
      Removed.push_back(LN);
    }
  }
  for (auto &R : Right) {
    if (!contains(MatchedRight, R)) {
      Added.push_back(R);
    }
  }
  RemovedAddedNodeMatcher(Removed, Added, Listener).match();
}

// The recursive version of sequential matcher. We do not only match two vectors
// of NodePtr but also their descendents.
class SequentialRecursiveMatcher : public NodeMatcher {
  NodePtr &Left;
  NodePtr &Right;
  MatchedNodeListener &Listener;

  void matchInternal(NodePtr L, NodePtr R) {
    Listener.foundMatch(L, R);
    if (!L || !R)
      return;
    for (unsigned I = 0; I < std::max(L->getChildrenCount(),
                                      R->getChildrenCount()); ++ I) {
      auto Left = I < L->getChildrenCount() ? L->childAt(I) : nullptr;
      auto Right = I < R->getChildrenCount() ? R->childAt(I): nullptr;
      matchInternal(Left, Right);
    }
  }

public:
  SequentialRecursiveMatcher(NodePtr &Left, NodePtr &Right,
                             MatchedNodeListener &Listener) : Left(Left),
                              Right(Right), Listener(Listener) {}
  virtual void match() override {
    matchInternal(Left, Right);
  }
};


// This is the interface of all passes on the given trees rooted at Left and Right.
class SDKTreeDiffPass {
public:
  virtual void pass(NodePtr Left, NodePtr Right) = 0;
  virtual ~SDKTreeDiffPass() {}
};

using NodePairVector = std::vector<std::pair<NodePtr, NodePtr>>;

// This map keeps track of updated nodes; thus we can conveniently find out what
// is the counterpart of a node before or after being updated.
class UpdatedNodesMap : public MatchedNodeListener {
  NodePairVector MapImpl;

public:
  virtual void foundMatch(NodePtr Left, NodePtr Right) {
    assert(Left->getKind() != SDKNodeKind::Nil &&
           Right->getKind() != SDKNodeKind::Nil &&
           "Not update operation.");
    MapImpl.push_back(std::make_pair(Left, Right));
  }

  NodePtr findUpdateCounterpart(NodePtr Node) const {
    assert(Node->isAnnotatedAs(NodeAnnotation::Updated) && "Not update operation.");
    auto FoundPair = std::find_if(MapImpl.begin(), MapImpl.end(),
                        [&](std::pair<NodePtr, NodePtr> Pair) {
      return Pair.second == Node || Pair.first == Node;
    });
    assert(FoundPair != MapImpl.end() && "Cannot find update counterpart.");
    return Node == FoundPair->first ? FoundPair->second : FoundPair->first;
  }
};

void detectThrowing(NodePtr L, NodePtr R) {
  assert(L->getKind() == R->getKind());
  if (auto LF = dyn_cast<SDKNodeAbstractFunc>(L)) {
    auto RF = R->getAs<SDKNodeAbstractFunc>();
    if (!LF->isThrowing() && RF->isThrowing()) {
      LF->annotate(NodeAnnotation::NowThrowing);
    }
  }
}

static void detectRename(NodePtr L, NodePtr R) {
  assert(L->getKind() == R->getKind());
  if (isa<SDKNodeDecl>(L) && L->getPrintedName() != R->getPrintedName()) {
    L->annotate(NodeAnnotation::Rename);
    L->annotate(NodeAnnotation::RenameOldName);
    L->addAnnotateComment(NodeAnnotation::RenameOldName, L->getPrintedName());
    L->annotate(NodeAnnotation::RenameNewName);
    L->addAnnotateComment(NodeAnnotation::RenameNewName, R->getPrintedName());
  }
}

// This is first pass on two given SDKNode trees. This pass removes the common part
// of two versions of SDK, leaving only the changed part.
class PrunePass : public MatchedNodeListener, public SDKTreeDiffPass {
  static void removeCommomChildren(NodePtr Left, NodePtr Right) {
    llvm::SmallPtrSet<NodePtr, 16> LeftToRemove;
    llvm::SmallPtrSet<NodePtr, 16> RightToRemove;
    for (auto &LC : Left->getChildren()) {
      for (auto &RC : Right->getChildren()) {
        if (*LC == *RC) {
          LeftToRemove.insert(LC.get());
          RightToRemove.insert(RC.get());
          break;
        }
      }
    }
    for (NodePtr L : LeftToRemove)
      Left->removeChild(L);
    for (NodePtr R : RightToRemove)
      Right->removeChild(R);
  }

  std::unique_ptr<UpdatedNodesMap> UpdateMap;

public:
  PrunePass() : UpdateMap(new UpdatedNodesMap()) {}

  virtual void foundRemoveAddMatch(NodePtr Left, NodePtr Right) override {
    if (Left->getKind() == SDKNodeKind::Nil)
      Right->annotate(NodeAnnotation::Added);
    else if (Right->getKind() == SDKNodeKind::Nil) {
      Left->annotate(NodeAnnotation::Removed);
    } else if (Right->getKind() == Left->getKind()) {
      foundMatch(Left, Right);
    } else {
      Left->annotate(NodeAnnotation::Removed);
      Right->annotate(NodeAnnotation::Added);
    }
  }

  virtual void foundMatch(NodePtr Left, NodePtr Right) override {
    assert(Left->getKind() == Right->getKind());
    SDKNodeKind Kind = Left->getKind();
    assert(Left->getKind() != SDKNodeKind::Nil &&
           Right->getKind() != SDKNodeKind::Nil);

    Left->annotate(NodeAnnotation::Updated);
    Right->annotate(NodeAnnotation::Updated);
    // Push the updated node to the map for future reference.
    UpdateMap->foundMatch(Left, Right);
    detectRename(Left, Right);
    detectThrowing(Left, Right);

    switch(Kind) {
    case SDKNodeKind::Root:
    case SDKNodeKind::TypeDecl: {
      // If the matched nodes are both modules, remove the contained
      // type decls that are identical. If the matched nodes are both type decls,
      // remove the contained function decls that are identical.
      removeCommomChildren(Left, Right);
      NodeVector LeftChildren;
      NodeVector RightChildren;
      Left->collectChildren(LeftChildren);
      Right->collectChildren(RightChildren);
      SameNameNodeMatcher(LeftChildren, RightChildren, *this).match();
      break;
    }

    case SDKNodeKind::Function:
    case SDKNodeKind::Setter:
    case SDKNodeKind::Getter:
    case SDKNodeKind::Constructor:
    case SDKNodeKind::TypeAlias:
    case SDKNodeKind::TypeFunc:
    case SDKNodeKind::TypeNominal: {
      // If mactched nodes are both function/var/TypeAlias decls, mapping their
      // parameters sequentially.
      SequentialNodeMatcher(Left->getChildren(),
                            Right->getChildren(), *this).match();
      break;
    }

    case SDKNodeKind::Var: {
      auto LC = Left->getChildren()[0].get();
      auto RC = Right->getChildren()[0].get();
      if (!(*LC == *RC))
        foundMatch(LC, RC);
      break;
    }

    case SDKNodeKind::Nil: {
      llvm_unreachable("nil node cannot be matched.");
    }
    }
  }

  virtual void pass(NodePtr Left, NodePtr Right) override {
    foundMatch(Left, Right);
  }

  std::unique_ptr<UpdatedNodesMap> getNodeUpdateMap() {
    return std::move(UpdateMap);
  }
};

// For a given SDK node tree, this will build up a mapping from USR to node
using USRToNodeMap = llvm::StringMap<NodePtr, llvm::BumpPtrAllocator>;

// Class to build up mappings from USR to SDKNode
class MapUSRToNode : public SDKNodeVisitor {
  friend class SDKNode; // for visit()
  USRToNodeMap usrMap;

  virtual void visit(NodePtr ptr) override {
    if (auto D = dyn_cast<SDKNodeDecl>(ptr)) {
      usrMap[D->getUsr()] = ptr;
    }
  }

public:
  MapUSRToNode() = default;

  const USRToNodeMap &getMap() const { return usrMap; }

  void map(NodePtr ptr) {
    SDKNode::preorderVisit(ptr, *this);
  }

  void dump(llvm::raw_ostream &) const;
  void dump() const { dump(llvm::errs()); }

private:
  MapUSRToNode(MapUSRToNode &) = delete;
  MapUSRToNode &operator=(MapUSRToNode &) = delete;
};

// Class to build up a diff of structurally different nodes, based on the given
// USR map for the left (original) side of the diff, based on parent types.
class TypeMemberDiffFinder : public SDKNodeVisitor {
  friend class SDKNode; // for visit()

  const USRToNodeMap &diffAgainst;

  // Vector of {givenNodePtr, diffAgainstPtr}
  NodePairVector TypeMemberDiffs;

  virtual void visit(NodePtr node) override {
    // Skip nodes that we don't have a correlate for
    auto declNode = dyn_cast<SDKNodeDecl>(node);
    if (!declNode)
      return;
    auto usr = declNode->getUsr();
    auto &usrName = usr;
    if (!diffAgainst.count(usrName))
      return;

    auto diffNode = diffAgainst.lookup(usrName);
    assert(node && diffNode && "nullptr visited?");
    auto nodeParent = node->getParent();
    auto diffParent = diffNode->getParent();
    assert(nodeParent && diffParent && "trying to check Root?");

    if (nodeParent->getKind() == SDKNodeKind::TypeDecl &&
        diffParent->getKind() == SDKNodeKind::Root)
      TypeMemberDiffs.push_back({diffNode, node});
    else if (node->getKind() == SDKNodeKind::Getter &&
             diffNode->getKind() == SDKNodeKind::Function &&
             node->isNameValid()) {
      diffNode->annotate(NodeAnnotation::Rename);
      diffNode->annotate(NodeAnnotation::RenameOldName);
      diffNode->addAnnotateComment(NodeAnnotation::RenameOldName,
                                   diffNode->getPrintedName());
      diffNode->annotate(NodeAnnotation::RenameNewName);
      diffNode->addAnnotateComment(NodeAnnotation::RenameNewName,
                                   node->getParent()->getPrintedName());
    }
  }

public:
  TypeMemberDiffFinder(const USRToNodeMap &rightUSRMap)
      : diffAgainst(rightUSRMap) {}

  void findDiffsFor(NodePtr ptr) { SDKNode::preorderVisit(ptr, *this); }

  const NodePairVector &getDiffs() const {
    return TypeMemberDiffs;
  }

  void dump(llvm::raw_ostream &) const;
  void dump() const { dump(llvm::errs()); }

private:
  TypeMemberDiffFinder(const TypeMemberDiffFinder &) = delete;
  TypeMemberDiffFinder &operator=(const TypeMemberDiffFinder &) = delete;

};

typedef std::unique_ptr<NodePairVector> RenamedNodes;

// Given a condition, search whether a node satisfies that condition exists
// in a tree.
class SearchVisitor : public SDKNodeVisitor {
  bool isFound = false;
  llvm::function_ref<bool(NodePtr)> Predicate;

public:
  SearchVisitor(llvm::function_ref<bool(NodePtr)> Predicate) :
    Predicate(Predicate) {}

  virtual void visit(NodePtr Node) override {
    isFound |= Predicate(Node);
  }

  bool search(NodePtr Node) {
    SDKNode::preorderVisit(Node, *this);
    return isFound;
  }
};

class ChangeRefinementPass : public SDKTreeDiffPass, public SDKNodeVisitor {
  bool IsVisitingLeft;
  std::unique_ptr<UpdatedNodesMap> UpdateMap;

#define ANNOTATE(Node, Counter, X, Y)                                          \
  auto ToAnnotate = IsVisitingLeft ? Node : Counter;                           \
  ToAnnotate->annotate(IsVisitingLeft ? X : Y);

  bool detectWrapOptional(SDKNodeType *Node, SDKNodeType *Counter) {
    if (Node->getTypeKind() != KnownTypeKind::Optional &&
        Node->getTypeKind() != KnownTypeKind::ImplicitlyUnwrappedOptional &&
        Counter->getTypeKind() == KnownTypeKind::Optional &&
        *Node == *Counter->getOnlyChild()) {
      ANNOTATE(Node, Counter, NodeAnnotation::WrapOptional,
               NodeAnnotation::UnwrapOptional)
      return true;
    }
    return false;
  }

  bool detectWrapImplicitOptional(SDKNodeType *Node, SDKNodeType *Counter) {
    if (Node->getTypeKind() != KnownTypeKind::Optional &&
        Node->getTypeKind() != KnownTypeKind::ImplicitlyUnwrappedOptional &&
        Counter->getTypeKind() == KnownTypeKind::ImplicitlyUnwrappedOptional &&
        *Node == *Counter->getOnlyChild()) {
      ANNOTATE(Node, Counter, NodeAnnotation::WrapImplicitOptional,
               NodeAnnotation::UnwrapOptional)
      return true;
    }

    return false;
  }

  bool detectOptionalUpdate(SDKNodeType *Node, SDKNodeType *Counter) {
    if (Node->getTypeKind() == KnownTypeKind::Optional &&
        Counter->getTypeKind() == KnownTypeKind::ImplicitlyUnwrappedOptional &&
        *Node->getOnlyChild() == *Counter->getOnlyChild()) {
      ANNOTATE(Node, Counter,
               NodeAnnotation::OptionalToImplicitOptional,
               NodeAnnotation::ImplicitOptionalToOptional)
      return true;
    }
    return false;
  }

  bool detectUnmanagedUpdate(SDKNodeType *Node, SDKNodeType *Counter) {
    if (IsVisitingLeft && Node->getTypeKind() == KnownTypeKind::Unmanaged &&
        Counter->getTypeKind() != KnownTypeKind::Unmanaged &&
        *Node->getOnlyChild() == *Counter) {
      Node->annotate(NodeAnnotation::UnwrapUnmanaged);
      return true;
    }
    return false;
  }

#undef ANNOTATE

  bool detectTypeRewritten(SDKNodeType *Node, SDKNodeType *Counter) {
    if (IsVisitingLeft &&
        (Node->getName() != Counter->getName()||
        Node->getChildrenCount() != Counter->getChildrenCount())) {
      Node->annotate(NodeAnnotation::TypeRewritten);
      Node->annotate(NodeAnnotation::TypeRewrittenLeft);
      Node->annotate(NodeAnnotation::TypeRewrittenRight);
      Node->addAnnotateComment(NodeAnnotation::TypeRewrittenLeft,
                               Node->getPrintedName());
      Node->addAnnotateComment(NodeAnnotation::TypeRewrittenRight,
                               Counter->getPrintedName());
      return true;
    }
    return false;
  }

  bool isUnhandledCase(SDKNodeType *Node) {
    auto Counter = UpdateMap->findUpdateCounterpart(Node)->getAs<SDKNodeType>();
    return Node->getTypeKind() == KnownTypeKind::Void ||
           Counter->getTypeKind() == KnownTypeKind::Void;
  }

public:
  ChangeRefinementPass(std::unique_ptr<UpdatedNodesMap> UpdateMap) :
    UpdateMap(std::move(UpdateMap)) {}

  virtual void pass(NodePtr Left, NodePtr Right) override {

    // Post-order visit is necessary since we propagate annotations bottom-up
    IsVisitingLeft = true;
    SDKNode::postorderVisit(Left, *this);
    IsVisitingLeft = false;
    SDKNode::postorderVisit(Right, *this);
  }

  virtual void visit(NodePtr N) override {
    auto Node = dyn_cast<SDKNodeType>(N);
    if (!Node || !Node->isAnnotatedAs(NodeAnnotation::Updated) ||
        isUnhandledCase(Node))
      return;
    auto Counter = const_cast<SDKNodeType*>(UpdateMap->
      findUpdateCounterpart(Node)->getAs<SDKNodeType>());

    bool Result = detectWrapOptional(Node, Counter)||
                  detectOptionalUpdate(Node, Counter)||
                  detectWrapImplicitOptional(Node, Counter)||
                  detectUnmanagedUpdate(Node, Counter)||
                  detectTypeRewritten(Node, Counter);
    (void) Result;
    return;
  }

  std::unique_ptr<UpdatedNodesMap> getNodeUpdateMap() {
    return std::move(UpdateMap);
  }
};

// DiffItem describes how an element in sdk evolves in a way that migrator can
// read conveniently. Each DiffItem corresponds to one json element and contains
// sub fields explaining how migrator can assist client code to cope with such
// sdk change. For instance, the following first json element describes an unwrap
// optional change in the first parameter of function "c:@F@CTTextTabGetOptions".
// Similarly, the second json element describes a type parameter down cast in the
// second parameter of function "c:objc(cs)NSXMLDocument(im)insertChildren:atIndex:".
// We keep both usrs because in the future this may support auto-rename.
class DiffItem {
public:
  SDKNodeKind NodeKind;
  NodeAnnotation DiffKind;
  StringRef ChildIndex;
  StringRef LeftUsr;
  StringRef RightUsr;
  StringRef LeftComment;
  StringRef RightComment;
  StringRef ModuleName;

  DiffItem(SDKNodeKind NodeKind, NodeAnnotation DiffKind, StringRef ChildIndex,
           StringRef LeftUsr, StringRef RightUsr, StringRef LeftComment,
           StringRef RightComment, StringRef ModuleName) : NodeKind(NodeKind),
           DiffKind(DiffKind), ChildIndex(ChildIndex), LeftUsr(LeftUsr),
           RightUsr(RightUsr), LeftComment(LeftComment),
           RightComment(RightComment), ModuleName(ModuleName) {
    assert(!ChildIndex.empty() && "Child index is empty.");
  }

  bool operator<(DiffItem Other) const {
    if (auto UsrCompare = LeftUsr.compare(Other.LeftUsr))
      return UsrCompare < 0;
    if (NodeKind != Other.NodeKind)
      return NodeKind < Other.NodeKind;
    if (DiffKind != Other.DiffKind)
      return DiffKind < Other.DiffKind;
    if (auto ChildCompare = ChildIndex.compare(Other.ChildIndex))
      return ChildCompare < 0;
    return false;
  }

  static void describe(llvm::raw_ostream &os) {
    os << "// SDK_CHANGE(node kind, diff kind, child index, left USR, "
          "right USR, left comment, right comment)\n";
  }

  static void undef(llvm::raw_ostream &os) {
    os << "#undef SDK_CHANGE\n";
  }

  void streamDef(llvm::raw_ostream &S) const {
    S << "SDK_CHANGE(" << NodeKind << ", " << DiffKind << ", \"" << ChildIndex
      << "\", \"" << LeftUsr << "\", \"" << RightUsr << "\", \""
      << LeftComment << "\", \"" << RightComment
      << "\", \"" << ModuleName << "\")";
  }
};

typedef std::vector<DiffItem> DiffVector;

// TypeMemberDiffItem stores info about movements of functions to type members
//
// Outputs:
//
// SDK_CHANGE_TYPE_MEMBER(USR, new type context name, new printed name, self
//                        index, old printed name)
//
// Examples:
//----------------------------------------------------------------------------//
// Init:
//
//  CGAffineTransformMakeScale(_:_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGAffineTransformMakeScale",
//                         "CGAffineTransform", "init(scaleX:y:)", ,
//                         "CGAffineTransformMakeScale(_:_:)")
//
//  Meaning that source should transform like:
//  let myAffineTransform = CGAffineTransformMakeScale(myX, myY)
//    ==>
//  let myAffineTransform = CGAffineTransform(scaleX: myX, y: myY)
//
//
//----------------------------------------------------------------------------//
// Static/Class Method:
//
//  CGColorGetConstantColor(_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGColorGetConstantColor", "CGColor",
//                         "constantColor(forName:)", ,
//                         "CGColorGetConstantColor(_:)")
//
// Meaning that source should transform like:
//  CGColorGetConstantColor(nameOfWhiteColor)
//    ==>
//  CGColor.constantColor(forName: nameOfWhiteColor)
//
//
//----------------------------------------------------------------------------//
// Instance Method:
//
//  CGEventPost(_:_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGEventPost", "CGEvent", "post(tap:)", 1,
//  "CGEventPost(_:_:)")
//
// Meaning that source should transform like:
//  CGEventPost(myTap, myEvent)
//    ==>
//  myEvent.post(tap: myTap)
//
//
//----------------------------------------------------------------------------//
// Static/Class Stored Variable:
//
//  kCGColorWhite
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@kCGColorWhite", "CGColor", "white", ,
//                         "kCGColorWhite")
//
// Meaning that source should transform like:
//  let colorName = kCGColorWhite
//    ==>
//  let colorName = CGColor.white
//
//
//----------------------------------------------------------------------------//
// Instance Computed Property
//
//
//  CGColorGetComponents(_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGColorGetComponents", "CGColor",
//                         "components", 0, "CGColorGetComponents(_:)")
//
// Meaning that source should transform like:
//  CGColorGetComponents(myColor)
//    ==>
//  myColor.components
//
//
struct TypeMemberDiffItem {
  StringRef usr;
  StringRef newTypeName;
  StringRef newPrintedName;
  Optional<uint8_t> selfIndex;
  StringRef oldPrintedName;

  static void describe(llvm::raw_ostream &os) {
    os << "// SDK_CHANGE_TYPE_MEMBER(USR, new typename, new printed name, "
          "self index, old printed name)\n";
  }

  static void undef(llvm::raw_ostream &os) {
    os << "#undef SDK_CHANGE_TYPE_MEMBER\n";
  }

  void streamDef(llvm::raw_ostream &os) const {
    std::string IndexContent = selfIndex.hasValue() ?
      std::to_string(selfIndex.getValue()) : "";

    os << "SDK_CHANGE_TYPE_MEMBER("
       << "\"" << usr << "\"" << ", "
       << "\"" << newTypeName << "\"" << ", "
       << "\"" << newPrintedName << "\"" << ", "
       << "\"" << IndexContent << "\"" << ", "
       << "\"" << oldPrintedName << "\""
       << ")";
  }

  bool operator<(TypeMemberDiffItem Other) const {
    return usr.compare(Other.usr) < 0;
  }
};
typedef std::vector<TypeMemberDiffItem> TypeMemberDiffVector;

} // End namespace


static void printNode(llvm::raw_ostream &os, NodePtr node) {
  os << "{" << node->getName() << " " << node->getKind() << " "
            << node->getPrintedName();
  if (auto F = dyn_cast<SDKNodeAbstractFunc>(node)) {
    if (F->hasSelfIndex()) {
      os << " selfIndex: ";
      os << F->getSelfIndex();
    }
  }
  os << "}";
}

void MapUSRToNode::dump(llvm::raw_ostream &os) const {
  for (auto &elt : usrMap) {
    auto &node = elt.getValue();
    os << elt.getKey() << " ==> ";
    printNode(os, node);
    if (node->getParent()) {
      os << "  parent: ";
      printNode(os, node->getParent());
    }
    os << "\n";
  }
}

void TypeMemberDiffFinder::dump(llvm::raw_ostream &os) const {
  for (auto pair : getDiffs()) {
    os << " - ";
    printNode(os, pair.first);
    os << " parent: ";
    printNode(os, pair.first->getParent());

    os << "\n + ";
    printNode(os, pair.second);
    os << " parent: ";
    printNode(os, pair.second->getParent());
    os << "\n\n";
  }
}

namespace {
template<typename T>

void removeRedundantAndSort(std::vector<T> &Diffs) {
  std::set<T> DiffSet(Diffs.begin(), Diffs.end());
  Diffs.assign(DiffSet.begin(), DiffSet.end());
  std::sort(Diffs.begin(), Diffs.end());
}

template<typename T>
void serializeDiffs(llvm::raw_ostream &Fs, std::vector<T> &Diffs) {
  removeRedundantAndSort(Diffs);
  Fs << "\n";
  T::describe(Fs);
  for (auto &Diff : Diffs) {
    Diff.streamDef(Fs);
    Fs << "\n";
  }
  T::undef(Fs);
  Fs << "\n";
}

static bool isTypeChangeInterestedFuncNode(NodePtr Decl) {
  switch(Decl->getKind()) {
    case SDKNodeKind::Constructor:
    case SDKNodeKind::Function:
      return true;
    default:
      return false;
  }
}

static bool isInterested(SDKNodeDecl* Decl, NodeAnnotation Anno) {
  switch (Anno) {
    case NodeAnnotation::WrapOptional:
    case NodeAnnotation::UnwrapOptional:
    case NodeAnnotation::ImplicitOptionalToOptional:
    case NodeAnnotation::OptionalToImplicitOptional:
    case NodeAnnotation::UnwrapUnmanaged:
    case NodeAnnotation::TypeRewritten:
      return isTypeChangeInterestedFuncNode(Decl) &&
        Decl->getParent()->getKind() == SDKNodeKind::TypeDecl;
    default:
      return true;
  }
}

class DiffItemEmitter : public SDKNodeVisitor {
  DiffVector &AllItems;

  bool doesAncestorHaveTypeRewritten() {
    return std::find_if(Ancestors.begin(), Ancestors.end(),[](NodePtr N) {
      return N->isAnnotatedAs(NodeAnnotation::TypeRewritten);
    }) != Ancestors.end();
  }

  StringRef getLeftComment(NodePtr Node, NodeAnnotation Anno) {
    if (Anno == NodeAnnotation::TypeRewritten)
      return Node->getAnnotateComment(NodeAnnotation::TypeRewrittenLeft);
    else if (Anno == NodeAnnotation::Rename)
      return Node->getAnnotateComment(NodeAnnotation::RenameOldName);
    return StringRef();
  }

  StringRef getRightComment(NodePtr Node, NodeAnnotation Anno) {
    if (Anno == NodeAnnotation::TypeRewritten)
      return Node->getAnnotateComment(NodeAnnotation::TypeRewrittenRight);
    else if (Anno == NodeAnnotation::ModernizeEnum)
      return Node->getAnnotateComment(NodeAnnotation::ModernizeEnum);
    else if (Anno == NodeAnnotation::Rename)
      return Node->getAnnotateComment(NodeAnnotation::RenameNewName);
    return StringRef();
  }

  bool handleAnnotation(NodePtr Node, SDKNodeDecl *NonTypeParent,
                        StringRef Index, NodeAnnotation Annotation) {
    if (isInterested(NonTypeParent, Annotation) &&
        Node->isAnnotatedAs(Annotation)) {
      auto Kind = NonTypeParent->getKind();
      StringRef LC = getLeftComment(Node, Annotation);
      StringRef RC = getRightComment(Node, Annotation);
      AllItems.emplace_back(Kind, Annotation, Index,
                            NonTypeParent->getUsr(), StringRef(), LC, RC,
                            NonTypeParent->getModuleName());
      return true;
    }
    return false;
  }

  virtual void visit(NodePtr Node) override {
    SDKNodeDecl *Parent = dyn_cast<SDKNodeDecl>(Node);
    if (!Parent) {
      if (auto TN = dyn_cast<SDKNodeType>(Node)) {
        Parent = TN->getClosestParentDecl();
      }
    }

    if (!Parent)
      return;
    auto Index = isa<SDKNodeType>(Node) ? getIndexString(Node) : "0";

    bool Result =
      doesAncestorHaveTypeRewritten() ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::WrapOptional) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::UnwrapOptional) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::ImplicitOptionalToOptional) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::OptionalToImplicitOptional) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::UnwrapUnmanaged) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::TypeRewritten) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::SetterToProperty) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::GetterToProperty) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::ModernizeEnum) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::Rename) ||
      handleAnnotation(Node, Parent, Index, NodeAnnotation::NowThrowing);
    (void) Result;
  }

  StringRef getIndexString(NodePtr Node) {
    llvm::SmallString<32> Builder;
    std::vector<int> Indexes;
    collectIndexes(Node, Indexes);
    auto First = true;
    for (auto I : Indexes) {
      if (!First)
        Builder.append(":");
      else
        First = false;
      Builder.append(std::to_string(I));
    }
    return InsertToBuffer(Builder.str());
  }

  void collectIndexes(NodePtr Node, std::vector<int> &Indexes) {
    for (unsigned I = Ancestors.size(); I > 0 && (I == Ancestors.size() ||
        isa<SDKNodeType>(Ancestors[I])); -- I) {
      auto Child = I == Ancestors.size() ? Node : Ancestors[I];
      auto Parent = Ancestors[I - 1];
      Indexes.insert(Indexes.begin(), Parent->getChildIndex(Child));
    }
  }
  DiffItemEmitter(DiffVector &AllItems) : AllItems(AllItems) {}

public:
  static void collectDiffItems(NodePtr Root, DiffVector &DV) {
    DiffItemEmitter Emitter(DV);
    SDKNode::postorderVisit(Root, Emitter);
  }
};

class DiagnosisEmitter : public SDKNodeVisitor {
  void visitType(SDKNodeType *T);
  void visitDecl(SDKNodeDecl *D);
  void visit(NodePtr Node) override;
  SDKNodeDecl *findAddedDecl(SDKNodeDecl *Node);
  static StringRef printName(StringRef Name);
  static StringRef printDiagKeyword(StringRef Name);
  static void collectAddedDecls(NodePtr Root, std::set<SDKNodeDecl*> &Results);

  template<typename T>
  struct DiagBag {
    std::vector<T> Diags;
    ~DiagBag() {
      llvm::outs() << "\n==================================================== ";
      T::theme(llvm::outs());
      llvm::outs() << " ====================================================\n";
      removeRedundantAndSort(Diags);
      std::for_each(Diags.begin(), Diags.end(), [](T &Diag) {
        Diag.output();
      });
    }
  };

  struct RemovedDeclDiag {
    DeclKind Kind;
    StringRef Name;
    bool IsDeprecated;
    RemovedDeclDiag(DeclKind Kind, StringRef Name, bool IsDeprecated) :
      Kind(Kind), Name(Name), IsDeprecated(IsDeprecated) {}
    bool operator<(RemovedDeclDiag Other) const;
    void output() const;
    static void theme(raw_ostream &OS) { OS << "Removed Decls"; };
  };

  struct MovedDeclDiag {
    DeclKind RemovedKind;
    DeclKind AddedKind;
    StringRef RemovedName;
    StringRef AddedName;
    MovedDeclDiag(DeclKind RemovedKind, DeclKind AddedKind,
                  StringRef RemovedName, StringRef AddedName) :
      RemovedKind(RemovedKind), AddedKind(AddedKind), RemovedName(RemovedName),
      AddedName(AddedName) {}
    bool operator<(MovedDeclDiag other) const;
    void output() const;
    static void theme(raw_ostream &OS) { OS << "Moved Decls"; };
  };

  struct RenamedDeclDiag {
    DeclKind KindBefore;
    DeclKind KindAfter;
    StringRef NameBefore;
    StringRef NameAfter;
    RenamedDeclDiag(DeclKind KindBefore, DeclKind KindAfter,
                    StringRef NameBefore, StringRef NameAfter) :
                      KindBefore(KindBefore), KindAfter(KindAfter),
                      NameBefore(NameBefore), NameAfter(NameAfter) {}
    bool operator<(RenamedDeclDiag Other) const;
    void output() const;
    static void theme(raw_ostream &OS) { OS << "Renamed Decls"; };
  };

  struct DeclTypeChangeDiag {
    DeclKind Kind;
    StringRef DeclName;
    StringRef TypeNameBefore;
    StringRef TypeNameAfter;
    StringRef Description;
    DeclTypeChangeDiag(DeclKind Kind, StringRef DeclName,
                       StringRef TypeNameBefore, StringRef TypeNameAfter,
                       StringRef Description) :
      Kind(Kind), DeclName(DeclName), TypeNameBefore(TypeNameBefore),
      TypeNameAfter(TypeNameAfter), Description(Description) {}
    bool operator<(DeclTypeChangeDiag Other) const;
    void output() const;
    static void theme(raw_ostream &OS) { OS << "Type Changes"; };
  };

  std::set<SDKNodeDecl*> AddedDecls;
  DiagBag<DeclTypeChangeDiag> TypeChangedDecls;
  DiagBag<RenamedDeclDiag> RenamedDecls;
  DiagBag<MovedDeclDiag> MovedDecls;
  DiagBag<RemovedDeclDiag> RemovedDecls;

  UpdatedNodesMap UpdateMap;
  DiagnosisEmitter(UpdatedNodesMap &UpdateMap) : UpdateMap(UpdateMap) {}
public:
  static void diagnosis(NodePtr LeftRoot, NodePtr RightRoot,
                        UpdatedNodesMap &UpdateMap);
};

void DiagnosisEmitter::collectAddedDecls(NodePtr Root,
                                         std::set<SDKNodeDecl*> &Results) {
  if (auto *D = dyn_cast<SDKNodeDecl>(Root)) {
    if (Root->isAnnotatedAs(NodeAnnotation::Added))
      Results.insert(D);
  }
  for (auto &C : Root->getChildren())
    collectAddedDecls(C.get(), Results);
}

SDKNodeDecl *DiagnosisEmitter::findAddedDecl(SDKNodeDecl *Root) {
  for (auto *Added : AddedDecls) {
    if (Root->getKind() == Added->getKind() &&
        Root->getPrintedName() == Added->getPrintedName())
      return Added;
  }
  return nullptr;
}

StringRef DiagnosisEmitter::printName(StringRef Name) {
  OSColor Color(llvm::outs(), llvm::raw_ostream::CYAN);
  Color << Name;
  return StringRef();
}

StringRef DiagnosisEmitter::printDiagKeyword(StringRef Name) {
  OSColor Color(llvm::outs(), llvm::raw_ostream::YELLOW);
  Color << Name;
  return StringRef();
}

bool DiagnosisEmitter::RemovedDeclDiag::
operator<(RemovedDeclDiag Other) const {
  if (Kind != Other.Kind)
    return Kind < Other.Kind;
  return Name.compare(Other.Name) < 0;
}

void DiagnosisEmitter::RemovedDeclDiag::output() const {
  llvm::outs() << Kind << " " << printName(Name) << " has been "
    << printDiagKeyword("removed");
  if (IsDeprecated)
    llvm::outs() << " (deprecated)";
  llvm::outs() << "\n";
}

bool DiagnosisEmitter::MovedDeclDiag::
operator<(MovedDeclDiag Other) const {
  if (RemovedKind != Other.RemovedKind)
    return RemovedKind < Other.RemovedKind;
  return RemovedName.compare(Other.RemovedName) < 0;
}

void DiagnosisEmitter::MovedDeclDiag::output() const {
  llvm::outs() << RemovedKind << " " << printName(RemovedName) << " has been "
    << printDiagKeyword("moved") << " to " << AddedKind << " "
    << printName(AddedName) << "\n";
}

bool DiagnosisEmitter::RenamedDeclDiag::
operator<(RenamedDeclDiag Other) const {
  if (KindBefore != Other.KindBefore)
    return KindBefore < Other.KindBefore;
  return NameBefore.compare(Other.NameBefore) < 0;
}

void DiagnosisEmitter::RenamedDeclDiag::output() const {
  llvm::outs() << KindBefore << " " << printName(NameBefore)
               << " has been " << printDiagKeyword("renamed") << " to "
               << KindAfter << " " << printName(NameAfter) << "\n";
}

bool DiagnosisEmitter::DeclTypeChangeDiag::
operator<(DeclTypeChangeDiag Other) const {
  if (Kind != Other.Kind)
    return Kind < Other.Kind;
  return DeclName.compare(Other.DeclName) < 0;
}

void DiagnosisEmitter::DeclTypeChangeDiag::output() const {
  llvm::outs() << Kind << " " << printName(DeclName) << " has "
               << Description << " type change from "
               << printName(TypeNameBefore) << " to "
               << printName(TypeNameAfter) << "\n";
}

void DiagnosisEmitter::diagnosis(NodePtr LeftRoot, NodePtr RightRoot,
                                 UpdatedNodesMap &UpdateMap) {
  DiagnosisEmitter Emitter(UpdateMap);
  collectAddedDecls(RightRoot, Emitter.AddedDecls);
  SDKNode::postorderVisit(LeftRoot, Emitter);
}

void DiagnosisEmitter::visitDecl(SDKNodeDecl *Node) {
  if (Node->isSDKPrivate())
    return;
  if (Node->isAnnotatedAs(NodeAnnotation::Removed) &&
      !Node->isAnnotatedAs(NodeAnnotation::Rename)) {
    if (auto *Added = findAddedDecl(Node)) {
      MovedDecls.Diags.emplace_back(Node->getDeclKind(),
                                    Added->getDeclKind(),
                                    Node->getFullyQualifiedName(),
                                    Added->getFullyQualifiedName());
    } else {
      RemovedDecls.Diags.emplace_back(Node->getDeclKind(),
                                      Node->getFullyQualifiedName(),
                                      Node->isDeprecated());
    }
  }
  if (Node->isAnnotatedAs(NodeAnnotation::Rename)) {
    auto *Count = UpdateMap.findUpdateCounterpart(Node)->getAs<SDKNodeDecl>();
    RenamedDecls.Diags.emplace_back(Node->getDeclKind(), Count->getDeclKind(),
                                    Node->getFullyQualifiedName(),
                                    Count->getFullyQualifiedName());
  }
}
void DiagnosisEmitter::visitType(SDKNodeType *Node) {
  auto *Parent = Node->getParent()->getAs<SDKNodeDecl>();
  if (!Parent || Parent->isSDKPrivate())
    return;

  if (Node->isAnnotatedAs(NodeAnnotation::Updated)) {
    auto *Count = UpdateMap.findUpdateCounterpart(Node)->getAs<SDKNodeType>();
    StringRef Descriptor;
    switch (Parent->getKind()) {
    case SDKNodeKind::Function:
    case SDKNodeKind::Var:
      Descriptor = Parent->getKind() == SDKNodeKind::Function ?
        SDKNodeFunction::getTypeRoleDescription(Parent->getChildIndex(Node)) :
        InsertToBuffer("declared");
      TypeChangedDecls.Diags.emplace_back(Parent->getDeclKind(),
                                          Parent->getFullyQualifiedName(),
                                          Node->getPrintedName(),
                                          Count->getPrintedName(),
                                          Descriptor);
      break;
    default:
      break;
    }
  }
}

void DiagnosisEmitter::visit(NodePtr Node) {
  if (auto *DNode = dyn_cast<SDKNodeDecl>(Node)) {
    visitDecl(DNode);
  }
  if (auto *TNode = dyn_cast<SDKNodeType>(Node)) {
    visitType(TNode);
  }
}

struct NoEscapeFuncParam {
  StringRef Usr;
  unsigned Index;

  NoEscapeFuncParam(StringRef Usr, unsigned Index) : Usr(Usr), Index(Index) {}

  static void describe(llvm::raw_ostream &os) {
    os << "// NOESCAPE_FUNC_PARAM(USR, Index)\n";
  }

  static void undef(llvm::raw_ostream &os) {
    os << "#undef NOESCAPE_FUNC_PARAM\n";
  }

  void streamDef(llvm::raw_ostream &os) const {
    os << "NOESCAPE_FUNC_PARAM("
    << "\"" << Usr << "\"" << ", "
    << "\"" << Index << "\"" << ")";
  }

  bool operator<(NoEscapeFuncParam Other) const {
    if (Usr != Other.Usr)
      return Usr.compare(Other.Usr) < 0;
    return Index < Other.Index;
  }
};

  typedef std::vector<NoEscapeFuncParam> NoEscapeFuncParamVector;

class NoEscapingFuncEmitter : public SDKNodeVisitor {
  NoEscapeFuncParamVector &AllItems;
  NoEscapingFuncEmitter(NoEscapeFuncParamVector &AllItems) : AllItems(AllItems) {}

  virtual void visit(NodePtr Node) override {
    if (Node->getKind() != SDKNodeKind::TypeFunc)
      return;
    if (Node->getAs<SDKNodeTypeFunc>()->isEscaping())
      return;
    auto Parent = Node->getParent();
    if (auto ParentFunc = dyn_cast<SDKNodeAbstractFunc>(Parent)) {
      if (ParentFunc->isObjc()) {
        unsigned Index = ParentFunc->getChildIndex(Node);
        AllItems.emplace_back(ParentFunc->getUsr(), Index);
      }
    }
  }

public:
  static void collectDiffItems(NodePtr Root, NoEscapeFuncParamVector &DV) {
    NoEscapingFuncEmitter Emitter(DV);
    SDKNode::postorderVisit(Root, Emitter);
  }
};

/// This info is about functions meet the following criteria:
///   - This function is a member function of a type.
///   - This function is overloaded.
struct OverloadedFuncInfo {
  StringRef Usr;
  OverloadedFuncInfo(StringRef Usr) : Usr(Usr) {}

  static void describe(llvm::raw_ostream &os) {
    os << "// OVERLOAD_FUNC_TRAILING_CLOSURE(USR)\n";
  }

  static void undef(llvm::raw_ostream &os) {
    os << "#undef OVERLOAD_FUNC_TRAILING_CLOSURE\n";
  }

  void streamDef(llvm::raw_ostream &os) const {
    os << "OVERLOAD_FUNC_TRAILING_CLOSURE("
       << "\"" << Usr << "\"" << ")";
  }

  bool operator<(OverloadedFuncInfo Other) const {
    return Usr.compare(Other.Usr) < 0;
  }
};

/// This provides a utility to view a printed name by parsing the components
/// of that name. The components include a base name and an array of argument
/// labels.
class DeclNameViewer {
  StringRef BaseName;
  SmallVector<StringRef, 4> Labels;

public:
  DeclNameViewer(StringRef Text) {
    auto ArgStart = Text.find_first_of('(');
    if (ArgStart == StringRef::npos) {
      BaseName = Text;
      return;
    }
    BaseName = Text.substr(0, ArgStart);
    auto ArgEnd = Text.find_last_of(')');
    assert(ArgEnd != StringRef::npos);
    StringRef AllArgs = Text.substr(ArgStart + 1, ArgEnd - ArgStart - 1);
    AllArgs.split(Labels, ":");
    if (Labels.empty())
      return;
    assert(Labels.back().empty());
    Labels.pop_back();
  }

  StringRef base() const { return BaseName; }

  llvm::ArrayRef<StringRef> args() const { return llvm::makeArrayRef(Labels); }

  unsigned partsCount() const { return 1 + Labels.size(); }

  unsigned commonParts(DeclNameViewer &Other) const {
    if (base() != Other.base())
      return 0;
    unsigned Result = 1;
    unsigned Len = std::min(args().size(), Other.args().size());
    for (unsigned I = 0; I < Len; ++ I) {
      if (args()[I] == Other.args()[I])
        Result ++;
      else
        return Result;
    }
    return Result;
  }
};

class OverloadMemberFunctionEmitter : public SDKNodeVisitor {

  std::vector<OverloadedFuncInfo> &AllItems;

  virtual void visit(NodePtr Node) override {
    if (Node->getKind() != SDKNodeKind::Function)
      return;
    auto Parent = Node->getParent();
    if (Parent->getKind() != SDKNodeKind::TypeDecl)
      return;
    DeclNameViewer CurrentViewer(Node->getPrintedName());
    if (CurrentViewer.args().empty())
      return;
    for (auto &C : Parent->getChildren()) {
      if (C.get() == Node)
        continue;
      if (C->getKind() != SDKNodeKind::Function)
        continue;
      DeclNameViewer ChildViewer(C->getPrintedName());
      if (ChildViewer.args().empty())
        continue;
      if (CurrentViewer.commonParts(ChildViewer) >=
          CurrentViewer.partsCount() - 1) {
        AllItems.emplace_back(Node->getAs<SDKNodeDecl>()->getUsr());
        return;
      }
    }
  }

  OverloadMemberFunctionEmitter(std::vector<OverloadedFuncInfo> &AllItems) :
    AllItems(AllItems) {}

public:
  static void collectDiffItems(NodePtr Root,
                               std::vector<OverloadedFuncInfo> &AllItems) {
    OverloadMemberFunctionEmitter Emitter(AllItems);
    SDKNode::postorderVisit(Root, Emitter);
  }
};

}// End of anonymous namespace

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

static StringRef constructFullTypeName(NodePtr Node) {
  assert(Node->getKind() == SDKNodeKind::TypeDecl);
  std::vector<NodePtr> TypeChain;
  for (auto C = Node; C->getKind() == SDKNodeKind::TypeDecl; C = C->getParent()) {
    TypeChain.insert(TypeChain.begin(), C);
  }
  assert(TypeChain.front()->getParent()->getKind() == SDKNodeKind::Root);
  llvm::SmallString<64> Buffer;
  bool First = true;
  for (auto N : TypeChain) {
    if (First) {
      First = false;
    } else {
      Buffer.append(".");
    }
    Buffer.append(N->getName());
  }
  return InsertToBuffer(Buffer.str());
}

struct RenameDetectorForMemberDiff : public MatchedNodeListener {
  void foundMatch(NodePtr Left, NodePtr Right) {
    detectRename(Left, Right);
  }
  void workOn(NodePtr Left, NodePtr Right) {
    if (Left->getKind() == Right->getKind() &&
        Left->getKind() == SDKNodeKind::TypeDecl) {
      NodeVector LeftChildren;
      NodeVector RightChildren;
      Left->collectChildren(LeftChildren);
      Right->collectChildren(RightChildren);
      SameNameNodeMatcher Matcher(LeftChildren, RightChildren, *this);
      Matcher.match();
    }
  }
};

static Optional<uint8_t> findSelfIndex(SDKNode* Node) {
  if (auto func = dyn_cast<SDKNodeAbstractFunc>(Node)) {
    return func->getSelfIndexOptional();
  } else if (auto vd = dyn_cast<SDKNodeVar>(Node)) {
    for (auto &C : vd->getChildren()) {
      if (isa<SDKNodeAbstractFunc>(C.get())) {
        if (auto Result = findSelfIndex(C.get()))
          return Result;
      }
    }
  }
  return None;
}

/// Find cases where a diff is due to a change to being a type member
static void findTypeMemberDiffs(NodePtr leftSDKRoot, NodePtr rightSDKRoot,
                                TypeMemberDiffVector &out) {
  // Mapping from USR to SDKNode
  MapUSRToNode leftMapper;
  leftMapper.map(leftSDKRoot);
  auto &leftMap = leftMapper.getMap();

  TypeMemberDiffFinder diffFinder(leftMap);
  diffFinder.findDiffsFor(rightSDKRoot);
  RenameDetectorForMemberDiff Detector;
  for (auto pair : diffFinder.getDiffs()) {
    auto left = pair.first;
    auto right = pair.second;
    auto rightParent = right->getParent();

    // SDK_CHANGE_TYPE_MEMBER(USR, new type context name, new printed name, self
    // index, old printed name)
    TypeMemberDiffItem item = {
        right->getAs<SDKNodeDecl>()->getUsr(), constructFullTypeName(rightParent),
        right->getPrintedName(), findSelfIndex(right), left->getPrintedName()};
    out.emplace_back(item);
    Detector.workOn(left, right);
  }
}

static int diagnoseModuleChange(StringRef LeftPath, StringRef RightPath) {
  if (!fs::exists(LeftPath)) {
    llvm::errs() << LeftPath << " does not exist\n";
    return 1;
  }
  if (!fs::exists(RightPath)) {
    llvm::errs() << RightPath << " does not exist\n";
    return 1;
  }
  SwiftDeclCollector LeftCollector;
  LeftCollector.deSerialize(LeftPath);
  SwiftDeclCollector RightCollector;
  RightCollector.deSerialize(RightPath);
  auto LeftModule = LeftCollector.getSDKRoot();
  auto RightModule = RightCollector.getSDKRoot();
  PrunePass Prune;
  Prune.pass(LeftModule, RightModule);
  ChangeRefinementPass RefinementPass(Prune.getNodeUpdateMap());
  RefinementPass.pass(LeftModule, RightModule);
  DiagnosisEmitter::diagnosis(LeftModule, RightModule,
                              *RefinementPass.getNodeUpdateMap());
  return 0;
}

static int compareSDKs(StringRef LeftPath, StringRef RightPath,
                       StringRef DiffPath,
                       llvm::StringSet<> &IgnoredRemoveUsrs) {
  if (!fs::exists(LeftPath)) {
    llvm::errs() << LeftPath << " does not exist\n";
    return 1;
  }
  if (!fs::exists(RightPath)) {
    llvm::errs() << RightPath << " does not exist\n";
    return 1;
  }
  llvm::errs() << "Diffing: " << LeftPath << " and " << RightPath << "\n";
  SwiftDeclCollector LeftCollector;
  LeftCollector.deSerialize(LeftPath);
  SwiftDeclCollector RightCollector;
  RightCollector.deSerialize(RightPath);
  llvm::errs() << "Finished deserializing" << "\n";
  auto LeftModule = LeftCollector.getSDKRoot();
  auto RightModule = RightCollector.getSDKRoot();

  // Structural diffs: not merely name changes but changes in SDK tree
  // structure.
  llvm::errs() << "Detecting type member diffs" << "\n";
  TypeMemberDiffVector typeMemberDiffs;
  findTypeMemberDiffs(LeftModule, RightModule, typeMemberDiffs);

  PrunePass Prune;
  Prune.pass(LeftModule, RightModule);
  llvm::errs() << "Finished pruning" << "\n";
  ChangeRefinementPass RefinementPass(Prune.getNodeUpdateMap());
  RefinementPass.pass(LeftModule, RightModule);
  DiffVector AllItems;
  DiffItemEmitter::collectDiffItems(LeftModule, AllItems);
  AllItems.erase(std::remove_if(AllItems.begin(), AllItems.end(), [&](DiffItem &Item) {
    return Item.DiffKind == NodeAnnotation::RemovedDecl &&
      IgnoredRemoveUsrs.find(Item.LeftUsr) != IgnoredRemoveUsrs.end();
  }), AllItems.end());

  NoEscapeFuncParamVector AllNoEscapingFuncs;
  NoEscapingFuncEmitter::collectDiffItems(RightModule, AllNoEscapingFuncs);

  llvm::errs() << "Dumping diff to " << DiffPath << '\n';
  std::vector<OverloadedFuncInfo> Overloads;
  OverloadMemberFunctionEmitter::collectDiffItems(RightModule, Overloads);

  std::error_code EC;
  llvm::raw_fd_ostream Fs(DiffPath, EC, llvm::sys::fs::F_None);

  serializeDiffs(Fs, AllItems);
  serializeDiffs(Fs, typeMemberDiffs);
  serializeDiffs(Fs, AllNoEscapingFuncs);
  serializeDiffs(Fs, Overloads);
  return 0;
}

static std::string getDumpFilePath(StringRef OutputDir, StringRef FileName) {
  std::string Path = OutputDir;
  Path += "/";
  Path += FileName;
  int Suffix = 0;
  auto ConstructPath = [&]() {
    return Path + (Suffix == 0 ? "" : std::to_string(Suffix)) + ".js";
  };
  for (; fs::exists(ConstructPath()); Suffix ++);
  return ConstructPath();
}

static int dumpSwiftModules(const CompilerInvocation &InitInvok,
                            const llvm::StringSet<> &ModuleNames,
                            StringRef OutputDir,
                            const std::vector<std::string> PrintApis) {
  if (!fs::exists(OutputDir)) {
    llvm::errs() << "Output directory '" << OutputDir << "' does not exist.\n";
    return 1;
  }

  std::vector<Module*> Modules;
  CompilerInvocation Invocation(InitInvok);
  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);
  if (CI.setup(Invocation)) {
    llvm::errs() << "Failed to setup the compiler instance\n";
    return 1;
  }

  auto &Context = CI.getASTContext();

  for (auto &Entry : ModuleNames) {
    StringRef Name = Entry.first();
    if (options::Verbose)
      llvm::errs() << "Loading module: " << Name << "...\n";
    auto *M = Context.getModuleByName(Name);
    if (!M) {
      if (options::Verbose)
        llvm::errs() << "Failed to load module: " << Name << '\n';
      if (options::AbortOnModuleLoadFailure)
        return 1;
    }
    Modules.push_back(M);
  }

  for (auto M : Modules) {
    SwiftDeclCollector Collector;
    SmallVector<Decl*, 256> Decls;
    M->getTopLevelDecls(Decls);
    for (auto D : Decls) {
      if (auto VD = dyn_cast<ValueDecl>(D))
        Collector.foundDecl(VD, DeclVisibilityKind::VisibleAtTopLevel);
    }
    std::string Path = getDumpFilePath(OutputDir, M->getName().str());
    Collector.serialize(Path);
    if (options::Verbose)
      llvm::errs() << "Dumped to "<< Path << "\n";
  }
  return 0;
}

static int dumpSDKContent(const CompilerInvocation &InitInvok,
                          const llvm::StringSet<> &ModuleNames,
                          StringRef OutputFile) {
  CompilerInvocation Invocation(InitInvok);

  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);
  if (CI.setup(Invocation)) {
    llvm::errs() << "Failed to setup the compiler instance\n";
    return 1;
  }

  auto &Ctx = CI.getASTContext();


  // Load standard library so that Clang importer can use it.
  auto *Stdlib = Ctx.getStdlibModule(/*loadIfAbsent=*/true);
  if (!Stdlib) {
    llvm::errs() << "Failed to load Swift stdlib\n";
    return 1;
  }

  std::vector<Module *> Modules;
  for (auto &Entry : ModuleNames) {
    StringRef Name = Entry.getKey();
    if (options::Verbose)
      llvm::errs() << "Loading module: " << Name << "...\n";
    auto *M = Ctx.getModuleByName(Name);
    if (!M) {
      llvm::errs() << "Failed to load module: " << Name << '\n';
      if (options::AbortOnModuleLoadFailure)
        return 1;
    } else {
      Modules.push_back(M);
    }
  }
  if (options::Verbose)
    llvm::errs() << "Scanning symbols...\n";
  SwiftDeclCollector Collector;
  Collector.lookupVisibleDecls(Modules);
  if (options::Verbose)
    llvm::errs() << "Dumping SDK...\n";
  Collector.serialize(OutputFile);
  if (options::Verbose)
    llvm::errs() << "Dumped to "<< OutputFile << "\n";
  return 0;
}

static int readFileLineByLine(StringRef Path, llvm::StringSet<> &Lines) {
  auto FileBufOrErr = llvm::MemoryBuffer::getFile(Path);
  if (!FileBufOrErr) {
    llvm::errs() << "error opening file: "
      << FileBufOrErr.getError().message() << '\n';
    return 1;
  }

  StringRef BufferText = FileBufOrErr.get()->getBuffer();
  while (!BufferText.empty()) {
    StringRef Line;
    std::tie(Line, BufferText) = BufferText.split('\n');
    Line = Line.trim();
    if (!Line.empty())
      Lines.insert(Line);
  }
  return 0;
}

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// getMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement getMainExecutable
// without being given the address of a function in the main executable).
void anchorForGetMainExecutable() {}

static int prepareForDump(const char *Main,
                          CompilerInvocation &InitInvok,
                          llvm::StringSet<> &Modules) {
  InitInvok.setMainExecutablePath(fs::getMainExecutable(Main,
    reinterpret_cast<void *>(&anchorForGetMainExecutable)));
  InitInvok.setModuleName("swift_ide_test");
  if (!options::SDK.empty()) {
    InitInvok.setSDKPath(options::SDK);
  } else if (const char *SDKROOT = getenv("SDKROOT")) {
    InitInvok.setSDKPath(SDKROOT);
  } else {
    llvm::errs() << "Provide '-sdk <path>' option or run with 'xcrun -sdk <..>\
    swift-api-digester'\n";
    return 1;
  }

  if (!options::Triple.empty())
    InitInvok.setTargetTriple(options::Triple);
  InitInvok.getClangImporterOptions().ModuleCachePath =
  options::ModuleCachePath;

  if (!options::SwiftVersion.empty()) {
    if (auto Version = version::Version::
        parseVersionString(options::SwiftVersion, SourceLoc(), nullptr)) {
      if (Version.getValue().isValidEffectiveLanguageVersion())
        InitInvok.getLangOptions().EffectiveLanguageVersion = Version.getValue();
      else {
        llvm::errs() << "Unsupported Swift Version.\n";
        return 1;
      }
    }
  }

  if (!options::ResourceDir.empty()) {
    InitInvok.setRuntimeResourcePath(options::ResourceDir);
  }
  InitInvok.setFrameworkSearchPaths(options::FrameworkPaths);

  if (!options::ModuleList.empty()) {
    if (readFileLineByLine(options::ModuleList, Modules))
        return 1;
  }

  for (auto M : options::ModuleNames) {
    Modules.insert(M);
  }

  if (Modules.empty()) {
    llvm::errs() << "Need to specifiy -include-all or -module <name>\n";
    return 1;
  }
  return 0;
}

static void readIgnoredUsrs(llvm::StringSet<> &IgoredUsrs) {
  StringRef Path = options::IgnoreRemovedDeclUSRs;
  if (Path.empty())
    return;
  if (!fs::exists(Path)) {
    llvm::errs() << Path << " does not exist.\n";
    return;
  }
  readFileLineByLine(Path, IgoredUsrs);
}

int main(int argc, char *argv[]) {
  INITIALIZE_LLVM(argc, argv);

  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift SDK Digester\n");
  CompilerInvocation InitInvok;

  ClangImporterOptions &ImporterOpts = InitInvok.getClangImporterOptions();
  ImporterOpts.DetailedPreprocessingRecord = true;

  llvm::StringSet<> Modules;
  std::vector<std::string> PrintApis;
  llvm::StringSet<> IgnoredUsrs;
  readIgnoredUsrs(IgnoredUsrs);
  for (auto Name : options::ApisPrintUsrs)
    PrintApis.push_back(Name);
  switch (options::Action) {
  case ActionType::DumpSwiftModules:
    return (prepareForDump(argv[0], InitInvok, Modules)) ? 1 :
      dumpSwiftModules(InitInvok, Modules, options::OutputFile, PrintApis);
  case ActionType::DumpSDK:
    return (prepareForDump(argv[0], InitInvok, Modules)) ? 1 :
      dumpSDKContent(InitInvok, Modules, options::OutputFile);
  case ActionType::CompareSDKs:
  case ActionType::DiagnoseSDKs:
    if (options::SDKJsonPaths.size() != 2) {
      llvm::errs() << "Only two SDK versions can be compared\n";
      llvm::cl::PrintHelpMessage();
      return 1;
    }
    if (options::Action == ActionType::CompareSDKs)
      return compareSDKs(options::SDKJsonPaths[0], options::SDKJsonPaths[1],
                         options::OutputFile, IgnoredUsrs);
    else
      return diagnoseModuleChange(options::SDKJsonPaths[0],
                                  options::SDKJsonPaths[1]);
  case ActionType::None:
    llvm::errs() << "Action required\n";
    llvm::cl::PrintHelpMessage();
    return 1;
  }
}
