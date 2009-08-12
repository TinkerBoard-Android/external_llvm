include(FindPerl)
include(LLVMLibDeps)

function(get_system_libs return_var)
  # Returns in `return_var' a list of system libraries used by LLVM.
  if( NOT MSVC )
    if( MINGW )
      set(system_libs ${system_libs} imagehlp psapi)
    elseif( CMAKE_HOST_UNIX )
      if( HAVE_LIBDL )
	set(system_libs ${system_libs} dl)
      endif()
      if( LLVM_ENABLE_THREADS AND HAVE_LIBPTHREAD )
	set(system_libs ${system_libs} pthread)
      endif()
    endif( MINGW )
  endif( NOT MSVC )
  set(${return_var} ${system_libs} PARENT_SCOPE)
endfunction(get_system_libs)


macro(llvm_config executable)
  explicit_llvm_config(${executable} ${ARGN})
endmacro(llvm_config)


function(explicit_llvm_config executable)
  set( link_components ${ARGN} )

  explicit_map_components_to_libraries(LIBRARIES ${link_components})
  target_link_libraries(${executable} ${LIBRARIES})
endfunction(explicit_llvm_config)


function(explicit_map_components_to_libraries out_libs)
  set( link_components ${ARGN} )
  foreach(c ${link_components})
    # add codegen, asmprinter, asmparser
    list(FIND LLVM_TARGETS_TO_BUILD ${c} idx)
    if( NOT idx LESS 0 )
      list(FIND llvm_libs "LLVM${c}CodeGen" idx)
      if( NOT idx LESS 0 )
	list(APPEND expanded_components "LLVM${c}CodeGen")
      else()
	list(FIND llvm_libs "LLVM${c}" idx)
	if( NOT idx LESS 0 )
	  list(APPEND expanded_components "LLVM${c}")
	else()
	  message(FATAL_ERROR "Target ${c} is not in the set of libraries.")
	endif()
      endif()
      list(FIND llvm_libs "LLVM${c}AsmPrinter" asmidx)
      if( NOT asmidx LESS 0 )
        list(APPEND expanded_components "LLVM${c}AsmPrinter")
      endif()
      list(FIND llvm_libs "LLVM${c}AsmParser" asmidx)
      if( NOT asmidx LESS 0 )
        list(APPEND expanded_components "LLVM${c}AsmParser")
      endif()
      list(FIND llvm_libs "LLVM${c}Info" asmidx)
      if( NOT asmidx LESS 0 )
        list(APPEND expanded_components "LLVM${c}Info")
      endif()
    elseif( c STREQUAL "native" )
      # TODO: we assume ARCH is X86. In this case, we must use nativecodegen
      # component instead. Do nothing, as in llvm-config script.
    elseif( c STREQUAL "nativecodegen" )
      # TODO: we assume ARCH is X86.
      list(APPEND expanded_components "LLVMX86CodeGen")
    elseif( c STREQUAL "backend" )
      # same case as in `native'.
    elseif( c STREQUAL "engine" )
      # TODO: as we assume we are on X86, this is `jit'.
      list(APPEND expanded_components "LLVMJIT")
    elseif( c STREQUAL "all" )
      list(APPEND expanded_components ${llvm_libs})
    else( NOT idx LESS 0 )
      list(APPEND expanded_components LLVM${c})
    endif( NOT idx LESS 0 )
  endforeach(c)
  # We must match capitalization.
  string(TOUPPER "${llvm_libs}" capitalized_libs)
  list(REMOVE_DUPLICATES expanded_components)
  set(curr_idx 0)
  list(LENGTH expanded_components lst_size)
  while( ${curr_idx} LESS ${lst_size} )
    list(GET expanded_components ${curr_idx} c)
    string(TOUPPER "${c}" capitalized)
    list(FIND capitalized_libs ${capitalized} idx)
    if( idx LESS 0 )
      message(FATAL_ERROR "Library ${c} not found in list of llvm libraries.")
    endif( idx LESS 0 )
    list(GET llvm_libs ${idx} canonical_lib)
    list(APPEND result ${canonical_lib})
    list(APPEND result ${MSVC_LIB_DEPS_${canonical_lib}})
    list(APPEND expanded_components ${MSVC_LIB_DEPS_${canonical_lib}})
    list(REMOVE_DUPLICATES expanded_components)
    list(LENGTH expanded_components lst_size)
    math(EXPR curr_idx "${curr_idx} + 1")
  endwhile( ${curr_idx} LESS ${lst_size} )
  list(REMOVE_DUPLICATES result)
  set(${out_libs} ${result} PARENT_SCOPE)
endfunction(explicit_map_components_to_libraries)
