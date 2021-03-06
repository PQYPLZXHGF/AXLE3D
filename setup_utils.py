# ------------------------------------------------------------------------------
# Copyright (c) 2012-2017 Adam Schackart / "AJ Hackman", all rights reserved.
# Distributed under the BSD license v2 (opensource.org/licenses/BSD-3-Clause)
# ------------------------------------------------------------------------------
# TODOS:
#  - allow setup.py to apply global attributes/config to all extensions and libs
#  - give build_shared some options (use shared_lib.debug instead of __debug__)
#  - only rebuild shared libs & EXEs if sources are modified (check timestamps)
#  - NativeExecutable option to dynamically link the CRT or copy the dynamic lib
#  - bundle debug libraries (python27_d.dll etc) with cx_freeze EXE on win32 dbg
#  - disable C++ exception handling, iterator debugging, and RTTI on all builds
#  - small code build (Oi is smaller than Os on MSVC, but issues warning D9025)
# ------------------------------------------------------------------------------
from __future__ import print_function

import distutils.command.clean
import distutils.ccompiler
import distutils

import Cython.Compiler.Options
import Cython.Distutils

import cx_Freeze

from glob import glob
import shutil
import copy
import time
import sys
import os

# ==============================================================================
# ~ [ utils ]
# ==============================================================================

if sys.version_info.major > 2 and sys.platform != 'win32':
    #
    # FIXME: in headerize, python is ignoring everything after the literal "\n"
    #
    if 0:
        def WindowsLineEndingsFile(name, mode='r'):
            return open(name, mode, newline='\r\n')
    else:
        WindowsLineEndingsFile = open

elif sys.platform != 'win32':
    class WindowsLineEndingsFile(file):
        """File object that forces CRLF (Win32-style) line endings on write."""
        def write(self, s):
            super(WindowsLineEndingsFile, self).write(s.replace('\n', '\r\n'))
else:
    WindowsLineEndingsFile = open

def headerize(path, src, dst, verbose=True):
    """Convert textfiles to string literals that can be #included in C code."""
    src_file = WindowsLineEndingsFile(os.path.join(path, src), 'r')
    dst_file = WindowsLineEndingsFile(os.path.join(path, dst), 'w')

    # TODO: make this (along with headerize_binary) a full-fledged distutils
    # command that runs before any C code is generated, compiled, or linked.

    if verbose: print('writing textfile "{}" to header "{}"' \
                        .format(src_file.name, dst_file.name))

    dst_file.write('// This file is automatically generated. Do not edit!\n\n')

    for line in src_file:
        dst_file.write('"{}\\n"\n'.format(line.strip()))

    dst_file.write(';\n')

def headerize_multi_line(path, src, dst, verbose=True):
    """Hack for MSVC's super obnoxious 65535-character string literal limit."""
    src_file = WindowsLineEndingsFile(os.path.join(path, src), 'r')
    dst_file = WindowsLineEndingsFile(os.path.join(path, dst), 'w')

    # TODO: make this (along with headerize(_binary)) a full-fledged distutils
    # command that runs before any C code is generated, compiled, or linked.

    if verbose: print('writing textfile "{}" to multi-line header "{}"' \
                                    .format(src_file.name, dst_file.name))

    dst_file.write('// This file is automatically generated. Do not edit!\n\n')

    for line in src_file:
        dst_file.write('"{}\\n",\n'.format(line.strip()))

    dst_file.write('NULL,\n')

# ==============================================================================
# ~ [ cython configuration ]
# ==============================================================================

# don't include class and function documentation in release builds
Cython.Compiler.Options.docstrings = __debug__

# don't copy cython code into generated c as comments (build speedup)
Cython.Compiler.Options.emit_code_comments = False

# abort compilation on the first error (don't keep printing errors)
Cython.Compiler.Options.fast_fail = True

# allow cimporting from a pyx file without a corresponding pxd file
Cython.Compiler.Options.cimport_from_pyx = True

# ===== [ compiler directives ] ================================================

try:
    cython_directive_defaults = Cython.Compiler.Options.get_directive_defaults()
except AttributeError:
    cython_directive_defaults = Cython.Compiler.Options.directive_defaults # old

cython_directive_defaults['boundscheck'] = __debug__
cython_directive_defaults['nonecheck'] = __debug__
cython_directive_defaults['initializedcheck'] = __debug__
# cython_directive_defaults['auto_cpdef'] = True
cython_directive_defaults['cdivision'] = not __debug__
cython_directive_defaults['cdivision_warnings'] = __debug__
cython_directive_defaults['overflowcheck'] = __debug__
# cython_directive_defaults['wraparound'] = False
cython_directive_defaults['emit_code_comments'] = False
cython_directive_defaults['annotation_typing'] = True
cython_directive_defaults['infer_types'] = True
# cython_directive_defaults['infer_types.verbose'] = True
cython_directive_defaults['autotestdict'] = __debug__
cython_directive_defaults['unraisable_tracebacks'] = __debug__

# ==============================================================================
# ~ [ info containers ]
# ==============================================================================

class Distribution(distutils.dist.Distribution):
    def __init__(self, attrs):
        self.executables = [] # cached by the constructor
        self.shared_libs = [] # shared libraries to build
        self.native_exes = [] # native C(++) applications

        distutils.dist.Distribution.__init__(self, attrs)

class Extension(Cython.Distutils.Extension):
    def apply_global_config(self):
        """Add compiler flags, macros, etc. that we want applied to all extensions. We
        assume that non-Windows platforms are Unix & have GCC-compatible compilers."""
        if sys.platform == 'win32':
            # silence bogus warnings about c standard library functions like sprintf
            self.define_macros.append(('_CRT_SECURE_NO_WARNINGS', '1'))

            # enable the windows cryptographic random number generator (RtlGenRandom)
            self.define_macros.append(('_CRT_RAND_S', '1'))

            # don't catch system exceptions in C++, and make extern "C" funcs faster
            self.extra_compile_args.append('/EHsc')

            self.extra_compile_args.append('/wd4065') # no switch cases
            self.extra_compile_args.append('/wd4101') # unused locals
            self.extra_compile_args.append('/wd4102') # unused labels
            self.extra_compile_args.append('/wd4244') # size_t -> int
            self.extra_compile_args.append('/wd4267') # size_t -> long
            self.extra_compile_args.append('/wd4334') # 32-bit shift
            self.extra_compile_args.append('/wd4985') # type annotations

            # more recent versions of cython export multiple module init functions
            self.extra_link_args.append('/ignore:4197')
        else:
            # for some esoteric, quixotic, idealistic, impractical, ivory tower reason,
            # unix linkers don't search for shared libraries in the application's path
            self.library_dirs.append('.')
            self.runtime_library_dirs.append('.')

            # silence various -Wall warnings we don't care about here. perhaps at some
            # point we should try passing -Wextra or even -pedantic and see what happens.
            self.extra_compile_args.append('-Wno-comment')
            self.extra_compile_args.append('-Wno-date-time')
            self.extra_compile_args.append('-Wno-expansion-to-defined')
            self.extra_compile_args.append('-Wno-strict-prototypes')

            # XXX: i don't want to disable this warning, but it happens in cython-generated code!
            self.extra_compile_args.append('-Wno-incompatible-pointer-types-discards-qualifiers')

            self.extra_compile_args.append('-Wno-unused-command-line-argument')
            self.extra_compile_args.append('-Wno-unused-function')
            self.extra_compile_args.append('-Wno-unused-label')
            self.extra_compile_args.append('-Wno-unused-local-typedefs')
            self.extra_compile_args.append('-Wno-unused-variable')

            self.extra_link_args.append('-Wno-unused-command-line-argument')

        # for whatever reason, distutils' unixccompiler doesn't have a different set of
        # debug arguments like in win32. XXX TODO -g is still passed in release builds.
        if sys.platform != 'win32' and __debug__:
            self.undef_macros.append('NDEBUG')
            self.extra_compile_args.append('-O0')
            self.extra_link_args.append('-O0')

        # TODO: this doesn't belong here! #define AE_DEBUG in ae_macros.h, and always
        # define it to 1 or 0 for "if (AE_DEBUG)", which will require code changes!
        # see SDL_assert.h for an example of how to detect DEBUG in the preprocessor.
        if __debug__:
            self.define_macros.append(('AE_DEBUG', '1')) # use our assert
        else:
            # disable the runtime check for "__debug__" in release builds
            self.define_macros.append(('CYTHON_WITHOUT_ASSERTIONS', '1'))

        return self

class SharedLibrary(Extension):
    pass

class NativeExecutable(Extension):
    def __init__(self, *args, **kwargs):
        Extension.__init__(self, *args, **kwargs) # can't super()

        if 'windows_subsystem' in kwargs: # windows, or console
            self.windows_subsystem = kwargs['windows_subsystem']

class Executable(cx_Freeze.Executable):
    pass

# ==============================================================================
# ~ [ commands ]
# ==============================================================================

class build_shared(distutils.core.Command):
    user_options = [] # TODO: populate with options (see TODO list above)

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):
        compiler = distutils.ccompiler.new_compiler(verbose=self.verbose,
                                dry_run=self.dry_run, force=self.force)

        # apply platform-specific configuration from environment variables
        distutils.sysconfig.customize_compiler(compiler)

        for shared_lib in self.distribution.shared_libs:
            shared_lib.apply_global_config() # apply our global attributes

            macros = shared_lib.define_macros[:] # build (un-)defines list
            for undef in shared_lib.undef_macros: macros.append((undef,))

            language = (shared_lib.language or # detect required language
                            compiler.detect_language(shared_lib.sources))

            objects = compiler.compile(shared_lib.sources, macros=macros,
                    include_dirs=shared_lib.include_dirs, debug=__debug__,
                    extra_postargs=shared_lib.extra_compile_args or [])

            compiler.link_shared_object(
                objects,
                compiler.library_filename(shared_lib.name, 'shared'),
                libraries=shared_lib.libraries,
                library_dirs=shared_lib.library_dirs,
                runtime_library_dirs=shared_lib.runtime_library_dirs,
                extra_postargs=shared_lib.extra_link_args or [],
                debug=__debug__,
                target_lang=language)

            for obj in objects: os.remove(obj)

class build_native(distutils.core.Command):
    user_options = [] # TODO: populate with options (see TODO list above)

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):
        # TODO: common path! this is mostly copied + pasted from shared lib
        compiler = distutils.ccompiler.new_compiler(verbose=self.verbose,
                                dry_run=self.dry_run, force=self.force)

        # apply platform-specific configuration from environment variables
        distutils.sysconfig.customize_compiler(compiler)

        for native_exe in self.distribution.native_exes:
            native_exe.apply_global_config() # apply our global attributes

            macros = native_exe.define_macros[:] # build (un-)defines list
            for undef in native_exe.undef_macros: macros.append((undef,))

            language = (native_exe.language or # detect required language
                            compiler.detect_language(native_exe.sources))

            objects = compiler.compile(native_exe.sources, macros=macros,
                    include_dirs=native_exe.include_dirs, debug=__debug__,
                    extra_postargs=native_exe.extra_compile_args or [])

            # we want to specify non-console mode by default on windows apps
            # XXX: there is probably a more elegant place to shoehorn this.
            # also note that this requires the use of WinMain on win32 apps!
            if native_exe.extra_link_args is None:
                native_exe.extra_link_args = []

            if sys.platform == 'win32':
                try:
                    s = native_exe.windows_subsystem.upper()
                except AttributeError:
                    s = 'CONSOLE' if __debug__ else 'WINDOWS'

                native_exe.extra_link_args.append('/SUBSYSTEM:{}'.format(s))

            compiler.link_executable(
                objects,
                native_exe.name,
                libraries=native_exe.libraries,
                library_dirs=native_exe.library_dirs,
                runtime_library_dirs=native_exe.runtime_library_dirs,
                extra_postargs=native_exe.extra_link_args,
                debug=__debug__,
                target_lang=language)

            for obj in objects: os.remove(obj)

class build_ext(Cython.Distutils.build_ext):
    def build_extension(self, ext):
        Cython.Distutils.build_ext.build_extension(self, ext.apply_global_config())

class build(cx_Freeze.build):
    def get_sub_commands(self):
        # only build exes with the "build_exe" command (it can take a few seconds)
        commands = distutils.command.build.build.get_sub_commands(self)

        # we want to build native executables after the shared libraries they use
        if self.distribution.native_exes: commands.insert(0, 'build_native')

        # we want to build shared libraries before the c extensions that use them
        if self.distribution.shared_libs: commands.insert(0, 'build_shared')

        return commands

class build_exe(cx_Freeze.build_exe):
    pass

class clean(distutils.command.clean.clean):
    def run(self):
        # just toss the entire build directory, as cx_freeze doesn't clean up exes.
        if os.path.exists('build'): shutil.rmtree('build')

        # clean up files cython leaves behind - this applies to python 2 builds only.
        # XXX TODO: the way these paths are handled is kinda hackish... clean it up!

        for name in [ os.path.splitext( extension.sources[0] )[0] for extension in \
                    self.distribution.ext_modules if '.py' in extension.sources[0]]:
            for ext in ['.c', '.cpp', '.so', '.pyd', '.pdb']:
                if os.path.exists(name + '_d' + ext): os.remove(name + '_d' + ext)
                if os.path.exists(name + ext): os.remove(name + ext)

        # clean up files cython leaves behind - this applies to python 3 builds only.
        # XXX TODO: the way these paths are handled is kinda hackish... clean it up!

        for path in [extension.name.split('.')[0] for extension in \
                                    self.distribution.ext_modules]:
            for name in os.listdir(path):
                for ext in ['.so', '.pyd']:
                    if name.endswith(ext): os.remove(os.path.join(path, name))

        # clean up after build_shared. if you need libs that are deleted by this,
        # then you must copy them from some platform-specific path before build.
        if sys.platform == 'win32':
            for name in [s for s in glob("*.dll") + glob("*.manifest") +
                                    glob("*.pdb") + glob("*.exp") +
                                    glob("*.lib") if "python" not in s]:
                os.remove(name)
        else:
            for name in glob("*.so"): os.remove(name)

        # clean up after build_native. only windows programs have file extensions!
        for name in [exe.name for exe in self.distribution.native_exes]:
            if sys.platform == 'win32': name += '.exe'

            # TODO: figure out a better way of determining platform EXE extension
            if os.path.exists(name): os.remove(name)

        # delete serialized python bytecode files throughout the entire source tree,
        # and clean up shared library build metadata left behind by visual studio.
        for dirpath, dirnames, filenames in os.walk('.'):

            # python 2 puts serialized bytecode files inline with package .py files.
            for filename in filenames:
                if (filename.endswith('.manifest') or
                    filename.endswith('.pyc') or
                    filename.endswith('.pyo') ):
                    os.remove(os.path.join(dirpath, filename))

            # python 3 puts serialized bytecode files into a __pycache__ directory.
            if '__pycache__' in dirpath: shutil.rmtree(dirpath)

        distutils.command.clean.clean.run(self) # delete temporary build directories

# ==============================================================================
# ~ [ setup ]
# ==============================================================================

def setup(**options):
    # don't pollute the user config with ours (in case they want to setup twice)
    kwargs = copy.deepcopy(options)

    kwargs.setdefault('distclass', Distribution)
    kwargs.setdefault('cmdclass', {})

    kwargs['cmdclass'].setdefault('build_shared', build_shared)
    kwargs['cmdclass'].setdefault('build_ext', build_ext)
    kwargs['cmdclass'].setdefault('build_native', build_native)
    kwargs['cmdclass'].setdefault('build', build)
    kwargs['cmdclass'].setdefault('build_exe', build_exe)
    kwargs['cmdclass'].setdefault('clean', clean)

    # avoid trying to iterate over Nones where we expect these attrs to be lists
    kwargs.setdefault('ext_modules', [])
    kwargs.setdefault('shared_libs', [])
    kwargs.setdefault('native_exes', [])

    kwargs.setdefault('options', {})

    kwargs['options'].setdefault('build_ext', {}) # build exts in code directory
    kwargs['options']['build_ext'].setdefault('inplace', True)

    # NOTE: Using debug command-line debug options requires that we link against
    # python27_d.(lib/dll) and use python_d.exe. For Python 2.7 and Visual Studio
    # 2008, open Python2.7.xx/PC/VS9.0/pcbuild.sln and build however you like.
    # If you use ctypes, you must also copy _ctypes_d.pyd into the main directory.
    kwargs['options']['build_ext'].setdefault('debug', __debug__)

    kwargs['options'].setdefault('build_exe', {}) # include the CRT and pass -OO
    kwargs['options']['build_exe'].setdefault('include_msvcr', True)
    kwargs['options']['build_exe'].setdefault('optimize', 0 if __debug__ else 2)

    # better linux compiler - it seems that distutils resets these during import.
    # we leave an optional backdoor here in case you have some other preference.
    # TODO: set LDSHARED as well? OSX requires this for CC overrides, does linux?
    # distutils' unixccompiler does a literal check for "GCC" & links differently
    # in that scenario, but everything seems to compile and link fine regardless.
    if sys.platform.startswith('linux') and getattr(sys, 'linux_clang', True):
        os.environ['CC' ] = 'clang'
        os.environ['CXX'] = 'clang++'

    start_time = time.time( )
    cx_Freeze.setup(**kwargs)

    print("Completed in", time.time() - start_time, "seconds.")
