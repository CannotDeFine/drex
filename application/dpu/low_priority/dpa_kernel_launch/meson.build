# Meson build for DPA kernel_launch demo.

project('DOCA_SAMPLE', 'C',
	# Get version number from file.
	version: run_command(find_program('cat'),
		files('/opt/mellanox/doca/applications/VERSION'), check: true).stdout().strip(),
	license: 'Proprietary',
	default_options: ['buildtype=debug'],
	meson_version: '>= 0.61.2'
)

SAMPLE_NAME = 'dpa_kernel_launch'
sample_program_name = SAMPLE_NAME + '_program'

# Comment this line to restore warnings of experimental DOCA features
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: ['c'])

sample_dependencies = []
# Core deps
sample_dependencies += dependency('doca-common')
sample_dependencies += dependency('doca-dpa')
sample_dependencies += dependency('doca-rdma')
sample_dependencies += dependency('doca-argp')
sample_dependencies += dependency('libflexio')
sample_dependencies += dependency('threads')

sample_srcs = [
	'host/' + SAMPLE_NAME + '_sample.c',
	SAMPLE_NAME + '_main.c',
	'../dpa_common.c',
]

sample_inc_dirs  = []
# Common DOCA library logic
sample_inc_dirs += include_directories('..')

# DPA compiler build script
dpacc_build_script_name = 'build_dpacc_samples.sh'
dpacc_build_script = meson.current_source_dir() + '/../' + dpacc_build_script_name
# Sample device source code absolute path to be compiled by the DPACC
sample_device_src = meson.project_source_root() + '/device/' +  SAMPLE_NAME + '_kernels_dev.c'

# Run DPA compiler
run_command(dpacc_build_script, meson.project_build_root(), sample_device_src, SAMPLE_NAME,
	    sample_program_name, check: true)

# Dependencies
sample_device_build_dir = meson.project_build_root() + '/' + SAMPLE_NAME + '/device/build_dpacc'
dpa_program_lib = meson.get_compiler('c').find_library(sample_program_name, dirs : sample_device_build_dir,
							static: true)

sample_dependencies += dpa_program_lib

executable('doca_' + SAMPLE_NAME, sample_srcs,
	dependencies : sample_dependencies,
	include_directories: sample_inc_dirs,
	install: false)
