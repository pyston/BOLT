//===--- DWARFRewriter.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "DWARFRewriter.h"
#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "DebugData.h"
#include "ParallelUtilities.h"
#include "Utils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DWP/DWP.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/ToolOutputFile.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

LLVM_ATTRIBUTE_UNUSED
static void printDie(const DWARFDie &DIE) {
  DIDumpOptions DumpOpts;
  DumpOpts.ShowForm = true;
  DumpOpts.Verbose = true;
  DumpOpts.ChildRecurseDepth = 0;
  DumpOpts.ShowChildren = 0;
  DIE.dump(dbgs(), 0, DumpOpts);
}

struct AttrInfo {
  DWARFFormValue V;
  uint64_t Offset;
};

/// Finds attributes FormValue and Offset.
///
/// \param DIE die to look up in.
/// \param Attr the attribute to extract.
/// \return an optional AttrInfo with DWARFFormValue and Offset.
Optional<AttrInfo> findAttributeInfo(const DWARFDie DIE,
                                     dwarf::Attribute Attr) {
  if (!DIE.isValid())
    return None;
  const DWARFAbbreviationDeclaration *AbbrevDecl =
      DIE.getAbbreviationDeclarationPtr();
  if (!AbbrevDecl)
    return None;
  Optional<uint32_t> Index = AbbrevDecl->findAttributeIndex(Attr);
  if (!Index)
    return None;
  const DWARFUnit &U = *DIE.getDwarfUnit();
  uint64_t Offset =
      AbbrevDecl->getAttributeOffsetFromIndex(*Index, DIE.getOffset(), U);
  Optional<DWARFFormValue> Value =
      AbbrevDecl->getAttributeValueFromOffset(*Index, Offset, U);
  if (!Value)
    return None;
  return AttrInfo{*Value, Offset};
}

using namespace llvm;
using namespace llvm::support::endian;
using namespace object;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltCategory;
extern cl::opt<unsigned> Verbosity;
extern cl::opt<std::string> OutputFilename;

static cl::opt<bool>
KeepARanges("keep-aranges",
  cl::desc("keep or generate .debug_aranges section if .gdb_index is written"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
DeterministicDebugInfo("deterministic-debuginfo",
  cl::desc("disables parallel execution of tasks that may produce"
           "nondeterministic debug info"),
  cl::init(true),
  cl::cat(BoltCategory));

static cl::opt<std::string> DwarfOutputPath(
    "dwarf-output-path",
    cl::desc("Path to where .dwo files or dwp file will be written out to."),
    cl::init(""), cl::cat(BoltCategory));

static cl::opt<bool>
    WriteDWP("write-dwp",
             cl::desc("output a single dwarf package file (dwp) instead of "
                      "multiple non-relocatable dwarf object files (dwo)."),
             cl::init(false), cl::cat(BoltCategory));

static cl::opt<bool>
    DebugSkeletonCu("debug-skeleton-cu",
                    cl::desc("prints out offsetrs for abbrev and debu_info of "
                             "Skeleton CUs that get patched."),
                    cl::ZeroOrMore, cl::Hidden, cl::init(false),
                    cl::cat(BoltCategory));
} // namespace opts

/// Returns DWO Name to be used. Handles case where user specifies output DWO
/// directory, and there are duplicate names. Assumes DWO ID is unique.
static std::string
getDWOName(llvm::DWARFUnit &CU,
           std::unordered_map<std::string, uint32_t> *NameToIndexMap,
           std::unordered_map<uint64_t, std::string> &DWOIdToName) {
  llvm::Optional<uint64_t> DWOId = CU.getDWOId();
  assert(DWOId && "DWO ID not found.");
  (void)DWOId;
  auto NameIter = DWOIdToName.find(*DWOId);
  if (NameIter != DWOIdToName.end())
    return NameIter->second;

  std::string DWOName = dwarf::toString(
      CU.getUnitDIE().find({dwarf::DW_AT_dwo_name, dwarf::DW_AT_GNU_dwo_name}),
      "");
  assert(!DWOName.empty() &&
         "DW_AT_dwo_name/DW_AT_GNU_dwo_name does not exists.");
  if (NameToIndexMap && !opts::DwarfOutputPath.empty()) {
    auto Iter = NameToIndexMap->find(DWOName);
    if (Iter == NameToIndexMap->end()) {
      Iter = NameToIndexMap->insert({DWOName, 0}).first;
    }
    DWOName.append(std::to_string(Iter->second));
    ++Iter->second;
  }
  DWOName.append(".dwo");
  DWOIdToName[*DWOId] = DWOName;
  return DWOName;
}

static bool isHighPcFormEightBytes(dwarf::Form DwarfForm) {
  return DwarfForm == dwarf::DW_FORM_addr || DwarfForm == dwarf::DW_FORM_data8;
}

void DWARFRewriter::updateDebugInfo() {
  ErrorOr<BinarySection &> DebugInfo = BC.getUniqueSectionByName(".debug_info");
  if (!DebugInfo)
    return;

  DebugInfo->registerPatcher(std::make_unique<SimpleBinaryPatcher>());
  auto *DebugInfoPatcher =
      static_cast<SimpleBinaryPatcher *>(DebugInfo->getPatcher());

  ARangesSectionWriter = std::make_unique<DebugARangesSectionWriter>();
  RangesSectionWriter = std::make_unique<DebugRangesSectionWriter>();
  StrWriter = std::make_unique<DebugStrWriter>(&BC);
  AbbrevWriter = std::make_unique<DebugAbbrevWriter>();

  AddrWriter = std::make_unique<DebugAddrWriter>(&BC);
  DebugLoclistWriter::setAddressWriter(AddrWriter.get());

  uint64_t NumCUs = BC.DwCtx->getNumCompileUnits();
  if ((opts::NoThreads || opts::DeterministicDebugInfo) &&
      BC.getNumDWOCUs() == 0) {
    // Use single entry for efficiency when running single-threaded
    NumCUs = 1;
  }

  LocListWritersByCU.reserve(NumCUs);

  for (size_t CUIndex = 0; CUIndex < NumCUs; ++CUIndex) {
    LocListWritersByCU[CUIndex] = std::make_unique<DebugLocWriter>(&BC);
  }
  // Unordered maps to handle name collision if output DWO directory is
  // specified.
  std::unordered_map<std::string, uint32_t> NameToIndexMap;
  std::unordered_map<uint64_t, std::string> DWOIdToName;

  auto updateDWONameCompDir = [&](DWARFUnit &Unit) -> void {
    const DWARFDie &DIE = Unit.getUnitDIE();
    Optional<AttrInfo> AttrInfoVal =
        findAttributeInfo(DIE, dwarf::DW_AT_GNU_dwo_name);
    (void)AttrInfoVal;
    assert(AttrInfoVal && "Skeleton CU doesn't have dwo_name.");

    std::string ObjectName = getDWOName(Unit, &NameToIndexMap, DWOIdToName);
    uint32_t NewOffset = StrWriter->addString(ObjectName.c_str());
    DebugInfoPatcher->addLE32Patch(AttrInfoVal->Offset, NewOffset);

    AttrInfoVal = findAttributeInfo(DIE, dwarf::DW_AT_comp_dir);
    (void)AttrInfoVal;
    assert(AttrInfoVal && "DW_AT_comp_dir is not in Skeleton CU.");

    if (!opts::DwarfOutputPath.empty()) {
      uint32_t NewOffset = StrWriter->addString(opts::DwarfOutputPath.c_str());
      DebugInfoPatcher->addLE32Patch(AttrInfoVal->Offset, NewOffset);
    }
  };

  auto processUnitDIE = [&](size_t CUIndex, DWARFUnit *Unit) {
    // Check if the unit is a skeleton and we need special updates for it and
    // its matching split/DWO CU.
    Optional<DWARFUnit *> SplitCU;
    Optional<uint64_t> RangesBase;
    llvm::Optional<uint64_t> DWOId = Unit->getDWOId();
    if (DWOId)
      SplitCU = BC.getDWOCU(*DWOId);

    // Skipping CUs that failed to load.
    if (SplitCU) {
      updateDWONameCompDir(*Unit);

      // Assuming there is unique DWOID per binary. i.e. two or more CUs don't
      // have same DWO ID.
      assert(LocListWritersByCU.count(*DWOId) == 0 &&
             "LocList writer for DWO unit already exists.");
      LocListWritersByCU[*DWOId] =
          std::make_unique<DebugLoclistWriter>(&BC, *DWOId);
      SimpleBinaryPatcher *DwoDebugInfoPatcher =
          getBinaryDWODebugInfoPatcher(*DWOId);
      RangesBase = RangesSectionWriter->getSectionOffset();
      DwoDebugInfoPatcher->setRangeBase(*RangesBase);
      DebugAbbrevWriter *DWOAbbrevWriter = getBinaryDWOAbbrevWriter(*DWOId);
      updateUnitDebugInfo(*DWOId, *(*SplitCU), *DwoDebugInfoPatcher,
                          *DWOAbbrevWriter);
      static_cast<DebugLoclistWriter *>(LocListWritersByCU[*DWOId].get())
          ->finalizePatches();
      if (!DwoDebugInfoPatcher->getWasRangBasedUsed())
        RangesBase = None;
    }

    updateUnitDebugInfo(CUIndex, *Unit, *DebugInfoPatcher, *AbbrevWriter,
                        RangesBase);
  };

  if (opts::NoThreads || opts::DeterministicDebugInfo) {
    for (std::unique_ptr<DWARFUnit> &CU : BC.DwCtx->compile_units()) {
      processUnitDIE(0, CU.get());
    }
  } else {
    // Update unit debug info in parallel
    ThreadPool &ThreadPool = ParallelUtilities::getThreadPool();
    size_t CUIndex = 0;
    for (std::unique_ptr<DWARFUnit> &CU : BC.DwCtx->compile_units()) {
      ThreadPool.async(processUnitDIE, CUIndex, CU.get());
      CUIndex++;
    }

    ThreadPool.wait();
  }

  flushPendingRanges(*DebugInfoPatcher);

  finalizeDebugSections(*DebugInfoPatcher);

  if (opts::WriteDWP)
    writeDWP(DWOIdToName);
  else
    writeDWOFiles(DWOIdToName);

  updateGdbIndexSection();
}

void DWARFRewriter::updateUnitDebugInfo(uint64_t CUIndex, DWARFUnit &Unit,
                                        SimpleBinaryPatcher &DebugInfoPatcher,
                                        DebugAbbrevWriter &AbbrevWriter,
                                        Optional<uint64_t> RangesBase) {
  // Cache debug ranges so that the offset for identical ranges could be reused.
  std::map<DebugAddressRangesVector, uint64_t> CachedRanges;

  auto &DebugLocWriter = *LocListWritersByCU[CUIndex].get();

  uint64_t DIEOffset = Unit.getOffset() + Unit.getHeaderSize();
  uint64_t NextCUOffset = Unit.getNextUnitOffset();
  DWARFDebugInfoEntry Die;
  DWARFDataExtractor DebugInfoData = Unit.getDebugInfoExtractor();
  uint32_t Depth = 0;

  while (
      DIEOffset < NextCUOffset &&
      Die.extractFast(Unit, &DIEOffset, DebugInfoData, NextCUOffset, Depth)) {
    if (const DWARFAbbreviationDeclaration *AbbrDecl =
            Die.getAbbreviationDeclarationPtr()) {
      if (AbbrDecl->hasChildren())
        ++Depth;
    } else {
      // NULL entry.
      if (Depth > 0)
        --Depth;
      if (Depth == 0)
        break;
    }

    DWARFDie DIE(&Unit, &Die);

    switch (DIE.getTag()) {
    case dwarf::DW_TAG_compile_unit: {
      auto ModuleRangesOrError = DIE.getAddressRanges();
      if (!ModuleRangesOrError) {
        consumeError(ModuleRangesOrError.takeError());
        break;
      }
      DWARFAddressRangesVector ModuleRanges = *ModuleRangesOrError;
      DebugAddressRangesVector OutputRanges =
          BC.translateModuleAddressRanges(ModuleRanges);
      const uint64_t RangesSectionOffset =
          RangesSectionWriter->addRanges(OutputRanges);
      if (!Unit.isDWOUnit())
        ARangesSectionWriter->addCURanges(Unit.getOffset(),
                                          std::move(OutputRanges));
      updateDWARFObjectAddressRanges(DIE, RangesSectionOffset, DebugInfoPatcher,
                                     AbbrevWriter, RangesBase);
      break;
    }
    case dwarf::DW_TAG_subprogram: {
      // Get function address either from ranges or [LowPC, HighPC) pair.
      bool UsesRanges = false;
      uint64_t Address;
      uint64_t SectionIndex, HighPC;
      if (!DIE.getLowAndHighPC(Address, HighPC, SectionIndex)) {
        Expected<DWARFAddressRangesVector> RangesOrError =
            DIE.getAddressRanges();
        if (!RangesOrError) {
          consumeError(RangesOrError.takeError());
          break;
        }
        DWARFAddressRangesVector Ranges = *RangesOrError;
        // Not a function definition.
        if (Ranges.empty())
          break;

        Address = Ranges.front().LowPC;
        UsesRanges = true;
      }

      // Clear cached ranges as the new function will have its own set.
      CachedRanges.clear();

      DebugAddressRangesVector FunctionRanges;
      if (const BinaryFunction *Function =
              BC.getBinaryFunctionAtAddress(Address))
        FunctionRanges = Function->getOutputAddressRanges();
      // Update ranges.
      if (UsesRanges) {
        updateDWARFObjectAddressRanges(
            DIE, RangesSectionWriter->addRanges(FunctionRanges),
            DebugInfoPatcher, AbbrevWriter);
      } else {
        // Delay conversion of [LowPC, HighPC) into DW_AT_ranges if possible.
        const DWARFAbbreviationDeclaration *Abbrev =
            DIE.getAbbreviationDeclarationPtr();
        assert(Abbrev && "abbrev expected");

        // Create a critical section.
        static std::shared_timed_mutex CriticalSectionMutex;
        std::unique_lock<std::shared_timed_mutex> Lock(CriticalSectionMutex);

        if (FunctionRanges.size() > 1) {
          convertPending(Unit, Abbrev, DebugInfoPatcher, AbbrevWriter);
          // Exit critical section early.
          Lock.unlock();
          convertToRanges(DIE, FunctionRanges, DebugInfoPatcher);
        } else if (ConvertedRangesAbbrevs.find(Abbrev) !=
                   ConvertedRangesAbbrevs.end()) {
          // Exit critical section early.
          Lock.unlock();
          convertToRanges(DIE, FunctionRanges, DebugInfoPatcher);
        } else {
          if (FunctionRanges.empty())
            FunctionRanges.emplace_back(DebugAddressRange());
          addToPendingRanges(Abbrev, DIE, FunctionRanges, Unit.getDWOId());
        }
      }
      break;
    }
    case dwarf::DW_TAG_lexical_block:
    case dwarf::DW_TAG_inlined_subroutine:
    case dwarf::DW_TAG_try_block:
    case dwarf::DW_TAG_catch_block: {
      uint64_t RangesSectionOffset =
          RangesSectionWriter->getEmptyRangesOffset();
      Expected<DWARFAddressRangesVector> RangesOrError = DIE.getAddressRanges();
      const BinaryFunction *Function = RangesOrError && !RangesOrError->empty()
          ? BC.getBinaryFunctionContainingAddress(RangesOrError->front().LowPC)
          : nullptr;
      if (Function) {
        DebugAddressRangesVector OutputRanges =
            Function->translateInputToOutputRanges(*RangesOrError);
        LLVM_DEBUG(if (OutputRanges.empty() != RangesOrError->empty()) {
          dbgs() << "BOLT-DEBUG: problem with DIE at 0x"
                 << Twine::utohexstr(DIE.getOffset()) << " in CU at 0x"
                 << Twine::utohexstr(Unit.getOffset()) << '\n';
        });
        RangesSectionOffset = RangesSectionWriter->addRanges(
            std::move(OutputRanges), CachedRanges);
      } else if (!RangesOrError) {
        consumeError(RangesOrError.takeError());
      }
      updateDWARFObjectAddressRanges(DIE, RangesSectionOffset, DebugInfoPatcher,
                                     AbbrevWriter);
      break;
    }
    default: {
      // Handle any tag that can have DW_AT_location attribute.
      DWARFFormValue Value;
      uint64_t AttrOffset;
      if (Optional<AttrInfo> AttrVal =
              findAttributeInfo(DIE, dwarf::DW_AT_location)) {
        AttrOffset = AttrVal->Offset;
        Value = AttrVal->V;
        if (Value.isFormClass(DWARFFormValue::FC_Constant) ||
            Value.isFormClass(DWARFFormValue::FC_SectionOffset)) {
          uint64_t Offset = Value.isFormClass(DWARFFormValue::FC_Constant)
                                      ? Value.getAsUnsignedConstant().getValue()
                                      : Value.getAsSectionOffset().getValue();
          DebugLocationsVector InputLL;

          Optional<object::SectionedAddress> SectionAddress =
              Unit.getBaseAddress();
          uint64_t BaseAddress = 0;
          if (SectionAddress)
            BaseAddress = SectionAddress->Address;

          Error E = Unit.getLocationTable().visitLocationList(
              &Offset, [&](const DWARFLocationEntry &Entry) {
                switch (Entry.Kind) {
                default:
                  llvm_unreachable("Unsupported DWARFLocationEntry Kind.");
                case dwarf::DW_LLE_end_of_list:
                  return false;
                case dwarf::DW_LLE_base_address:
                  assert(Entry.SectionIndex == SectionedAddress::UndefSection &&
                         "absolute address expected");
                  BaseAddress = Entry.Value0;
                  break;
                case dwarf::DW_LLE_offset_pair:
                  assert(
                      (Entry.SectionIndex == SectionedAddress::UndefSection &&
                       !Unit.isDWOUnit()) &&
                      "absolute address expected");
                  InputLL.emplace_back(DebugLocationEntry{
                      BaseAddress + Entry.Value0, BaseAddress + Entry.Value1,
                      Entry.Loc});
                  break;
                case dwarf::DW_LLE_startx_length:
                  assert(Unit.isDWOUnit() &&
                         "None DWO Unit with DW_LLE_startx_length encoding.");
                  Optional<object::SectionedAddress> EntryAddress =
                      Unit.getAddrOffsetSectionItem(Entry.Value0);
                  assert(EntryAddress && "Address does not exist.");
                  InputLL.emplace_back(DebugLocationEntry{
                      EntryAddress->Address,
                      EntryAddress->Address + Entry.Value1, Entry.Loc});
                  break;
                }
                return true;
              });

          uint64_t OutputLocListOffset = DebugLocWriter::EmptyListTag;
          if (E || InputLL.empty()) {
            errs() << "BOLT-WARNING: empty location list detected at 0x"
                   << Twine::utohexstr(Offset) << " for DIE at 0x"
                   << Twine::utohexstr(DIE.getOffset()) << " in CU at 0x"
                   << Twine::utohexstr(Unit.getOffset()) << '\n';
          } else {
            const uint64_t Address = InputLL.front().LowPC;
            if (const BinaryFunction *Function =
                    BC.getBinaryFunctionContainingAddress(Address)) {
              const DebugLocationsVector OutputLL = Function
                  ->translateInputToOutputLocationList(InputLL);
              LLVM_DEBUG(if (OutputLL.empty()) {
                dbgs() << "BOLT-DEBUG: location list translated to an empty "
                          "one at 0x"
                       << Twine::utohexstr(DIE.getOffset()) << " in CU at 0x"
                       << Twine::utohexstr(Unit.getOffset()) << '\n';
              });
              OutputLocListOffset = DebugLocWriter.addList(OutputLL);
            }
          }

          if (OutputLocListOffset != DebugLocWriter::EmptyListTag) {
            std::lock_guard<std::mutex> Lock(LocListDebugInfoPatchesMutex);
            if (Unit.isDWOUnit()) {
              // Not sure if better approach is to hide all of this away in a
              // class. Also re-using LocListDebugInfoPatchType. Wasting some
              // space for DWOID/CUIndex.
              DwoLocListDebugInfoPatches[CUIndex].push_back(
                  {AttrOffset, CUIndex, OutputLocListOffset});
            } else {
              LocListDebugInfoPatches.push_back(
                  {AttrOffset, CUIndex, OutputLocListOffset});
            }
          } else {
            std::lock_guard<std::mutex> Lock(DebugInfoPatcherMutex);
            DebugInfoPatcher.addLE32Patch(AttrOffset,
                                          DebugLocWriter::EmptyListOffset);
          }
        } else {
          assert((Value.isFormClass(DWARFFormValue::FC_Exprloc) ||
                  Value.isFormClass(DWARFFormValue::FC_Block)) &&
                 "unexpected DW_AT_location form");
          if (Unit.isDWOUnit()) {
            ArrayRef<uint8_t> Expr = *Value.getAsBlock();
            DataExtractor Data(
                StringRef((const char *)Expr.data(), Expr.size()),
                Unit.getContext().isLittleEndian(), 0);
            DWARFExpression LocExpr(Data, Unit.getAddressByteSize(),
                                    Unit.getFormParams().Format);
            for (auto &Expr : LocExpr) {
              if (Expr.getCode() != dwarf::DW_OP_GNU_addr_index)
                continue;
              uint64_t Index = Expr.getRawOperand(0);
              Optional<object::SectionedAddress> EntryAddress =
                  Unit.getAddrOffsetSectionItem(Index);
              assert(EntryAddress && "Address is not found.");
              assert(Index <= std::numeric_limits<uint32_t>::max() &&
                     "Invalid Operand Index.");
              AddrWriter->addIndexAddress(EntryAddress->Address,
                                          static_cast<uint32_t>(Index),
                                          *Unit.getDWOId());
            }
          }
        }
      } else if (Optional<AttrInfo> AttrVal =
                     findAttributeInfo(DIE, dwarf::DW_AT_low_pc)) {
        AttrOffset = AttrVal->Offset;
        Value = AttrVal->V;
        const Optional<uint64_t> Result = Value.getAsAddress();
        if (Result.hasValue()) {
          const uint64_t Address = Result.getValue();
          uint64_t NewAddress = 0;
          if (const BinaryFunction *Function =
                  BC.getBinaryFunctionContainingAddress(Address)) {
            NewAddress = Function->translateInputToOutputAddress(Address);
            LLVM_DEBUG(dbgs()
                       << "BOLT-DEBUG: Fixing low_pc 0x"
                       << Twine::utohexstr(Address) << " for DIE with tag "
                       << DIE.getTag() << " to 0x"
                       << Twine::utohexstr(NewAddress) << '\n');
          }

          dwarf::Form Form = Value.getForm();
          assert(Form != dwarf::DW_FORM_LLVM_addrx_offset &&
                 "DW_FORM_LLVM_addrx_offset is not supported");
          std::lock_guard<std::mutex> Lock(DebugInfoPatcherMutex);
          if (Form == dwarf::DW_FORM_GNU_addr_index) {
            assert(Unit.isDWOUnit() &&
                   "DW_FORM_GNU_addr_index in Non DWO unit.");
            uint64_t Index = Value.getRawUValue();
            // If there is no new address, storing old address.
            // Re-using Index to make implementation easier.
            // DW_FORM_GNU_addr_index is variable lenght encoding so we either
            // have to create indices of same sizes, or use same index.
            AddrWriter->addIndexAddress(NewAddress ? NewAddress : Address,
                                        Index, *Unit.getDWOId());
          } else {
            DebugInfoPatcher.addLE64Patch(AttrOffset, NewAddress);
          }
        } else if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: unexpected form value for attribute at 0x"
                 << Twine::utohexstr(AttrOffset);
        }
      }
    }
    }
  }

  if (DIEOffset > NextCUOffset) {
    errs() << "BOLT-WARNING: corrupt DWARF detected at 0x"
           << Twine::utohexstr(Unit.getOffset()) << '\n';
  }

  AbbrevWriter.addUnitAbbreviations(Unit);
}

void DWARFRewriter::updateDWARFObjectAddressRanges(
    const DWARFDie DIE, uint64_t DebugRangesOffset,
    SimpleBinaryPatcher &DebugInfoPatcher, DebugAbbrevWriter &AbbrevWriter,
    Optional<uint64_t> RangesBase) {

  // Some objects don't have an associated DIE and cannot be updated (such as
  // compiler-generated functions).
  if (!DIE)
    return;

  const DWARFAbbreviationDeclaration *AbbreviationDecl =
      DIE.getAbbreviationDeclarationPtr();
  if (!AbbreviationDecl) {
    if (opts::Verbosity >= 1) {
      errs() << "BOLT-WARNING: object's DIE doesn't have an abbreviation: "
             << "skipping update. DIE at offset 0x"
             << Twine::utohexstr(DIE.getOffset()) << '\n';
    }
    return;
  }

  if (RangesBase) {
    // If DW_AT_GNU_ranges_base is present, update it. No further modifications
    // are needed for ranges base.
    Optional<AttrInfo> RangesBaseAttrInfo =
        findAttributeInfo(DIE, dwarf::DW_AT_GNU_ranges_base);
    if (RangesBaseAttrInfo) {
      DebugInfoPatcher.addLE32Patch(RangesBaseAttrInfo->Offset,
                                    static_cast<uint32_t>(*RangesBase));
      RangesBase = None;
    }
  }

  if (AbbreviationDecl->findAttributeIndex(dwarf::DW_AT_ranges)) {
    // Case 1: The object was already non-contiguous and had DW_AT_ranges.
    // In this case we simply need to update the value of DW_AT_ranges
    // and introduce DW_AT_GNU_ranges_base if required.
    Optional<AttrInfo> AttrVal = findAttributeInfo(DIE, dwarf::DW_AT_ranges);

    std::lock_guard<std::mutex> Lock(DebugInfoPatcherMutex);
    DebugInfoPatcher.addLE32Patch(
        AttrVal->Offset, DebugRangesOffset - DebugInfoPatcher.getRangeBase());
    if (!RangesBase)
      return;

    // Convert DW_AT_low_pc into DW_AT_GNU_ranges_base.
    Optional<AttrInfo> LowPCAttrInfo =
        findAttributeInfo(DIE, dwarf::DW_AT_low_pc);
    if (!LowPCAttrInfo) {
      errs() << "BOLT-ERROR: skeleton CU at 0x"
             << Twine::utohexstr(DIE.getOffset())
             << " does not have DW_AT_GNU_ranges_base or DW_AT_low_pc to"
                " convert to update ranges base\n";
      return;
    }

    AbbrevWriter.addAttributePatch(
        *DIE.getDwarfUnit(), AbbreviationDecl, dwarf::DW_AT_low_pc,
        dwarf::DW_AT_GNU_ranges_base, dwarf::DW_FORM_indirect);
    DebugInfoPatcher.addUDataPatch(LowPCAttrInfo->Offset, dwarf::DW_FORM_udata,
                                   1);
    DebugInfoPatcher.addUDataPatch(LowPCAttrInfo->Offset + 1, *RangesBase, 7);

    return;
  }

  // Case 2: The object has both DW_AT_low_pc and DW_AT_high_pc emitted back
  // to back. Replace with new attributes and patch the DIE.
  if (AbbreviationDecl->findAttributeIndex(dwarf::DW_AT_low_pc) &&
      AbbreviationDecl->findAttributeIndex(dwarf::DW_AT_high_pc)) {
    convertToRanges(*DIE.getDwarfUnit(), AbbreviationDecl, AbbrevWriter,
                    RangesBase);
    convertToRanges(DIE, DebugRangesOffset, DebugInfoPatcher, RangesBase);
  } else {
    if (opts::Verbosity >= 1) {
      errs() << "BOLT-ERROR: cannot update ranges for DIE at offset 0x"
             << Twine::utohexstr(DIE.getOffset()) << '\n';
    }
  }
}

void DWARFRewriter::updateLineTableOffsets() {
  const MCSection *LineSection =
    BC.Ctx->getObjectFileInfo()->getDwarfLineSection();
  auto CurrentFragment = LineSection->begin();
  uint64_t CurrentOffset = 0;
  uint64_t Offset = 0;

  ErrorOr<BinarySection &> DbgInfoSection =
      BC.getUniqueSectionByName(".debug_info");
  ErrorOr<BinarySection &> TypeInfoSection =
      BC.getUniqueSectionByName(".debug_types");
  assert(((BC.DwCtx->getNumTypeUnits() > 0 && TypeInfoSection) ||
          BC.DwCtx->getNumTypeUnits() == 0) &&
         "Was not able to retrieve Debug Types section.");

  // There is no direct connection between CU and TU, but same offsets,
  // encoded in DW_AT_stmt_list, into .debug_line get modified.
  // We take advantage of that to map original CU line table offsets to new
  // ones.
  std::unordered_map<uint64_t, uint64_t> DebugLineOffsetMap;

  auto GetStatementListValue = [](DWARFUnit *Unit) {
    Optional<DWARFFormValue> StmtList =
        Unit->getUnitDIE().find(dwarf::DW_AT_stmt_list);
    Optional<uint64_t> Offset = dwarf::toSectionOffset(StmtList);
    assert(Offset && "Was not able to retreive value of DW_AT_stmt_list.");
    return *Offset;
  };

  for (const std::unique_ptr<DWARFUnit> &CU : BC.DwCtx->compile_units()) {
    const unsigned CUID = CU->getOffset();
    MCSymbol *Label = BC.Ctx->getMCDwarfLineTable(CUID).getLabel();
    if (!Label)
      continue;

    Optional<AttrInfo> AttrVal =
        findAttributeInfo(CU.get()->getUnitDIE(), dwarf::DW_AT_stmt_list);
    if (!AttrVal)
      continue;
    const uint64_t LTOffset = AttrVal->Offset;

    // Line tables are stored in MCContext in ascending order of offset in the
    // output file, thus we can compute all table's offset by passing through
    // each fragment at most once, continuing from the last CU's beginning
    // instead of from the first fragment.
    MCFragment *Fragment = Label->getFragment();
    while (&*CurrentFragment != Fragment) {
      switch (CurrentFragment->getKind()) {
      case MCFragment::FT_Dwarf:
        Offset += cast<MCDwarfLineAddrFragment>(*CurrentFragment)
          .getContents().size() - CurrentOffset;
        break;
      case MCFragment::FT_Data:
        Offset += cast<MCDataFragment>(*CurrentFragment)
          .getContents().size() - CurrentOffset;
        break;
      default:
        llvm_unreachable(".debug_line section shouldn't contain other types "
                         "of fragments.");
      }
      ++CurrentFragment;
      CurrentOffset = 0;
    }

    Offset += Label->getOffset() - CurrentOffset;
    CurrentOffset = Label->getOffset();

    DebugLineOffsetMap[GetStatementListValue(CU.get())] = Offset;
    assert(DbgInfoSection && ".debug_info section must exist");
    DbgInfoSection->addRelocation(LTOffset,
                                  nullptr,
                                  ELF::R_X86_64_32,
                                  Offset,
                                  0,
                                  /*Pending=*/true);

    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: CU " << CUID
                      << " has line table at " << Offset << "\n");
  }

  for (const std::unique_ptr<DWARFUnit> &TU : BC.DwCtx->types_section_units()) {
    DWARFUnit *Unit = TU.get();
    Optional<AttrInfo> AttrVal =
        findAttributeInfo(TU.get()->getUnitDIE(), dwarf::DW_AT_stmt_list);
    if (!AttrVal)
      continue;
    const uint64_t LTOffset = AttrVal->Offset;
    auto Iter = DebugLineOffsetMap.find(GetStatementListValue(Unit));
    assert(Iter != DebugLineOffsetMap.end() &&
           "Type Unit Updated Line Number Entry does not exist.");
    TypeInfoSection->addRelocation(LTOffset, nullptr, ELF::R_X86_64_32,
                                   Iter->second, 0, /*Pending=*/true);
  }

  // Set .debug_info as finalized so it won't be skipped over when
  // we process sections while writing out the new binary.  This ensures
  // that the pending relocations will be processed and not ignored.
  if(DbgInfoSection)
    DbgInfoSection->setIsFinalized();

  if (TypeInfoSection)
    TypeInfoSection->setIsFinalized();
}

void DWARFRewriter::finalizeDebugSections(
    SimpleBinaryPatcher &DebugInfoPatcher) {
  // Skip .debug_aranges if we are re-generating .gdb_index.
  if (opts::KeepARanges || !BC.getGdbIndexSection()) {
    SmallVector<char, 16> ARangesBuffer;
    raw_svector_ostream OS(ARangesBuffer);

    auto MAB = std::unique_ptr<MCAsmBackend>(BC.TheTarget->createMCAsmBackend(
        *BC.STI, *BC.MRI, MCTargetOptions()));

    ARangesSectionWriter->writeARangesSection(OS);
    const StringRef &ARangesContents = OS.str();

    BC.registerOrUpdateNoteSection(".debug_aranges",
                                    copyByteArray(ARangesContents),
                                    ARangesContents.size());
  }

  if (StrWriter->isInitialized()) {
    RewriteInstance::addToDebugSectionsToOverwrite(".debug_str");
    std::unique_ptr<DebugStrBufferVector> DebugStrSectionContents =
        StrWriter->finalize();
    BC.registerOrUpdateNoteSection(".debug_str",
                                   copyByteArray(*DebugStrSectionContents),
                                   DebugStrSectionContents->size());
  }

  if (AddrWriter->isInitialized()) {
    AddressSectionBuffer AddressSectionContents = AddrWriter->finalize();
    BC.registerOrUpdateNoteSection(".debug_addr",
                                   copyByteArray(AddressSectionContents),
                                   AddressSectionContents.size());
    for (auto &CU : BC.DwCtx->compile_units()) {
      DWARFDie DIE = CU->getUnitDIE();
      if (Optional<AttrInfo> AttrVal =
              findAttributeInfo(DIE, dwarf::DW_AT_GNU_addr_base)) {
        uint64_t Offset = AddrWriter->getOffset(*CU->getDWOId());
        DebugInfoPatcher.addLE32Patch(AttrVal->Offset,
                                      static_cast<int32_t>(Offset));
      }
    }
  }

  std::unique_ptr<DebugBufferVector> RangesSectionContents =
      RangesSectionWriter->finalize();
  BC.registerOrUpdateNoteSection(".debug_ranges",
                                 copyByteArray(*RangesSectionContents),
                                 RangesSectionContents->size());

  std::unique_ptr<DebugBufferVector> LocationListSectionContents =
      makeFinalLocListsSection(DebugInfoPatcher);
  BC.registerOrUpdateNoteSection(".debug_loc",
                                 copyByteArray(*LocationListSectionContents),
                                 LocationListSectionContents->size());

  std::unique_ptr<DebugBufferVector> AbbrevSectionContents =
      AbbrevWriter->finalize();
  BC.registerOrUpdateNoteSection(".debug_abbrev",
                                 copyByteArray(*AbbrevSectionContents),
                                 AbbrevSectionContents->size());

  // Update abbreviation offsets if they were changed.
  SimpleBinaryPatcher *DebugTypesPatcher = nullptr;
  for (auto &CU : BC.DwCtx->normal_units()) {
    assert(!CU->isDWOUnit());

    const uint64_t NewAbbrevOffset =
        AbbrevWriter->getAbbreviationsOffsetForUnit(*CU);
    if (CU->getAbbreviationsOffset() == NewAbbrevOffset)
      continue;

    // DWARFv4
    // unit_length - 4 bytes
    // version - 2 bytes
    // So + 6 to patch debug_abbrev_offset
    constexpr uint64_t AbbrevFieldOffset = 6;
    if (!CU->isTypeUnit()) {
      DebugInfoPatcher.addLE32Patch(CU->getOffset() + AbbrevFieldOffset,
                                    static_cast<uint32_t>(NewAbbrevOffset));
      continue;
    }

    if (!DebugTypesPatcher) {
      ErrorOr<BinarySection &> DebugTypes =
          BC.getUniqueSectionByName(".debug_types");
      DebugTypes->registerPatcher(std::make_unique<SimpleBinaryPatcher>());
      DebugTypesPatcher =
          static_cast<SimpleBinaryPatcher *>(DebugTypes->getPatcher());
    }
    DebugTypesPatcher->addLE32Patch(CU->getOffset() + AbbrevFieldOffset,
                                    static_cast<uint32_t>(NewAbbrevOffset));
  }
}

// Creates all the data structures necessary for creating MCStreamer.
// They are passed by reference because they need to be kept around.
// Also creates known debug sections. These are sections handled by
// handleDebugDataPatching.
using KnownSectionsEntry = std::pair<MCSection *, DWARFSectionKind>;
namespace {

std::unique_ptr<BinaryContext>
createDwarfOnlyBC(const object::ObjectFile &File) {
  return BinaryContext::createBinaryContext(
      &File, false,
      DWARFContext::create(File, nullptr, "", WithColor::defaultErrorHandler,
                           WithColor::defaultWarningHandler));
}

StringMap<KnownSectionsEntry>
createKnownSectionsMap(const MCObjectFileInfo &MCOFI) {
  StringMap<KnownSectionsEntry> KnownSectionsTemp = {
      {"debug_info.dwo", {MCOFI.getDwarfInfoDWOSection(), DW_SECT_INFO}},
      {"debug_types.dwo", {MCOFI.getDwarfTypesDWOSection(), DW_SECT_EXT_TYPES}},
      {"debug_str_offsets.dwo",
       {MCOFI.getDwarfStrOffDWOSection(), DW_SECT_STR_OFFSETS}},
      {"debug_str.dwo", {MCOFI.getDwarfStrDWOSection(), DW_SECT_EXT_unknown}},
      {"debug_loc.dwo", {MCOFI.getDwarfLocDWOSection(), DW_SECT_EXT_LOC}},
      {"debug_abbrev.dwo", {MCOFI.getDwarfAbbrevDWOSection(), DW_SECT_ABBREV}},
      {"debug_line.dwo",
       {MCOFI.getDwarfLineDWOSection(), DW_SECT_EXT_unknown}}};
  return KnownSectionsTemp;
}

StringRef getSectionName(const SectionRef &Section) {
  Expected<StringRef> SectionName = Section.getName();
  assert(SectionName && "Invalid section name.");
  StringRef Name = *SectionName;
  Name = Name.substr(Name.find_first_not_of("._"));
  return Name;
}

// Exctracts an appropriate slice if input is DWP.
// Applies patches or overwrites the section.
Optional<StringRef>
updateDebugData(std::string &Storage, const SectionRef &Section,
                const StringMap<KnownSectionsEntry> &KnownSections,
                MCStreamer &Streamer, DWARFRewriter &Writer,
                const DWARFUnitIndex::Entry *DWOEntry, uint64_t DWOId,
                std::unique_ptr<DebugBufferVector> &OutputBuffer) {
  auto applyPatch = [&](BinaryPatcher *Patcher, StringRef Data,
                        uint32_t Offset) -> StringRef {
    Storage = Data.str();
    Patcher->patchBinary(Storage, Offset);
    return StringRef(Storage.c_str(), Storage.size());
  };

  using DWOSectionContribution =
      const DWARFUnitIndex::Entry::SectionContribution;
  auto getSliceData = [&](const DWARFUnitIndex::Entry *DWOEntry,
                          StringRef OutData, DWARFSectionKind Sec,
                          uint32_t &DWPOffset) -> StringRef {
    if (DWOEntry) {
      DWOSectionContribution *DWOContrubution = DWOEntry->getContribution(Sec);
      DWPOffset = DWOContrubution->Offset;
      OutData = OutData.substr(DWPOffset, DWOContrubution->Length);
    }
    return OutData;
  };

  StringRef Name = getSectionName(Section);
  auto SectionIter = KnownSections.find(Name);
  if (SectionIter == KnownSections.end())
    return None;
  Streamer.SwitchSection(SectionIter->second.first);
  Expected<StringRef> Contents = Section.getContents();
  assert(Contents && "Invalid contents.");
  StringRef OutData = *Contents;
  uint32_t DWPOffset = 0;

  switch (SectionIter->second.second) {
  default: {
    if (!Name.equals("debug_str.dwo"))
      errs() << "BOLT-WARNING: Unsupported Debug section: " << Name << "\n";
    return OutData;
  }
  case DWARFSectionKind::DW_SECT_INFO: {
    OutData = getSliceData(DWOEntry, OutData, DWARFSectionKind::DW_SECT_INFO,
                           DWPOffset);
    SimpleBinaryPatcher *Patcher = Writer.getBinaryDWODebugInfoPatcher(DWOId);
    return applyPatch(Patcher, OutData, DWPOffset);
  }
  case DWARFSectionKind::DW_SECT_EXT_TYPES: {
    return getSliceData(DWOEntry, OutData, DWARFSectionKind::DW_SECT_EXT_TYPES,
                        DWPOffset);
  }
  case DWARFSectionKind::DW_SECT_STR_OFFSETS: {
    return getSliceData(DWOEntry, OutData,
                        DWARFSectionKind::DW_SECT_STR_OFFSETS, DWPOffset);
  }
  case DWARFSectionKind::DW_SECT_ABBREV: {
    DebugAbbrevWriter *AbbrevWriter = Writer.getBinaryDWOAbbrevWriter(DWOId);
    OutputBuffer = AbbrevWriter->finalize();
    // Creating explicit StringRef here, otherwise
    // with impicit conversion it will take null byte as end of
    // string.
    return StringRef(reinterpret_cast<const char *>(OutputBuffer->data()),
                     OutputBuffer->size());
  }
  case DWARFSectionKind::DW_SECT_EXT_LOC: {
    DebugLocWriter *LocWriter = Writer.getDebugLocWriter(DWOId);
    OutputBuffer = LocWriter->finalize();
    // Creating explicit StringRef here, otherwise
    // with impicit conversion it will take null byte as end of
    // string.
    return StringRef(reinterpret_cast<const char *>(OutputBuffer->data()),
                     OutputBuffer->size());
  }
  case DWARFSectionKind::DW_SECT_LINE: {
    return getSliceData(DWOEntry, OutData, DWARFSectionKind::DW_SECT_LINE,
                        DWPOffset);
  }
  }
}

} // namespace

void DWARFRewriter::writeDWP(
    std::unordered_map<uint64_t, std::string> &DWOIdToName) {
  SmallString<0> OutputNameStr;
  StringRef OutputName;
  if (opts::DwarfOutputPath.empty()) {
    OutputName =
        Twine(opts::OutputFilename).concat(".dwp").toStringRef(OutputNameStr);
  } else {
    StringRef ExeFileName = llvm::sys::path::filename(opts::OutputFilename);
    OutputName = Twine(opts::DwarfOutputPath)
                     .concat("/")
                     .concat(ExeFileName)
                     .concat(".dwp")
                     .toStringRef(OutputNameStr);
    errs() << "BOLT-WARNING: dwarf-output-path is in effect and .dwp file will "
              "possibly be written to another location that is not the same as "
              "the executable\n";
  }
  std::error_code EC;
  std::unique_ptr<ToolOutputFile> Out =
      std::make_unique<ToolOutputFile>(OutputName, EC, sys::fs::OF_None);

  const object::ObjectFile *File = BC.DwCtx->getDWARFObj().getFile();
  std::unique_ptr<BinaryContext> TmpBC = createDwarfOnlyBC(*File);
  std::unique_ptr<MCStreamer> Streamer = TmpBC->createStreamer(Out->os());
  const MCObjectFileInfo &MCOFI = *Streamer->getContext().getObjectFileInfo();
  StringMap<KnownSectionsEntry> KnownSections = createKnownSectionsMap(MCOFI);
  MCSection *const StrSection = MCOFI.getDwarfStrDWOSection();
  MCSection *const StrOffsetSection = MCOFI.getDwarfStrOffDWOSection();

  // Data Structures for DWP book keeping
  // Size of array corresponds to the number of sections supported by DWO format
  // in DWARF4/5.
  uint32_t ContributionOffsets[8] = {};
  std::deque<SmallString<32>> UncompressedSections;
  DWPStringPool Strings(*Streamer, StrSection);
  MapVector<uint64_t, UnitIndexEntry> IndexEntries;
  constexpr uint32_t IndexVersion = 2;

  // Setup DWP code once.
  DWARFContext *DWOCtx = BC.getDWOContext();
  const DWARFUnitIndex *CUIndex = nullptr;
  bool IsDWP = false;
  if (DWOCtx) {
    CUIndex = &DWOCtx->getCUIndex();
    IsDWP = !CUIndex->getRows().empty();
  }

  for (const std::unique_ptr<DWARFUnit> &CU : BC.DwCtx->compile_units()) {
    Optional<uint64_t> DWOId = CU->getDWOId();
    if (!DWOId)
      continue;

    // Skipping CUs that we failed to load.
    Optional<DWARFUnit *> DWOCU = BC.getDWOCU(*DWOId);
    if (!DWOCU)
      continue;

    assert(CU->getVersion() == 4 && "For DWP output only DWARF4 is supported");
    UnitIndexEntry CurEntry = {};
    CurEntry.DWOName =
        dwarf::toString(CU->getUnitDIE().find(
                            {dwarf::DW_AT_dwo_name, dwarf::DW_AT_GNU_dwo_name}),
                        "");
    const char *Name = CU->getUnitDIE().getShortName();
    if (Name)
      CurEntry.Name = Name;
    StringRef CurStrSection;
    StringRef CurStrOffsetSection;

    // This maps each section contained in this file to its length.
    // This information is later on used to calculate the contributions,
    // i.e. offset and length, of each compile/type unit to a section.
    std::vector<std::pair<DWARFSectionKind, uint32_t>> SectionLength;

    const DWARFUnitIndex::Entry *DWOEntry = nullptr;
    if (IsDWP)
      DWOEntry = CUIndex->getFromHash(*DWOId);

    bool StrSectionWrittenOut = false;
    const object::ObjectFile *DWOFile =
        (*DWOCU)->getContext().getDWARFObj().getFile();
    for (const SectionRef &Section : DWOFile->sections()) {
      std::string Storage = "";
      std::unique_ptr<DebugBufferVector> OutputData;
      Optional<StringRef> TOutData =
          updateDebugData(Storage, Section, KnownSections, *Streamer, *this,
                          DWOEntry, *DWOId, OutputData);
      if (!TOutData)
        continue;

      StringRef OutData = *TOutData;
      StringRef Name = getSectionName(Section);
      if (Name.equals("debug_str.dwo"))
        CurStrSection = OutData;
      else {
        // Since handleDebugDataPatching returned true, we already know this is
        // a known section.
        auto SectionIter = KnownSections.find(Name);
        if (SectionIter->second.second == DWARFSectionKind::DW_SECT_STR_OFFSETS)
          CurStrOffsetSection = OutData;
        else
          Streamer->emitBytes(OutData);
        auto Index =
            getContributionIndex(SectionIter->second.second, IndexVersion);
        CurEntry.Contributions[Index].Offset = ContributionOffsets[Index];
        CurEntry.Contributions[Index].Length = OutData.size();
        ContributionOffsets[Index] += CurEntry.Contributions[Index].Length;
      }

      // Strings are combined in to a new string section, and de-duplicated
      // based on hash.
      if (!StrSectionWrittenOut && !CurStrOffsetSection.empty() &&
          !CurStrSection.empty()) {
        writeStringsAndOffsets(*Streamer.get(), Strings, StrOffsetSection,
                               CurStrSection, CurStrOffsetSection,
                               CU->getVersion());
        StrSectionWrittenOut = true;
      }
    }
    CompileUnitIdentifiers CUI{*DWOId, CurEntry.Name.c_str(),
                               CurEntry.DWOName.c_str()};
    auto P = IndexEntries.insert(std::make_pair(CUI.Signature, CurEntry));
    if (!P.second) {
      Error Err = buildDuplicateError(*P.first, CUI, "");
      errs() << "BOLT-ERROR: " << toString(std::move(Err)) << "\n";
      return;
    }
  }

  // Lie about the type contribution for DWARF < 5. In DWARFv5 the type
  // section does not exist, so no need to do anything about this.
  ContributionOffsets[getContributionIndex(DW_SECT_EXT_TYPES, 2)] = 0;
  writeIndex(*Streamer.get(), MCOFI.getDwarfCUIndexSection(),
             ContributionOffsets, IndexEntries, IndexVersion);

  Streamer->Finish();
  Out->keep();
}

void DWARFRewriter::writeDWOFiles(
    std::unordered_map<uint64_t, std::string> &DWOIdToName) {
  // Setup DWP code once.
  DWARFContext *DWOCtx = BC.getDWOContext();
  const DWARFUnitIndex *CUIndex = nullptr;
  bool IsDWP = false;
  if (DWOCtx) {
    CUIndex = &DWOCtx->getCUIndex();
    IsDWP = !CUIndex->getRows().empty();
  }

  for (const std::unique_ptr<DWARFUnit> &CU : BC.DwCtx->compile_units()) {
    Optional<uint64_t> DWOId = CU->getDWOId();
    if (!DWOId)
      continue;

    // Skipping CUs that we failed to load.
    Optional<DWARFUnit *> DWOCU = BC.getDWOCU(*DWOId);
    if (!DWOCU)
      continue;

    std::string CompDir = opts::DwarfOutputPath.empty()
                              ? CU->getCompilationDir()
                              : opts::DwarfOutputPath.c_str();
    std::string ObjectName = getDWOName(*CU.get(), nullptr, DWOIdToName);
    auto FullPath = CompDir.append("/").append(ObjectName);

    std::error_code EC;
    std::unique_ptr<ToolOutputFile> TempOut =
        std::make_unique<ToolOutputFile>(FullPath, EC, sys::fs::OF_None);

    const DWARFUnitIndex::Entry *DWOEntry = nullptr;
    if (IsDWP)
      DWOEntry = CUIndex->getFromHash(*DWOId);

    const object::ObjectFile *File =
        (*DWOCU)->getContext().getDWARFObj().getFile();
    std::unique_ptr<BinaryContext> TmpBC = createDwarfOnlyBC(*File);
    std::unique_ptr<MCStreamer> Streamer = TmpBC->createStreamer(TempOut->os());
    StringMap<KnownSectionsEntry> KnownSections =
        createKnownSectionsMap(*Streamer->getContext().getObjectFileInfo());

    for (const SectionRef &Section : File->sections()) {
      std::string Storage = "";
      std::unique_ptr<DebugBufferVector> OutputData;
      if (Optional<StringRef> OutData =
              updateDebugData(Storage, Section, KnownSections, *Streamer, *this,
                              DWOEntry, *DWOId, OutputData))
        Streamer->emitBytes(*OutData);
    }
    Streamer->Finish();
    TempOut->keep();
  }
}

void DWARFRewriter::updateGdbIndexSection() {
  if (!BC.getGdbIndexSection())
    return;

  // See https://sourceware.org/gdb/onlinedocs/gdb/Index-Section-Format.html
  // for .gdb_index section format.

  StringRef GdbIndexContents = BC.getGdbIndexSection()->getContents();

  const char *Data = GdbIndexContents.data();

  // Parse the header.
  const uint32_t Version = read32le(Data);
  if (Version != 7 && Version != 8) {
    errs() << "BOLT-ERROR: can only process .gdb_index versions 7 and 8\n";
    exit(1);
  }

  // Some .gdb_index generators use file offsets while others use section
  // offsets. Hence we can only rely on offsets relative to each other,
  // and ignore their absolute values.
  const uint32_t CUListOffset = read32le(Data + 4);
  const uint32_t CUTypesOffset = read32le(Data + 8);
  const uint32_t AddressTableOffset = read32le(Data + 12);
  const uint32_t SymbolTableOffset = read32le(Data + 16);
  const uint32_t ConstantPoolOffset = read32le(Data + 20);
  Data += 24;

  // Map CUs offsets to indices and verify existing index table.
  std::map<uint32_t, uint32_t> OffsetToIndexMap;
  const uint32_t CUListSize = CUTypesOffset - CUListOffset;
  const unsigned NumCUs = BC.DwCtx->getNumCompileUnits();
  if (CUListSize != NumCUs * 16) {
    errs() << "BOLT-ERROR: .gdb_index: CU count mismatch\n";
    exit(1);
  }
  for (unsigned Index = 0; Index < NumCUs; ++Index, Data += 16) {
    const DWARFUnit *CU = BC.DwCtx->getUnitAtIndex(Index);
    const uint64_t Offset = read64le(Data);
    if (CU->getOffset() != Offset) {
      errs() << "BOLT-ERROR: .gdb_index CU offset mismatch\n";
      exit(1);
    }

    OffsetToIndexMap[Offset] = Index;
  }

  // Ignore old address table.
  const uint32_t OldAddressTableSize = SymbolTableOffset - AddressTableOffset;
  // Move Data to the beginning of symbol table.
  Data += SymbolTableOffset - CUTypesOffset;

  // Calculate the size of the new address table.
  uint32_t NewAddressTableSize = 0;
  for (const auto &CURangesPair : ARangesSectionWriter->getCUAddressRanges()) {
    const SmallVector<DebugAddressRange, 2> &Ranges = CURangesPair.second;
    NewAddressTableSize += Ranges.size() * 20;
  }

  // Difference between old and new table (and section) sizes.
  // Could be negative.
  int32_t Delta = NewAddressTableSize - OldAddressTableSize;

  size_t NewGdbIndexSize = GdbIndexContents.size() + Delta;

  // Free'd by ExecutableFileMemoryManager.
  auto *NewGdbIndexContents = new uint8_t[NewGdbIndexSize];
  uint8_t *Buffer = NewGdbIndexContents;

  write32le(Buffer, Version);
  write32le(Buffer + 4, CUListOffset);
  write32le(Buffer + 8, CUTypesOffset);
  write32le(Buffer + 12, AddressTableOffset);
  write32le(Buffer + 16, SymbolTableOffset + Delta);
  write32le(Buffer + 20, ConstantPoolOffset + Delta);
  Buffer += 24;

  // Copy over CU list and types CU list.
  memcpy(Buffer, GdbIndexContents.data() + 24,
         AddressTableOffset - CUListOffset);
  Buffer += AddressTableOffset - CUListOffset;

  // Generate new address table.
  for (const std::pair<const uint64_t, DebugAddressRangesVector> &CURangesPair :
       ARangesSectionWriter->getCUAddressRanges()) {
    const uint32_t CUIndex = OffsetToIndexMap[CURangesPair.first];
    const DebugAddressRangesVector &Ranges = CURangesPair.second;
    for (const DebugAddressRange &Range : Ranges) {
      write64le(Buffer, Range.LowPC);
      write64le(Buffer + 8, Range.HighPC);
      write32le(Buffer + 16, CUIndex);
      Buffer += 20;
    }
  }

  const size_t TrailingSize =
      GdbIndexContents.data() + GdbIndexContents.size() - Data;
  assert(Buffer + TrailingSize == NewGdbIndexContents + NewGdbIndexSize &&
         "size calculation error");

  // Copy over the rest of the original data.
  memcpy(Buffer, Data, TrailingSize);

  // Register the new section.
  BC.registerOrUpdateNoteSection(".gdb_index", NewGdbIndexContents,
                                 NewGdbIndexSize);
}

void DWARFRewriter::convertToRanges(DWARFDie DIE,
                                    const DebugAddressRangesVector &Ranges,
                                    SimpleBinaryPatcher &DebugInfoPatcher) {
  uint64_t RangesSectionOffset;
  if (Ranges.empty()) {
    RangesSectionOffset = RangesSectionWriter->getEmptyRangesOffset();
  } else {
    RangesSectionOffset = RangesSectionWriter->addRanges(Ranges);
  }

  convertToRanges(DIE, RangesSectionOffset, DebugInfoPatcher);
}

void DWARFRewriter::convertPending(const DWARFUnit &Unit,
                                   const DWARFAbbreviationDeclaration *Abbrev,
                                   SimpleBinaryPatcher &DebugInfoPatcher,
                                   DebugAbbrevWriter &AbbrevWriter) {
  if (ConvertedRangesAbbrevs.count(Abbrev))
    return;

  convertToRanges(Unit, Abbrev, AbbrevWriter);

  auto I = PendingRanges.find(Abbrev);
  if (I != PendingRanges.end()) {
    for (std::pair<DWARFDieWrapper, DebugAddressRange> &Pair : I->second) {
      convertToRanges(Pair.first, {Pair.second}, DebugInfoPatcher);
    }
    PendingRanges.erase(I);
  }

  ConvertedRangesAbbrevs.emplace(Abbrev);
}

void DWARFRewriter::addToPendingRanges(
    const DWARFAbbreviationDeclaration *Abbrev, DWARFDie DIE,
    DebugAddressRangesVector &FunctionRanges, Optional<uint64_t> DWOId) {
  Optional<DWARFFormValue> LowPcValue = DIE.find(dwarf::DW_AT_low_pc);
  Optional<DWARFFormValue> HighPcValue = DIE.find(dwarf::DW_AT_high_pc);
  if (LowPcValue &&
      LowPcValue->getForm() == dwarf::Form::DW_FORM_GNU_addr_index) {
    assert(DWOId && "Invalid DWO ID.");
    (void)DWOId;
    assert(HighPcValue && "Low PC exists, but not High PC.");
    (void)HighPcValue;
    uint64_t IndexL = LowPcValue->getRawUValue();
    uint64_t IndexH = HighPcValue->getRawUValue();
    for (auto Address : FunctionRanges) {
      AddrWriter->addIndexAddress(Address.LowPC, IndexL, *DWOId);
      // 2.17.2
      // If the value of the DW_AT_high_pc is of class address, it is the
      // relocated address of the first location past the last instruction
      // associated with the entity; if it is of class constant, the value is
      // an unsigned integer offset which when added to the low PC gives the
      // address of the first location past the last instruction associated
      // with the entity.
      if (!HighPcValue->isFormClass(DWARFFormValue::FC_Constant))
        AddrWriter->addIndexAddress(Address.HighPC, IndexH, *DWOId);
    }
  }
  PendingRanges[Abbrev].emplace_back(
      std::make_pair(DWARFDieWrapper(DIE), FunctionRanges.front()));
}

std::unique_ptr<DebugBufferVector>
DWARFRewriter::makeFinalLocListsSection(SimpleBinaryPatcher &DebugInfoPatcher) {
  auto LocBuffer = std::make_unique<DebugBufferVector>();
  auto LocStream = std::make_unique<raw_svector_ostream>(*LocBuffer);
  auto Writer =
      std::unique_ptr<MCObjectWriter>(BC.createObjectWriter(*LocStream));

  uint64_t SectionOffset = 0;

  // Add an empty list as the first entry;
  const char Zeroes[16] = {0};
  *LocStream << StringRef(Zeroes, 16);
  SectionOffset += 2 * 8;

  std::unordered_map<uint64_t, uint64_t> SectionOffsetByCU(
      LocListWritersByCU.size());

  for (std::pair<const uint64_t, std::unique_ptr<DebugLocWriter>> &Loc :
       LocListWritersByCU) {
    uint64_t CUIndex = Loc.first;
    DebugLocWriter *LocWriter = Loc.second.get();
    if (llvm::isa<DebugLoclistWriter>(*LocWriter))
      continue;
    SectionOffsetByCU[CUIndex] = SectionOffset;
    std::unique_ptr<DebugBufferVector> CurrCULocationLists =
        LocWriter->finalize();
    *LocStream << *CurrCULocationLists;
    SectionOffset += CurrCULocationLists->size();
  }

  for (std::pair<const uint64_t, VectorLocListDebugInfoPatchType> &Iter :
       DwoLocListDebugInfoPatches) {
    uint64_t DWOId = Iter.first;
    SimpleBinaryPatcher *Patcher = getBinaryDWODebugInfoPatcher(DWOId);
    for (LocListDebugInfoPatchType &Patch : Iter.second) {
      Patcher->addLE32Patch(Patch.DebugInfoOffset,
                            SectionOffsetByCU[Patch.CUIndex] +
                                Patch.CUWriterOffset);
    }
  }

  for (LocListDebugInfoPatchType &Patch : LocListDebugInfoPatches) {
    DebugInfoPatcher.addLE32Patch(Patch.DebugInfoOffset,
                                  SectionOffsetByCU[Patch.CUIndex] +
                                      Patch.CUWriterOffset);
  }

  return LocBuffer;
}

void DWARFRewriter::flushPendingRanges(SimpleBinaryPatcher &DebugInfoPatcher) {
  for (std::pair<const DWARFAbbreviationDeclaration *const,
                 std::vector<std::pair<DWARFDieWrapper, DebugAddressRange>>>
           &I : PendingRanges) {
    for (std::pair<DWARFDieWrapper, DebugAddressRange> &RangePair : I.second) {
      patchLowHigh(RangePair.first, RangePair.second, DebugInfoPatcher);
    }
  }
  clearList(PendingRanges);
}

namespace {

void getRangeAttrData(DWARFDie DIE, uint64_t &LowPCOffset,
                      uint64_t &HighPCOffset, DWARFFormValue &LowPCFormValue,
                      DWARFFormValue &HighPCFormValue) {
  Optional<AttrInfo> LowPCVal = findAttributeInfo(DIE, dwarf::DW_AT_low_pc);
  Optional<AttrInfo> HighPCVal = findAttributeInfo(DIE, dwarf::DW_AT_high_pc);
  LowPCOffset = LowPCVal->Offset;
  HighPCOffset = HighPCVal->Offset;
  LowPCFormValue = LowPCVal->V;
  HighPCFormValue = HighPCVal->V;

  if ((LowPCFormValue.getForm() != dwarf::DW_FORM_addr &&
       LowPCFormValue.getForm() != dwarf::DW_FORM_GNU_addr_index) ||
      (HighPCFormValue.getForm() != dwarf::DW_FORM_addr &&
       HighPCFormValue.getForm() != dwarf::DW_FORM_data8 &&
       HighPCFormValue.getForm() != dwarf::DW_FORM_data4)) {
    errs() << "BOLT-WARNING: unexpected form value. Cannot update DIE "
           << "at offset 0x" << Twine::utohexstr(DIE.getOffset()) << "\n";
    return;
  }
  if ((LowPCOffset == -1U || (LowPCOffset + 8 != HighPCOffset)) &&
      LowPCFormValue.getForm() != dwarf::DW_FORM_GNU_addr_index) {
    errs() << "BOLT-WARNING: high_pc expected immediately after low_pc. "
           << "Cannot update DIE at offset 0x"
           << Twine::utohexstr(DIE.getOffset()) << '\n';
    return;
  }
}

} // namespace

void DWARFRewriter::patchLowHigh(DWARFDie DIE, DebugAddressRange Range,
                                 SimpleBinaryPatcher &DebugInfoPatcher) {
  uint64_t LowPCOffset, HighPCOffset;
  DWARFFormValue LowPCFormValue, HighPCFormValue;
  getRangeAttrData(DIE, LowPCOffset, HighPCOffset, LowPCFormValue,
                   HighPCFormValue);
  auto *TempDebugPatcher = &DebugInfoPatcher;
  if (LowPCFormValue.getForm() == dwarf::DW_FORM_GNU_addr_index) {
    DWARFUnit *Unit = DIE.getDwarfUnit();
    assert(Unit->isDWOUnit() && "DW_FORM_GNU_addr_index not part of DWO.");
    uint32_t AddressIndex =
        AddrWriter->getIndexFromAddress(Range.LowPC, *Unit->getDWOId());
    TempDebugPatcher = getBinaryDWODebugInfoPatcher(*Unit->getDWOId());
    TempDebugPatcher->addUDataPatch(LowPCOffset, AddressIndex,
                                    std::abs(int(HighPCOffset - LowPCOffset)));
    // TODO: In DWARF5 support ULEB128 for high_pc
  } else {
    TempDebugPatcher->addLE64Patch(LowPCOffset, Range.LowPC);
  }

  if (isHighPcFormEightBytes(HighPCFormValue.getForm())) {
    TempDebugPatcher->addLE64Patch(HighPCOffset, Range.HighPC - Range.LowPC);
  } else {
    TempDebugPatcher->addLE32Patch(HighPCOffset, Range.HighPC - Range.LowPC);
  }
}

void DWARFRewriter::convertToRanges(const DWARFUnit &Unit,
                                    const DWARFAbbreviationDeclaration *Abbrev,
                                    DebugAbbrevWriter &AbbrevWriter,
                                    Optional<uint64_t> RangesBase) {
  dwarf::Form HighPCForm = Abbrev->findAttribute(dwarf::DW_AT_high_pc)->Form;
  dwarf::Form LowPCForm = Abbrev->findAttribute(dwarf::DW_AT_low_pc)->Form;

  // DW_FORM_GNU_addr_index is already variable encoding so nothing to do
  // there. If HighForm is 8 bytes need to change low_pc to be variable
  // encoding to consume extra bytes from high_pc, since DW_FORM_sec_offset is
  // 4 bytes for DWARF32.
  // FIXME: update comments
  if (RangesBase) {
    assert(LowPCForm != dwarf::DW_FORM_GNU_addr_index);
    AbbrevWriter.addAttributePatch(Unit, Abbrev, dwarf::DW_AT_low_pc,
                                   dwarf::DW_AT_GNU_ranges_base,
                                   dwarf::DW_FORM_indirect);
  } else if (LowPCForm != dwarf::DW_FORM_GNU_addr_index &&
             isHighPcFormEightBytes(HighPCForm)) {
    AbbrevWriter.addAttributePatch(Unit, Abbrev, dwarf::DW_AT_low_pc,
                                   dwarf::DW_AT_low_pc,
                                   dwarf::DW_FORM_indirect);
  }

  AbbrevWriter.addAttributePatch(Unit, Abbrev, dwarf::DW_AT_high_pc,
                                 dwarf::DW_AT_ranges,
                                 dwarf::DW_FORM_sec_offset);
}

void DWARFRewriter::convertToRanges(DWARFDie DIE, uint64_t RangesSectionOffset,
                                    SimpleBinaryPatcher &DebugInfoPatcher,
                                    Optional<uint64_t> RangesBase) {
  uint64_t LowPCOffset, HighPCOffset;
  DWARFFormValue LowPCFormValue, HighPCFormValue;
  getRangeAttrData(DIE, LowPCOffset, HighPCOffset, LowPCFormValue,
                   HighPCFormValue);

  // Size to fill with the first field (DW_AT_low_pc or DW_AT_GNU_ranges_base).
  unsigned NumBytesToFill = 0;
  assert(DIE.getDwarfUnit()->getAddressByteSize() == 8);
  if (isHighPcFormEightBytes(HighPCFormValue.getForm())) {
    NumBytesToFill = 12;
  } else if (HighPCFormValue.getForm() == dwarf::DW_FORM_data4) {
    NumBytesToFill = 8;
  } else {
    llvm_unreachable("unexpected DW_AT_high_pc form");
  }

  std::lock_guard<std::mutex> Lock(DebugInfoPatcherMutex);
  uint32_t BaseOffset = 0;
  if (LowPCFormValue.getForm() == dwarf::DW_FORM_GNU_addr_index) {
    // Use ULEB128 for the value.
    DebugInfoPatcher.addUDataPatch(LowPCOffset, 0,
                                   std::abs(int(HighPCOffset - LowPCOffset)) +
                                       NumBytesToFill - 8);
    // Ranges are relative to DW_AT_GNU_ranges_base.
    BaseOffset = DebugInfoPatcher.getRangeBase();
  } else {
    if (RangesBase) {
      DebugInfoPatcher.addUDataPatch(LowPCOffset, dwarf::DW_FORM_udata, 1);
      DebugInfoPatcher.addUDataPatch(LowPCOffset + 1, *RangesBase,
                                     NumBytesToFill - 1);
    } else if (NumBytesToFill == 12) {
      // Creatively encoding dwarf::DW_FORM_addr in to 4 bytes.
      // Write an indirect 0 value for DW_AT_low_pc so that we can fill
      // 12 bytes of space.
      DebugInfoPatcher.addUDataPatch(LowPCOffset, dwarf::DW_FORM_addr, 4);
      DebugInfoPatcher.addLE64Patch(LowPCOffset + 4, 0);
    } else {
      // NumBytesToFill == 8
      DebugInfoPatcher.addLE64Patch(LowPCOffset, 0);
    }
  }
  DebugInfoPatcher.addLE32Patch(HighPCOffset + NumBytesToFill - 8,
                                RangesSectionOffset - BaseOffset);
}
