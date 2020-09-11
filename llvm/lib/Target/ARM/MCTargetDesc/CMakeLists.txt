add_llvm_component_library(LLVMARMDesc
  ARMAsmBackend.cpp
  ARMELFObjectWriter.cpp
  ARMELFStreamer.cpp
  ARMInstPrinter.cpp
  ARMMachObjectWriter.cpp
  ARMMachORelocationInfo.cpp
  ARMMCAsmInfo.cpp
  ARMMCCodeEmitter.cpp
  ARMMCExpr.cpp
  ARMMCNaClExpander.cpp # LOCALMOD
  ARMMCTargetDesc.cpp
  ARMTargetStreamer.cpp
  ARMUnwindOpAsm.cpp
  ARMWinCOFFObjectWriter.cpp
  ARMWinCOFFStreamer.cpp
  )
