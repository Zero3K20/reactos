add_executable(udfsbench udfsbench.c)
set_module_type(udfsbench win32cui UNICODE)
add_importlibs(udfsbench msvcrt kernel32 ntdll)
add_rostests_file(TARGET udfsbench SUBDIR suppl)
