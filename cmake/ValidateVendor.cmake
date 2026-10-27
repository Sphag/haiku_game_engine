# Validates third_party/manifest.json against the vendored files.
#
#   cmake -P cmake/ValidateVendor.cmake   standalone check
#   include() + engine_validate_vendor()  used by the root CMakeLists.txt at configure time
#
# Checks every listed file for presence and SHA-256, rejects files under a
# dependency root that the manifest does not list, and checks the revision,
# URL, license, patch, and dependsOn entries. It also enforces the repository
# rules from US-002: no submodules, no Git LFS, no Git metadata inside
# third_party/, no prohibited engine dependencies, and an explicit list of
# system ABI exceptions. Performs no network access.

# IN_LIST needs CMP0057 NEW; the root project gets it from cmake_minimum_required,
# but the standalone "cmake -P" run does not.
cmake_policy(SET CMP0057 NEW)

function(engine_validate_vendor vendor_dir)
  if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR
      "ValidateVendor requires CMake 3.28 or newer; found ${CMAKE_VERSION}.\n"
      "Install a newer CMake and rerun this check.")
  endif()

  set(manifest "${vendor_dir}/manifest.json")
  if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR "Vendor manifest is missing: ${manifest}")
  endif()

  file(READ "${manifest}" json)
  string(JSON schema_version ERROR_VARIABLE json_error GET "${json}" schemaVersion)
  if(json_error)
    message(FATAL_ERROR "Vendor manifest is not valid JSON or has no schemaVersion: ${json_error}")
  endif()
  if(NOT schema_version EQUAL 1)
    message(FATAL_ERROR "Unsupported vendor manifest schemaVersion '${schema_version}' (expected 1).")
  endif()

  string(JSON dependency_count LENGTH "${json}" dependencies)
  if(dependency_count EQUAL 0)
    message(FATAL_ERROR "Vendor manifest lists no dependencies.")
  endif()

  set(errors "")
  set(file_total 0)

  # Repository rules: sources are ordinary files, never submodules or LFS pointers.
  get_filename_component(repo_root "${vendor_dir}/.." ABSOLUTE)
  if(EXISTS "${repo_root}/.gitmodules" OR EXISTS "${vendor_dir}/.gitmodules")
    string(APPEND errors "repository uses submodules (.gitmodules); vendor the sources as ordinary files\n")
  endif()
  if(EXISTS "${repo_root}/.gitattributes")
    file(READ "${repo_root}/.gitattributes" repo_attributes)
    if(repo_attributes MATCHES "filter=lfs")
      string(APPEND errors ".gitattributes routes files through Git LFS; vendored sources must be ordinary files\n")
    endif()
  endif()
  file(GLOB vendor_git_entries LIST_DIRECTORIES TRUE "${vendor_dir}/*/.git" "${vendor_dir}/*/.gitmodules")
  if(vendor_git_entries)
    string(APPEND errors "vendored roots contain Git metadata: ${vendor_git_entries}\n")
  endif()

  # Prohibited engine dependencies and explicit system ABI exceptions come from the manifest.
  string(JSON prohibited_count ERROR_VARIABLE prohibited_error LENGTH "${json}" prohibitedDependencies)
  if(prohibited_error OR prohibited_count EQUAL 0)
    string(APPEND errors "manifest has no prohibitedDependencies list\n")
    set(prohibited_count 0)
  endif()
  set(prohibited_words "")
  if(prohibited_count GREATER 0)
    math(EXPR last_prohibited "${prohibited_count} - 1")
    foreach(prohibited_index RANGE ${last_prohibited})
      string(JSON word GET "${json}" prohibitedDependencies ${prohibited_index})
      string(TOLOWER "${word}" word)
      list(APPEND prohibited_words "${word}")
    endforeach()
  endif()

  string(JSON abi_count ERROR_VARIABLE abi_error LENGTH "${json}" systemAbiExceptions)
  if(abi_error OR abi_count EQUAL 0)
    string(APPEND errors "manifest has no systemAbiExceptions list; list native ABIs that are not vendored\n")
  endif()

  # Names and roots first, so dependsOn references can be checked against the whole manifest.
  set(dep_names "")
  set(dep_roots "")
  math(EXPR last_dependency "${dependency_count} - 1")
  foreach(dep_index RANGE ${last_dependency})
    string(JSON dep GET "${json}" dependencies ${dep_index})
    string(JSON dep_name ERROR_VARIABLE _ignored GET "${dep}" name)
    string(JSON dep_root ERROR_VARIABLE _ignored GET "${dep}" root)
    list(APPEND dep_names "${dep_name}")
    if(dep_root IN_LIST dep_roots)
      string(APPEND errors "${dep_name}: root ${dep_root} is shared with another dependency\n")
    endif()
    list(APPEND dep_roots "${dep_root}")

    string(TOLOWER "${dep_name} ${dep_root}" dep_haystack)
    foreach(word IN LISTS prohibited_words)
      if(dep_haystack MATCHES "${word}")
        string(APPEND errors "${dep_name}: prohibited dependency (${word}); the PRD forbids it\n")
      endif()
    endforeach()
  endforeach()

  foreach(dep_index RANGE ${last_dependency})
    string(JSON dep GET "${json}" dependencies ${dep_index})

    foreach(field name version url revision license licenseFile root)
      string(JSON value ERROR_VARIABLE field_error GET "${dep}" ${field})
      if(field_error OR "${value}" STREQUAL "")
        string(APPEND errors "dependency #${dep_index}: missing or empty '${field}'\n")
      endif()
      set(${field} "${value}")
    endforeach()
    set(license_file "${licenseFile}")

    string(LENGTH "${revision}" revision_length)
    if(NOT url MATCHES "^https://" OR NOT revision MATCHES "^[0-9a-f]+$" OR NOT revision_length EQUAL 40)
      string(APPEND errors "${name}: url must be https:// and revision a 40-character commit hash\n")
    endif()

    foreach(array_field patches dependsOn)
      string(JSON _ ERROR_VARIABLE array_error GET "${dep}" ${array_field})
      if(array_error)
        string(APPEND errors "${name}: missing '${array_field}' array\n")
      endif()
    endforeach()

    # Every parent/dependency relationship must name another manifest entry.
    string(JSON depends_count ERROR_VARIABLE depends_error LENGTH "${dep}" dependsOn)
    if(NOT depends_error AND depends_count GREATER 0)
      math(EXPR last_depends "${depends_count} - 1")
      foreach(depends_index RANGE ${last_depends})
        string(JSON parent GET "${dep}" dependsOn ${depends_index})
        if(NOT parent IN_LIST dep_names)
          string(APPEND errors "${name}: dependsOn '${parent}' is not a manifest dependency\n")
        endif()
      endforeach()
    endif()

    string(JSON file_count LENGTH "${dep}" files)
    if(file_count EQUAL 0)
      string(APPEND errors "${name}: manifest lists no files\n")
    endif()

    set(listed_paths "")
    set(license_listed FALSE)
    if(file_count GREATER 0)
      math(EXPR last_file "${file_count} - 1")
      foreach(file_index RANGE ${last_file})
        string(JSON entry GET "${dep}" files ${file_index})
        string(JSON rel_path GET "${entry}" path)
        string(JSON expected_hash GET "${entry}" sha256)

        get_filename_component(abs_path "${vendor_dir}/${rel_path}" ABSOLUTE)
        list(APPEND listed_paths "${abs_path}")
        if("${rel_path}" STREQUAL "${license_file}")
          set(license_listed TRUE)
        endif()

        if(NOT EXISTS "${abs_path}")
          string(APPEND errors "${name}: missing vendored file ${rel_path}\n")
          continue()
        endif()
        file(SHA256 "${abs_path}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
          string(APPEND errors "${name}: modified file ${rel_path}\n")
        endif()
      endforeach()
    endif()
    math(EXPR file_total "${file_total} + ${file_count}")

    if(NOT license_listed)
      string(APPEND errors "${name}: licenseFile ${license_file} is not in the file list\n")
    endif()

    # Local patches are applied to listed sources; each patch file is itself listed and hashed.
    string(JSON patch_count ERROR_VARIABLE patch_error LENGTH "${dep}" patches)
    if(NOT patch_error AND patch_count GREATER 0)
      math(EXPR last_patch "${patch_count} - 1")
      foreach(patch_index RANGE ${last_patch})
        string(JSON patch GET "${dep}" patches ${patch_index})
        string(JSON patch_path GET "${patch}" path)
        string(JSON patch_hash GET "${patch}" sha256)
        get_filename_component(patch_abs "${vendor_dir}/${patch_path}" ABSOLUTE)
        if(NOT EXISTS "${patch_abs}")
          string(APPEND errors "${name}: missing patch ${patch_path}\n")
          continue()
        endif()
        file(SHA256 "${patch_abs}" patch_actual)
        if(NOT patch_actual STREQUAL patch_hash)
          string(APPEND errors "${name}: modified patch ${patch_path}\n")
        endif()
        if(NOT patch_abs IN_LIST listed_paths)
          string(APPEND errors "${name}: patch ${patch_path} is not in the file list\n")
        endif()
      endforeach()
    endif()

    # Anything else under the dependency root is not covered by the manifest.
    file(GLOB_RECURSE present_files LIST_DIRECTORIES FALSE "${vendor_dir}/${root}/*")
    foreach(present IN LISTS present_files)
      get_filename_component(present_abs "${present}" ABSOLUTE)
      list(FIND listed_paths "${present_abs}" listed_index)
      if(listed_index EQUAL -1)
        string(APPEND errors "${name}: unlisted file ${present_abs}\n")
      endif()
    endforeach()
  endforeach()

  if(NOT errors STREQUAL "")
    message(FATAL_ERROR
      "Vendor validation failed:\n${errors}"
      "Restore the files from the pinned revision, or update third_party/manifest.json "
      "after reviewing the change.")
  endif()

  message(STATUS "Vendor validation passed: ${dependency_count} dependencies, ${file_total} files")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  get_filename_component(_engine_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  engine_validate_vendor("${_engine_repo_root}/third_party")
endif()
