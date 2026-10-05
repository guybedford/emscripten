/**
 * @license
 * Copyright 2011 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 */

import * as os from 'node:os';
import * as fs from 'node:fs';
import * as path from 'node:path';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert';

import {
  debugLog,
  isDecorator,
  isJsOnlySymbol,
  error,
  readFile,
  pushCurrentFile,
  popCurrentFile,
  addToCompileTimeContext,
  runInMacroContext,
  mergeInto,
  localFile,
  timer,
} from './utility.mjs';
import {preprocess, processMacros} from './parseTools.mjs';

// Various namespace-like modules

// List of symbols that were added from the library.
export const librarySymbols = [];
// Library symbols exported via the `__export` decorator.
export const extraExports = new Set();
// Map of library symbols which are aliases for native symbols
// e.g. `wasmTable` -> `__indirect_function_table`
export const nativeAliases = {};

const srcDir = fileURLToPath(new URL('.', import.meta.url));
const systemLibdir = path.join(srcDir, 'lib');

function sigToArgs(sig) {
  return Array.from({length: sig.length - 1}, (_, i) => `a${i + 1}`).join(', ');
}

function isBeneath(childPath, parentPath) {
  const relativePath = path.relative(parentPath, childPath);
  return !relativePath.startsWith('..') && !path.isAbsolute(relativePath);
}

function calculateLibraries() {
  // Core system libraries (always linked against)
  let libraries = [
    'libint53.js',
    'libcore.js',
    'libsigs.js',
    'libccall.js',
    'libaddfunction.js',
    'libgetvalue.js',
    'libmath.js',
    'libpath.js',
    'libstrings.js',
    'libhtml5.js',
    'libstack_trace.js',
    'libwasi.js',
    'libeventloop.js',
    'libpromise.js',
  ];

  if (LINK_AS_CXX) {
    if (DISABLE_EXCEPTION_THROWING && !WASM_EXCEPTIONS) {
      libraries.push('libexceptions_stub.js');
    } else {
      libraries.push('libexceptions.js');
    }
  }

  if (!WASM_EXCEPTIONS) {
    libraries.push('libunwind.js');
  }

  if (!MINIMAL_RUNTIME) {
    libraries.push('libbrowser.js');
    libraries.push('libwget.js');
  }

  if (!STANDALONE_WASM) {
    libraries.push('libtime.js');
  }

  if (SUPPORT_BASE64_EMBEDDING || ENVIRONMENT_MAY_BE_SHELL) {
    libraries.push('libbase64.js');
  }

  if (AUTODEBUG) {
    libraries.push('libautodebug.js');
  }

  if (!WASMFS) {
    libraries.push('libfd.js');
    libraries.push('libsyscall.js');
  }

  if (MAIN_MODULE) {
    libraries.push('libdylink.js');
  }

  if (FILESYSTEM) {
    libraries.push('libfs_shared.js');
    if (WASMFS) {
      libraries.push(
        'libwasmfs.js',
        'libwasmfs_js_file.js',
        'libwasmfs_jsimpl.js',
        'libwasmfs_fetch.js',
        'libwasmfs_node.js',
        'libwasmfs_opfs.js',
      );
    } else {
      // Core filesystem libraries (always linked against, unless -sFILESYSTEM=0 is specified)
      libraries.push(
        'libfs.js',
        'libmemfs.js',
        'libtty.js',
        'libpipefs.js', // ok to include it by default since it's only used if the syscall is used
        'libsockfs.js', // ok to include it by default since it's only used if the syscall is used
        'libepoll.js', // ok to include it by default since it's only used if the syscall is used
      );

      if (NODERAWSOCKETS) {
        libraries.push('libsockfs_node.js');
      }

      if (NODERAWFS) {
        // NODERAWFS requires NODEFS
        libraries.push('libnodefs.js');
        libraries.push('libnoderawfs.js');
        // NODERAWFS overwrites libpath.js
        libraries.push('libnodepath.js');
      }
    }
  }

  // Additional JS libraries (without AUTO_JS_LIBRARIES, link to these explicitly via -lxxx.js)
  if (AUTO_JS_LIBRARIES) {
    libraries.push(
      'libwebgl.js',
      'libhtml5_webgl.js',
      'libopenal.js',
      'libglut.js',
      'libxlib.js',
      'libegl.js',
      'libuuid.js',
      'libglew.js',
      'libidbstore.js',
      'libasync.js',
    );
    if (USE_SDL != 2) {
      libraries.push('libsdl.js');
    }
  } else {
    if (ASYNCIFY) {
      libraries.push('libasync.js');
    }
    if (USE_SDL == 1) {
      libraries.push('libsdl.js');
    }
    if (USE_SDL == 2) {
      libraries.push('libegl.js', 'libwebgl.js', 'libhtml5_webgl.js');
    }
  }

  if (USE_GLFW) {
    libraries.push('libglfw.js');
  }

  if (LZ4) {
    libraries.push('liblz4.js');
  }

  if (SHARED_MEMORY) {
    libraries.push('libatomic.js');
  }

  if (MAX_WEBGL_VERSION >= 2) {
    // libwebgl2.js must be included only after libwebgl.js, so if we are
    // about to include libwebgl2.js, first squeeze in libwebgl.js.
    libraries.push('libwebgl.js');
    libraries.push('libwebgl2.js');
  }

  if (GL_EXPLICIT_UNIFORM_LOCATION || GL_EXPLICIT_UNIFORM_BINDING) {
    libraries.push('libc_preprocessor.js');
  }

  if (LEGACY_GL_EMULATION) {
    libraries.push('libglemu.js');
  }

  if (!STRICT) {
    libraries.push('liblegacy.js');
  }

  if (BOOTSTRAPPING_STRUCT_INFO) {
    libraries = ['libbootstrap.js', 'libstrings.js', 'libint53.js'];
  }

  if (SUPPORT_BIG_ENDIAN) {
    libraries.push('liblittle_endian_heap.js');
  }

  // Resolve system libraries
  libraries = libraries.map((filename) => path.join(systemLibdir, filename));

  // Add all user specified JS library files to the link.
  // These must be added last after all Emscripten-provided system libraries
  // above, so that users can override built-in JS library symbols in their
  // own code.
  libraries.push(...JS_LIBRARIES);

  // Deduplicate libraries to avoid processing any library file multiple times
  libraries = [...new Set(libraries)]

  return libraries;
}

let tempDir;

function getTempDir() {
  if (!tempDir) {
    const tempRoot = os.tmpdir();
    tempDir = fs.mkdtempSync(path.join(tempRoot, 'emcc-jscompiler-'));
  }
  return tempDir;
}

function preprocessFiles(filenames) {
  timer.start('preprocessFiles')
  const results = {};
  for (const filename of filenames) {
    debugLog(`pre-processing JS library: ${filename}`);
    pushCurrentFile(filename);
    try {
      results[filename] = processMacros(preprocess(filename), filename);
    } catch (e) {
      error(`error preprocessing JS library "${filename}":`);
      throw e;
    } finally {
      popCurrentFile();
    }
  }
  timer.stop('preprocessFiles')
  return results;
}

export const LibraryManager = {
  library: {},
  // The JS and JS docs of each library definition indexed my mangled name.
  libraryDefinitions: {},
  structs: {},
  loaded: false,
  libraries: [],

  has(name) {
    if (!path.isAbsolute(name)) {
      // Our libraries used to be called `library_xxx.js` rather than
      // `lib_xx.js`.  In case we have external code using this function
      // we check for the old form too.
      if (name.startsWith('library_')) {
        name = name.replace('library_', 'lib');
      }
      name = path.join(systemLibdir, name);
    }
    return this.libraries.includes(name);
  },

  load() {
    timer.start('load')

    assert(!this.loaded);
    this.loaded = true;
    // Save the list for has() queries later.
    this.libraries = calculateLibraries();

    const preprocessed = preprocessFiles(this.libraries);

    timer.start('executeJS')
    for (const [filename, contents] of Object.entries(preprocessed)) {
      this.executeJSLibraryFile(filename, contents);
    }
    timer.stop('executeJS')

    this.expandResultForms();
    this.addAliasDependencies();

    timer.stop('load')
  },

  /**
   * Result forms of an `__async: 'auto'` function. Such a function returns a
   * pointer-sized value or a Promise of one, as its trailing `canWait`
   * argument allows (see jsifier). An alias of it named with a form suffix
   * returns the same result in another shape, and so can always wait:
   *
   *   foo_fd: 'foo',       // an fd readable once settled; read() is the value
   *                        // (see $fdFromPromise)
   *   foo_promise: 'foo',  // an em_promise_t on the calling thread
   *
   * The body runs on the main thread (`__proxy: 'sync'`). On a pthread the
   * promise form is built on the fd form through emscripten_fd_promise, since
   * descriptors are process-wide and promises per-thread.
   */
  expandResultForms() {
    const forms = new Map();
    for (const [key, value] of Object.entries(this.library)) {
      if (isDecorator(key) || typeof value != 'string') continue;
      if (!/_(fd|promise)$/.test(key) || this.library[value + '__async'] != 'auto') continue;
      if (!forms.has(value)) forms.set(value, []);
      forms.get(value).push(key);
    }

    for (const [base, aliases] of forms) {
      const impl = this.library[base];
      const sig = this.library[base + '__sig'];
      assert(typeof impl == 'function', `${base}: result forms need a function body`);
      assert(sig, `${base}: result forms need ${base}__sig`);
      assert(sig[0] == 'p' || sig[0] == 'i', `${base}: result forms need a pointer-sized result ('p' or 'i' return in __sig)`);
      assert((this.library[base + '__proxy'] ?? 'sync') == 'sync', `${base}: result forms need a sync-proxied body (it runs on the main thread)`);
      const args = sigToArgs(sig);
      // Whether the body names the trailing canWait (see jsifier).
      const takesCanWait = impl.length == sig.length;

      // Share the body between the forms. Internal unless it came from a user
      // library, whose forms would otherwise warn about depending on it.
      const implName = `$${base}Impl`;
      this.library[implName] = impl;
      this.library[implName + '__deps'] = this.library[base + '__deps'] ?? [];
      if (!this.library[base + '__user']) this.library[implName + '__internal'] = true;
      const callImpl = (canWait) => `${base}Impl(${args}${takesCanWait ? `${args ? ', ' : ''}${canWait}` : ''})`;
      const define = (name, text, deps, sig, proxy) => {
        this.library[name] = runInMacroContext(`(${text})`, {filename: `<${name}>`});
        this.library[name + '__deps'] = deps;
        this.library[name + '__sig'] = sig;
        this.library[name + '__proxy'] = proxy;
      };
      // The sync form keeps its 'auto' wrapping, which supplies canWait.
      const syncArgs = takesCanWait ? `${args}${args ? ', ' : ''}canWait` : args;
      define(base, `(${syncArgs}) => ${callImpl('canWait')}`, [implName], sig, 'sync');

      for (const name of aliases) {
        const aliasSig = this.library[name + '__sig'];
        if (name.endsWith('_fd')) {
          const fdSig = 'i' + sig.slice(1);
          assert(!aliasSig || aliasSig == fdSig, `${name}__sig must be '${fdSig}'`);
          // The descriptor table is the JS one; WASMFS has no readiness layer.
          if (WASMFS) {
            define(name, `(${args}) => abort('${name} is not supported with WASMFS')`, [], fdSig, 'none');
          } else {
            define(name, `(${args}) => fdFromPromise(${callImpl('true')})`, [implName, '$fdFromPromise'], fdSig, 'sync');
          }
        } else {
          const promiseSig = 'p' + sig.slice(1);
          assert(!aliasSig || aliasSig == promiseSig, `${name}__sig must be '${promiseSig}'`);
          const fdForm = aliases.find((a) => a.endsWith('_fd'));
          assert(!PTHREADS || fdForm, `${name}: a promise form needs an fd form of ${base} under PTHREADS`);
          define(name, `(${args}) => {
  ${PTHREADS ? `if (ENVIRONMENT_IS_PTHREAD) {
    var fd = _${fdForm}(${args});
    var id = _emscripten_fd_promise(fd, ${cDefine('POLLIN')});
    return addPromise(getPromise(id).then((revents) => {
      var value = __emscripten_fd_take_result(fd);
      if (revents & ${cDefine('POLLERR')}) throw 0;
      return value;
    }).finally(() => _emscripten_promise_destroy(id)));
  }` : ''}
  return addPromise(Promise.resolve(${callImpl('true')}));
}`, [implName, '$addPromise', '$getPromise',
            ...(PTHREADS ? [fdForm, 'emscripten_fd_promise', '_emscripten_fd_take_result', 'emscripten_promise_destroy'] : [])],
            promiseSig, 'none');
        }
      }
    }
  },

  isAlias(entry) {
    return (typeof entry == 'string' && entry[0] != '=' && (this.library.hasOwnProperty(entry) || WASM_EXPORTS.has(entry)));
  },

  /**
   * Automatically add the target of an alias to it's dependency list.
   */
  addAliasDependencies() {
    const aliases = {};
    for (const [key, value] of Object.entries(this.library)) {
      if (this.isAlias(value)) {
        aliases[key] = value;
      }
    }
    for (const [key, value] of Object.entries(aliases)) {
      (this.library[key + '__deps'] ??= []).push(value);
    }
  },

  executeJSLibraryFile(filename, contents) {
    const userLibraryProxy = new Proxy(this.library, {
      set(target, prop, value) {
        target[prop] = value;
        if (!isDecorator(prop)) {
          target[prop + '__user'] = true;
        }
        return true;
      },
    });

    const isUserLibrary = !isBeneath(filename, systemLibdir);
    if (isUserLibrary) {
      debugLog(`executing user JS library: ${filename}`);
    } else {
      debugLog(`exectuing system JS library: ${filename}`);
    }

    let origLibrary;
    // When we parse user libraries also set `__user` attribute
    // on each element so that we can distinguish them later.
    if (isUserLibrary) {
      origLibrary = this.library;
      this.library = userLibraryProxy;
    }
    pushCurrentFile(filename);
    let preprocessedName = filename.replace(/\.\w+$/, '.preprocessed$&')
    if (VERBOSE) {
      preprocessedName = path.join(getTempDir(), path.basename(filename));
    }

    try {
      runInMacroContext(contents, {filename: preprocessedName})
    } catch (e) {
      error(`failure to execute JS library "${filename}":`);
      if (VERBOSE) {
        fs.writeFileSync(preprocessedName, contents);
        error(`preprocessed JS saved to ${preprocessedName}`)
      } else {
        error('use -sVERBOSE to save preprocessed JS');
      }
      throw e;
    } finally {
      popCurrentFile();
      if (origLibrary) {
        this.library = origLibrary;
      }
    }
    if (VERBOSE) {
      fs.rmSync(getTempDir(), { recursive: true, force: true });
    }
  }
};

// options is optional input object containing mergeInto params
// currently, it can contain
//
// key: noOverride, value: true
// if it is set, it prevents symbol redefinition and shows error
// in case of redefinition
//
// key: checkSig, value: true
// if it is set, __sig is checked for functions and error is reported
// if <function name>__sig is missing
function addToLibrary(obj, options = null) {
  mergeInto(LibraryManager.library, obj, options);
}

let structs = {};
let defines = {};

/**
 * Read JSON file containing struct and macro/define information
 * that can then be used in JavaScript via macros.
 */
function loadStructInfo(filename) {
  const temp = JSON.parse(readFile(filename));
  Object.assign(structs, temp.structs);
  Object.assign(defines, temp.defines);
}

if (!BOOTSTRAPPING_STRUCT_INFO) {
  // Load struct and define information.
  if (MEMORY64) {
    loadStructInfo(localFile('struct_info_generated_wasm64.json'));
  } else {
    loadStructInfo(localFile('struct_info_generated.json'));
  }
}

// Use proxy objects for C_DEFINES and C_STRUCTS so that we can give useful
// error messages.
const C_STRUCTS = new Proxy(structs, {
  get(target, prop) {
    if (!(prop in target)) {
      throw new Error(
        `Missing C struct ${prop}! If you just added it to struct_info.json, you need to run ./tools/gen_struct_info.py (then run a second time with --wasm64)`,
      );
    }
    return target[prop];
  },
});

const C_DEFINES = new Proxy(defines, {
  get(target, prop) {
    if (!(prop in target)) {
      throw new Error(
        `Missing C define ${prop}! If you just added it to struct_info.json, you need to run ./tools/gen_struct_info.py (then run a second time with --wasm64)`,
      );
    }
    return target[prop];
  },
});

// shorter alias for C_DEFINES
const cDefs = C_DEFINES;

// Legacy function that existed solely to give error message.  These are now
// provided by the cDefs proxy object above.
function cDefine(key) {
  return cDefs[key];
}

function isInternalSymbol(ident) {
  return ident + '__internal' in LibraryManager.library;
}

function getUnusedLibrarySymbols() {
  const librarySymbolSet = new Set(librarySymbols);
  const missingSyms = new Set();
  for (const [ident, value] of Object.entries(LibraryManager.library)) {
    if (typeof value === 'function' || typeof value === 'number') {
      if (isJsOnlySymbol(ident) && !isDecorator(ident) && !isInternalSymbol(ident)) {
        const name = ident.slice(1);
        if (!librarySymbolSet.has(name)) {
          missingSyms.add(name);
        }
      }
    }
  }
  return missingSyms;
}

// When running with ASSERTIONS enabled we create stubs for each library
// function that that was not included in the build.  This gives useful errors
// when library dependencies are missing from `__deps` or depended on without
// being added to DEFAULT_LIBRARY_FUNCS_TO_INCLUDE
// TODO(sbc): These errors could potentially be generated at build time via
// some kind of acorn pass that searched for uses of these missing symbols.
function addMissingLibraryStubs(unusedLibSymbols) {
  let rtn = '';
  rtn += 'var missingLibrarySymbols = [\n';
  for (const sym of unusedLibSymbols) {
    rtn += `  '${sym}',\n`;
  }
  rtn += '];\n';
  rtn += 'missingLibrarySymbols.forEach(missingLibrarySymbol)\n';
  return rtn;
}

function exportSymbol(name) {
  // In MODULARIZE=instance mode symbols are exported by being included in
  // an export { foo, bar } list so we build up the simple list of names
  if (MODULARIZE === 'instance') {
    return name;
  }
  return `Module['${name}'] = ${name};`;
}

// export parts of the JS runtime that the user asked for
function exportRuntimeSymbols() {
  // optionally export something.
  function shouldExport(name) {
    // Native exports are not available to be exported initially.  Instead,
    // they get exported later in `assignWasmExports`.
    if (nativeAliases[name]) {
      return false;
    }
    // If requested to be exported, export it.
    if (EXPORTED_RUNTIME_METHODS.has(name)) {
      // Unless we are in MODULARIZE=instance mode then HEAP objects are
      // exported separately in updateMemoryViews
      if (MODULARIZE == 'instance' || !name.startsWith('HEAP')) {
        return true;
      }
    }
    return false;
  }

  // All possible runtime elements that can be exported
  let runtimeElements = [
    'run',
    'out',
    'err',
    'callMain',
    'abort',
    'wasmExports',
  ];

  if (SUPPORT_BIG_ENDIAN) {
    runtimeElements.push('HEAP_DATA_VIEW');
  }

  if (LOAD_SOURCE_MAP) {
    runtimeElements.push('WasmSourceMap');
  }

  if (STACK_OVERFLOW_CHECK) {
    runtimeElements.push('writeStackCookie');
    runtimeElements.push('checkStackCookie');
  }

  if (RETAIN_COMPILER_SETTINGS) {
    runtimeElements.push('getCompilerSetting');
  }

  if (RUNTIME_DEBUG) {
    runtimeElements.push('prettyPrint');
  }

  // dynCall_* methods are not hardcoded here, as they
  // depend on the file being compiled. check for them
  // and add them.
  for (const name of EXPORTED_RUNTIME_METHODS) {
    if (/^dynCall_/.test(name)) {
      // a specific dynCall; add to the list
      runtimeElements.push(name);
    }
  }

  // Add JS library elements such as FS, GL, ENV, etc. These are prefixed with
  // '$ which indicates they are JS methods.
  let runtimeElementsSet = new Set(runtimeElements);
  for (const ident of Object.keys(LibraryManager.library)) {
    if (isJsOnlySymbol(ident) && !isDecorator(ident) && !isInternalSymbol(ident)) {
      const jsname = ident.slice(1);
      // Note that this assertion may be hit when a function is moved into the
      // JS library. In that case the function should be removed from the list
      // of runtime elements above.
      assert(!runtimeElementsSet.has(jsname), 'runtimeElements contains library symbol: ' + ident);
      runtimeElements.push(jsname);
    }
  }

  // check all exported things exist, error when missing
  runtimeElementsSet = new Set(runtimeElements);
  for (const name of EXPORTED_RUNTIME_METHODS) {
    if (!runtimeElementsSet.has(name)) {
      error(`undefined exported symbol: "${name}" in EXPORTED_RUNTIME_METHODS`);
    }
  }

  const exports = runtimeElements.filter(shouldExport);
  const results = exports.map(exportSymbol);

  if (MODULARIZE == 'instance') {
    if (results.length == 0) return '';
    return '// Runtime exports\nexport { ' + results.join(', ') + ' };\n';
  }

  if (ASSERTIONS && !EXPORT_ALL) {
    // in ASSERTIONS mode we show a useful error if it is used without being
    // exported.  See `unexportedRuntimeSymbol` in runtime_debug.js.
    const unusedLibSymbols = getUnusedLibrarySymbols();
    if (unusedLibSymbols.size) {
      results.push(addMissingLibraryStubs(unusedLibSymbols));
    }

    const unexported = [];
    for (const name of runtimeElements) {
      if (
        !EXPORTED_RUNTIME_METHODS.has(name) &&
        !EXPORTED_FUNCTIONS.has(name) &&
        !extraExports.has(name) &&
        !unusedLibSymbols.has(name)
      ) {
        unexported.push(name);
      }
    }

    if (unexported.length || unusedLibSymbols.size) {
      let unexportedStubs = 'var unexportedSymbols = [\n';
      for (const sym of unexported) {
        unexportedStubs += `  '${sym}',\n`;
      }
      unexportedStubs += '];\n';
      unexportedStubs += 'unexportedSymbols.forEach(unexportedRuntimeSymbol);\n';
      results.push(unexportedStubs);
    }
  }

  results.unshift('// Begin runtime exports');
  results.push('// End runtime exports');
  return results.join('\n  ') + '\n';
}

function exportLibrarySymbols() {
  assert(MODULARIZE != 'instance');
  const results = ['// Begin JS library exports'];
  for (const ident of librarySymbols) {
    if ((EXPORT_ALL || EXPORTED_FUNCTIONS.has(ident) || extraExports.has(ident)) && !nativeAliases[ident] && !ident.startsWith('HEAP')) {
      results.push(exportSymbol(ident));
    }
  }
  results.push('// End JS library exports');
  return results.join('\n  ') + '\n';
}

function exportJSSymbols() {
  // In MODULARIZE=instance mode JS library symbols are marked with `export`
  // at the point of declaration.
  if (MODULARIZE == 'instance') return exportRuntimeSymbols();
  return exportRuntimeSymbols() + '  ' + exportLibrarySymbols();
}

addToCompileTimeContext({
  exportJSSymbols,
  loadStructInfo,
  LibraryManager,
  librarySymbols,
  extraExports,
  addToLibrary,
  cDefs,
  cDefine,
  C_STRUCTS,
  C_DEFINES,
});
