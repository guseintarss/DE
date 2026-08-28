project('wf-json', ['cpp'],
		    version : '0.0',
		    meson_version: '>=0.55.0',
		    default_options : ['cpp_std=c++17'])

json = dependency('yyjson', required: false)
if not json.found()
	cmake = import('cmake')
	cmake_opts = cmake.subproject_options()
	cmake_opts.add_cmake_defines({'CMAKE_POSITION_INDEPENDENT_CODE': true})
	sub_proj = cmake.subproject('yyjson', options: cmake_opts)
	json = sub_proj.dependency('yyjson')
endif

wf_json_inc_dirs = include_directories('.')

if get_option('install_header') == true
    install_subdir('wayfire',
        install_dir: get_option('includedir'))
endif

wfjson_lib = static_library('wfjson', ['src/json.cpp'], install: false, dependencies: json)
wfjson = declare_dependency(link_with: wfjson_lib, include_directories: wf_json_inc_dirs, dependencies: json)
