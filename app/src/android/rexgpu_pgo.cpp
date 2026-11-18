// ds2 - Clang PGO of the GPU plugin (DS2_PGO_SDK=generate, app/CMakeLists.txt).
//
// Compiled into librexgpu-xenos.so only in that build. The instrumented plugin carries its own copy of the
// profile runtime (hidden symbols), so the app cannot write its counters with its own __llvm_profile_write_file;
// this export does it for the plugin (called from ds2_framerate.cpp's PGO writer every 30 s, as Android kills
// apps instead of letting them exit).

extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);

extern "C" __attribute__((visibility("default"))) int rexgpu_pgo_write(const char* path) {
  __llvm_profile_set_filename(path);
  return __llvm_profile_write_file();
}
