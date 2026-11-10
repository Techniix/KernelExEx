Minimal C++ library headers for building KernelEx with clang.

KernelEx was written against the Visual C++ 6 library and uses a very small
part of it: std::map, std::list, std::vector, a few algorithms and
placement new. These headers provide just that, with no exceptions, no
allocators and no dependency beyond operator new/delete (kexcrt). They are
not a general-purpose standard library.
