# JITC

Just In Time C for Tcl

## SYNOPSIS

**package require jitc** ?0.8.5?

**jitc::capply** *cdef* *symbol* ?*arg* …?  
**jitc::bind** *name* *cdef* *symbol* ?*curryarg* …?  
**jitc::symbols** *cdef*  
**jitc::dump** **mir**\|**c** *cdef*  
**jitc::packageinclude**  
**jitc::re2c** ?*option* …? *source*  
**jitc::packcc** ?*option* …? *source*  
**jitc::lemon** ?*option* …? *source*

## DESCRIPTION

This package provides just-in-time compilation of C code to memory for
Tcl scripts, with the ability to call functions in that compiled object,
and reference-counted memory management for the compiled objects.

There are several other similar projects (critcl, tcc4tcl, etc.), but
this package takes a different design approach: treating C code and
related settings as a value stored in a Tcl_Obj and doing the
compilation as needed, caching the result and freeing the memory when
the last reference goes away.

Code is compiled in-process by libslimcc (an embeddable fork of the
slimcc C compiler) to MIR, which generates the machine code (x86_64 and
aarch64). The language is C23 (GNU dialect) with the TS 25755 **defer**
statement. Nothing is written to disk or loaded with **dlopen**, except
the source files written for the debugger when debug information is
requested.

Compiling a *cdef* also links it: functions and variables it references
but doesn’t define are resolved against the process (the Tcl core, libc,
and libraries loaded through **library** or **package** parts). A
reference that can’t be resolved fails the compile with a **JITC
COMPILE** error naming the undefined symbols.

## COMPILE ERRORS

A *cdef* that fails to compile or link raises an error whose message
lists each diagnostic with its location, the source line and a caret
under the offending token:

    ERROR: In "cdef", line 3: undefined variable 'y':
        return y + TCL_OK;
               ^

Warnings are always fatal, as they were with jitc’s TinyCC backend:
these are on, and all of them are reported before the compile fails:

| Warning                          | Diagnoses                                                                                                                              |
|----------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| **-Wincompatible-pointer-types** | assigning (or passing, initializing, returning) a pointer to a different type, other than through **void\*** or a change of signedness |
| **-Wdiscarded-qualifiers**       | a pointer conversion that drops **const** or **volatile** from the target                                                              |
| **-Wint-conversion**             | a pointer converted to an integer without a cast                                                                                       |
| **-Wreturn-type**                | control reaching the end of a function that returns a value                                                                            |

**\#warning** is fatal too. A warning can be turned off for a *cdef*
with **-Wno-** *name* in its **options**; **-Wno-error** has no effect.

The error code is **JITC COMPILE** *diagnostics* *code*, where *code* is
the code part that failed and *diagnostics* is a list with an element
per diagnostic of the form {*level* *file* *line* *message* *extras*}:
*level* is **error**, **warning** or **note** (more about the diagnostic
before it, such as the macro it was expanded from), *file* and *line*
are empty for a diagnostic with no location (an undefined symbol at link
time), and *extras* is a dictionary giving the diagnostic’s context:

| Key                        | Value                                                                                                                                                                                              |
|----------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **option**                 | the warning’s flag, such as **-Wreturn-type**                                                                                                                                                      |
| **source**                 | the full text the diagnostic is in, as the compiler saw it: the code part after any **filter**, or a header                                                                                        |
| **offset**, **length**     | the character offset of the offending token in **source**, and its length                                                                                                                          |
| **column**                 | the token’s character column on its line                                                                                                                                                           |
| **src_name**, **src_line** | the name and line of **source**, which differ from *file* and *line* when the code maps its lines with **\#line** (as **re2c** output does, so that *file* and *line* point at the **re2c** input) |

Only **option** is present for a diagnostic with no location. **source**
makes the context available to tools even for generated code that was
never written to disk.

**jitc::capply** *cdef* *symbol* ?*arg* …?  
Execute *symbol* in the compiled *cdef* as a Tcl_ObjCmdProc. If *symbol*
points to something other than a Tcl_ObjCmdProc things are likely to get
interesting quickly.

**jitc::bind** *name* *cdef* *symbol* ?*curryarg* …?  
Register *name* as a command that invokes *symbol* in *def*. *symbol*
must be a Tcl_ObjCmdProc or you’re in for a bad time. If any *curryarg*s
are supplied they are prepended to any args passed to *symbol* when
*name* is invoked. The *ClientData* passed to *symbol* is a
**Tcl_Obj\*\*** slot private to this binding, initially NULL and kept
across calls, in which *symbol* may cache a value: store a Tcl_Obj with
a reference held (**Tcl_IncrRefCount**), replacing (and releasing) any
previous one. The reference is released when *name* is deleted.

**jitc::symbols** *cdef*  
Return a list of the symbols in *cdef*.

**jitc::dump** **mir**\|**c** *cdef*  
Return an intermediate form of *cdef*, for inspection and debugging.
With **mir**, return the textual MIR intermediate representation of the
compiled *cdef* (compiling it first if necessary): the source-level IR
the C front-end emits, before the MIR code generator’s optimization
passes, including the runtime helper functions linked into every cdef.
With **c**, return the assembled C source as the compiler sees it: a
list with one element per **code** part, each being the **\#include**
preamble (assembled from the implicit Tcl headers plus any **package**,
**use** and **export** headers) prepended to that block’s body after its
**filter** (if any) has run. **file** parts are standalone translation
units and are not included.

**jitc::packageinclude**  
Return the path for the headers bundled with this package.

**jitc::re2c** ?*option* …? *source*  
Process the C source code *source* through the bundled **re2c**.
*option*s are as understood by **re2c** (see **re2c**(1)). The modified
source code is returned. Useful as a filter (see the **filter** part) to
implement very fast regular expression based lexers.

## CDEF FORMAT

The *cdef* argument to **jitc::capply** and **jitc::symbols** is a list
of pairs of elements: *part* and *value*. When compiling and linking the
code the parts are applied in sequence. *part* must be one of:

**code**  
*value* is a chunk of C code as a string.

**file**  
*value* names a file containing C code. Cannot currently refer to a path
handled by a Tcl VFS plugin.

**mode**  
Select the mode of operation *value*, which must be either **tcl** (the
default), or **raw**. **tcl** mode automatically sets up include and
library search paths to link to the running Tcl interpreter, and
includes a header file which brings in **tcl.h** and defines a handful
of convenience macros for implementing Tcl commands in C (see the
**CONVENIENCE MACROS** section). Mode **raw** turns off this behaviour.

**debug**  
Generate debug information for the *cdef*, as for **-g** in **options**
(see there). *value* is accepted for compatibility and ignored: copies
of the code sections, for the debugger to show, are written to a
temporary directory under **P_tmpdir**, removed when the *cdef* is freed
(but left behind if the program crashes, so a core file can be examined
with its source).

**options**  
*value* contains a C compiler style option string. The string is split
into a Tcl list, and the following options are honoured:

- **-I** *dir* — add *dir* to the include search path (as
  **include_path**).

- **-D** *name* \[**=** *val*\] — predefine a preprocessor macro (as
  **define**).

- **-O** *n* — set the MIR code generator’s optimization level for this
  *cdef*. *n* is 0–3 (a bare **-O** means **-O1**); higher values are
  clamped to MIR’s maximum. **-O0** disables register allocation and
  most optimization passes for the fastest compile, **-O1** adds
  register allocation and the cheap passes, **-O2** (the default)
  enables the standard passes, and **-O3** adds the more expensive ones.
  The last **-O** wins. See **PERFORMANCE** for the trade-offs.

- **-g** (any **-g**\* form) — generate debug information, registered
  with gdb through its JIT interface: function symbols, source line
  tables, variables and call frame information, so backtraces (live or
  from a core file) and stepping work through compiled code. On its own
  **-g** implies **-O0** with every local kept in memory, for full
  variable inspection. With an explicit **-O1** or higher, the code is
  optimized as without **-g** (as gcc’s **-g -O**): backtraces and line
  information remain, but some locals can’t be printed. **-g** without
  **-O** is therefore much slower than the default, so production code
  wanting debug information should combine it with an explicit **-O2**
  (see **PERFORMANCE**).

- **-Wno-** *name* — turn off a warning (see **COMPILE ERRORS**);
  **-W** *name* turns it back on.

All other options (**-std**, linker options, **-U**, &c.) have no
libslimcc equivalent and are silently ignored.

**include_path**  
Add the path in *value* to the paths searched for include files.

**sysinclude_path**  
Add the path in *value* to the paths searched for system include files.

**symbols**  
Import symbols from the *cdef* given as the first element of *value*.
The following (0 or more) elements of *value* name symbols to import
from that cdef.

**library_path**  
Add the path in *value* to the list of paths searched for libraries.

**library**  
Add the path in *value* to the libraries linked into the code.

**tccpath**  
Accepted for compatibility with the TinyCC backend of jitc 0.7 and
earlier, and ignored.

**define**  
Define a preprocessor symbol. *value* must be a 2 element list, the
first of which is the name of the symbol and the second its value.

**undefine**  
Undefine the preprocessor symbol *value*. Currently ignored (libslimcc
has no interface for it).

**package**  
Load and link with the Tcl package *value*, which must be a list, the
first element of which names the required package and the remaining
elements are args as accepted by the **package require** Tcl command to
constrain the version requirements. In addition to loading the package,
if it exports build information via *package_name***::pkgconfig** (Tip
\#59, Tcl_RegisterConfig) the exported configuration will be used to
automatically extend the include and library paths searched, link in the
library and automatically include the package’s header file in all
**code** parts. The values used from the exported config are:

| Key                | Effect                                       |
|--------------------|----------------------------------------------|
| header             | Added as an include in all **code** parts    |
| includedir,runtime | Added as a search path for headers, ala -I   |
| includedir,install | Added as a search path for headers, ala -I   |
| libdir,runtime     | Added as a search path for libraries, ala -L |
| libdir,install     | Added as a search path for libraries, ala -L |
| library            | Linked into the compiled code, ala -l        |

Any keys that aren’t defined are ignored.

**filter**  
Pass the C source code through the filter specified by *value*, which
must be a Tcl command prefix to which will be added an arg containing
the C source code and which must return the modified source.

**export**  
Declare the symbols exported from this object and the header text
required to use them, for use by other cdefs as described by the **use**
part below. *value* must be a dictionary with keys described below:

| Key         | Description                                         |
|-------------|-----------------------------------------------------|
| **symbols** | A list of the exported symbols. Optional            |
| **header**  | The text of the header section to include. Optional |

**use**  
Link with the cdef given in *value*. Any symbols and header text it
declares in its **export** part are automatically imported.

## SPECIAL SYMBOLS

If the *cdef* exports a symbol **init** then that is called when the
compile is done. **init** must be a function taking a **Tcl_Interp** and
returning **int**: TCL_OK if the initialization succeeded or TCL_ERROR
if it failed (an error message should be left in the interpreter result
as usual in this case). Any error thrown will propagate to the command
that caused the compilation.

If the *cdef* exports a symbol **release** then it is called when the
memory containing the compiled *cdef* is about to be freed. It must be a
function taking **Tcl_Interp** and returning **void**. It should reverse
any side effects created in the interpreter by **init** or any of the
code run in the *cdef*, and free any memory it allocated.

## CONVENIENCE MACROS

In the default **tcl** mode (as selected by the **mode** part of the
*cdef*), some helpful macros and utilities are included:

| Macro                                                 | Description                                                                                                                                                                                                                                                                                                                                                  |
|-------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **INIT**                                              | Expands to the standard initialization hook export: “int init(Tcl_Interp\* interp)”.                                                                                                                                                                                                                                                                         |
| **RELEASE**                                           | Expands to the standard release hook export: “void release(Tcl_Interp\* interp)”.                                                                                                                                                                                                                                                                            |
| **OBJCMD**(*name*)                                    | Defines a Tcl_ObjCmdProc named *name*. **cdata** is the passed ClientData, **interp** is the Tcl_Interp, **objc** holds the count of arguments and **objv** is an array of Tcl_Obj pointers to the arguments.                                                                                                                                                |
| **CHECK_ARGS**(*msg*)                                 | Checks that the count of arguments passed to the command matches **A_objc** (in addition to the command argument itself identified by **A_cmd**). If not, an error message is left in the **interp** containing *msg* and we return **TCL_ERROR**. The usual idiom is to define an enum to assign symbolic labels to the argument numbers, see the examples. |
| **TEST_OK_LABEL**(*label*, *code*, *checked_command*) | Test the return code from *checked_command* and store it in *code*. If it differs from **TCL_OK** jump to the label *label*. Useful to implement exception handling that releases any allocated resources and returns *code* at the end of the function.                                                                                                     |
| **replace_tclobj**(*varPtr*, *replacement*)           | Assign the Tcl_Obj pointed to by *replacement* into the variable whose address is *varPtr*, managing the refCount for the Tcl_Objs. If the variable being assigned to already pointed to a Tcl_Obj, its refcount is decremented. If *replacement* is non-NULL its refcount is incremented.                                                                   |

To make these available in source code referend in **file** parts, or
**code** parts in **raw** **mode**, include **tclstuff.h**, which is
installed in the package installation directory. This is in the default
include search path for **mode** **tcl**, but can be retrieved by the
command **jitc::packageinclude**

Against Tcl 8.6 the preamble also supplies the parts of the Tcl 8.7+ C
API that cdefs commonly use: **Tcl_Size** (and **TCL_SIZE_MAX**,
**TCL_SIZE_MODIFIER**), and the TIP 445 internal representation API
(**Tcl_ObjInternalRep**, **Tcl_FetchInternalRep**,
**Tcl_StoreInternalRep**, **Tcl_FreeInternalRep**, &c) from
**tip445.h**, installed alongside **tclstuff.h**. So the same cdef
source compiles against Tcl 8.6 and 9. Against the Tcl 8.7 alphas, which
have that API under its pre-release names (**Tcl_FetchIntRep**, …),
**tip445.h** maps the final names onto those.

## EXAMPLES

Hello, world:

``` tcl
package require jitc

jitc::capply {
    code {
        int hello(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[])
        {
            if (objc != 2) {
                Tcl_WrongNumArgs(interp, 1, objv, "noun");
                return TCL_ERROR;
            }
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("hello, %s", Tcl_GetString(objv[1])));
            return TCL_OK;
        }
    }
} hello jitc
```

### Resource Management

**init** and **release** symbols for resource management:

``` tcl
package require jitc

set cdef [string trim { code {
    #include <stdio.h>

    Tcl_DString     g_str;

    int accumulate(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[])
    {
        Tcl_Size len;
        const char* str;

        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 1, objv, "string");
            return TCL_ERROR;
        }

        str = Tcl_GetStringFromObj(objv[1], &len);

        Tcl_DStringAppend(&g_str, str, len);

        printf("Current value of the accumulated value: %*s\n",
            Tcl_DStringLength(&g_str),
            Tcl_DStringValue(&g_str));

        return TCL_OK;
    }

    int init(Tcl_Interp* interp)
    {
        Tcl_DStringInit(&g_str);
        printf("%s", "Initialized g_str\n");
        return TCL_OK;
    }

    void release(Tcl_Interp* interp)
    {
        Tcl_DStringFree(&g_str);
        printf("%s", "Freed g_str\n");
    }
}}]     ;# string trim to avoid a reference in the literal table

jitc::capply $cdef accumulate "foo"
jitc::capply $cdef accumulate "bar"

unset cdef
```

Produces:

    Initialized g_str
    Current value of the accumulated value: foo
    Current value of the accumulated value: foobar
    Freed g_str

### Filters

Check if the supplied argument is a valid decimal number, using a re2c
filter and the standard convenience macros:

``` tcl
package require jitc

jitc::capply {
    filter  {jitc::re2c --no-debug-info --case-ranges}
    code    {
        static Tcl_Obj* g_true  = NULL;
        static Tcl_Obj* g_false = NULL;

        INIT {
            replace_tclobj(&g_true,  Tcl_NewBooleanObj(1));
            replace_tclobj(&g_false, Tcl_NewBooleanObj(0));
            return TCL_OK;
        }

        RELEASE {
            replace_tclobj(&g_true,  NULL);
            replace_tclobj(&g_false, NULL);
        }

        OBJCMD(isdecimal) {
            enum {A_cmd, A_STR, A_objc};
            CHECK_ARGS("str");
            Tcl_Size    len;
            const char* str = Tcl_GetStringFromObj(objv[A_STR], &len);
            const char* YYCURSOR = str;
            const char* YYLIMIT  = str+len;
            const char* YYMARKER;
            /*!re2c
                re2c:yyfill:enable = 0;
                re2c:define:YYCTYPE = "char";
                re2c:eof = 0;

                number = [1-9][0-9]*;

                number  {
                    Tcl_SetObjResult(interp,
                        YYCURSOR==YYLIMIT ? g_true : g_false
                    );
                    goto done;
                }
                $       { Tcl_SetObjResult(interp, g_false); goto done; }
                *       { Tcl_SetObjResult(interp, g_false); goto done; }
            */
        done:
            return TCL_OK;
        }
    }
} isdecimal 12345
```

Note the use of global Tcl_Objs **g_true** and **g_false** to store
cached true and false Tcl values. This is safe to do here because this
compiled object can only be called from this Tcl interpreter (and thus
thread), and the created objects are released in the RELEASE handler, so
that when the *cdef* value is no longer reachable they will be freed,
avoiding a memory leak. Also, although they are global variables, they
are not exported by the linker, so their scope is limited to only the
code in this **code** part.

### Package Dependencies

Use the **package** part mechanism to bring in the **dedup** package
(https://github.com:cyanogilvie/dedup) and automatically load it,
include its header, link to the lib and set up the include and library
search paths for the compiler to find its resources. This automatic
setup requires the package to make its build information available via
the Tcl_RegisterConfig (TIP 59) mechanism, as described in the
**package** part:

``` tcl
package require jitc

set cdef {
    package     {dedup 0.9.19}
    code        {
        struct dedup_pool* g_dedup = NULL;

        INIT {
            g_dedup = Dedup_NewPool(interp);
            return TCL_OK;
        }

        RELEASE {
            Dedup_FreePool(g_dedup);
            g_dedup = NULL;
        }

        OBJCMD(dedup) {
            enum {A_cmd, A_STR, A_objc};
            CHECK_ARGS("string");
            Tcl_Size len;
            const char* str = Tcl_GetStringFromObj(objv[A_STR], &len);
            Tcl_SetObjResult(interp, Dedup_NewStringObj(g_dedup, str, len));
            return TCL_OK;
        }

        OBJCMD(stats) {
            Tcl_DString ds;

            enum {A_cmd, A_objc};
            CHECK_ARGS("");
            Tcl_DStringInit(&ds);
            Dedup_Stats(&ds, g_dedup);
            Tcl_SetObjResult(interp,
                Tcl_NewStringObj(Tcl_DStringValue(&ds),
                Tcl_DStringLength(&ds)));
            Tcl_DStringFree(&ds);
            return TCL_OK;
        }
    }
}

set first   [jitc::capply $cdef dedup "foo bar"]
set second  [jitc::capply $cdef dedup "foo bar"]
puts "first:  [tcl::unsupported::representation $first]"
puts "second: [tcl::unsupported::representation $second]"
puts "dedup pool stats:\n[jitc::capply $cdef stats]"
```

## C API

This package exports a stubs API for use by other extensions:

int **Jitc_GetSymbolFromObj**(Tcl_Interp\* *interp*, Tcl_Obj\* *cdef*, Tcl_Obj\* *symbol*, void\*\* *val*)  
Retrieve the symbol *symbol* from *cdef*, compiling it if needed.

int **Jitc_GetSymbolsFromObj**(Tcl_Interp\* *interp*, Tcl_Obj\* *cdef*, Tcl_Obj\*\* *symbols*)  
Retrieve a list of all symbols in *cdef*, compiling it if needed.

int **Jitc_GetExportHeadersFromObj**(Tcl_Interp\* *interp*, Tcl_Obj\* *cdef*, Tcl_Obj\*\* *headers*)  
Retrieve the headers text exported from *cdef*, compiling it if needed.
*headers* may be NULL if *cdef* doesn’t declare any exported header.
Will still return **TCL_OK** for this case.

int **Jitc_GetExportSymbolsFromObj**(Tcl_Interp\* *interp*, Tcl_Obj\* *cdef*, Tcl_Obj\*\* *symbols*)  
Retrieve a list of the symbols declared for export from *cdef*,
compiling it if needed. *symbols* may be NULL if *cdef* doesn’t declare
any exported symbols. Will still return **TCL_OK** for this case.

## PERFORMANCE

A *cdef* is compiled once, the first time a symbol is needed from it,
and the result is cached in the *cdef* value’s internal representation.
Compile time is therefore paid once per distinct *cdef* value, not per
call. Keep *cdef* values in variables or namespace globals rather than
rebuilding them per call, so that the cached compile survives. The
**tcl** mode preamble (tcl.h and the headers it pulls in) is
preprocessed once and cached, so even a small *cdef* compiles in a
couple of milliseconds.

The optimization level trades compile time for run time. The table below
gives timings for an re2c-generated lexer: **jitclib::json_check**’s
JSON validator, checking a 3.4 MB document. Each row shows compiling the
*cdef* and running it once, by **-O** level, against the TinyCC backend
of jitc 0.7:

| Backend           | x86_64 compile | x86_64 run | aarch64 compile | aarch64 run |
|-------------------|----------------|------------|-----------------|-------------|
| TinyCC (jitc 0.7) | 4.6 ms         | 7.7 ms     | 11.5 ms         | 28.6 ms     |
| **-O0**           | 7.9 ms         | 7.2 ms     | 22.2 ms         | 18.4 ms     |
| **-O1**           | 8.8 ms         | 6.2 ms     | 23.9 ms         | 16.7 ms     |
| **-O2** (default) | 11.5 ms        | 5.6 ms     | 30.5 ms         | 14.1 ms     |
| **-O3**           | 12.2 ms        | 5.6 ms     | 30.6 ms         | 14.0 ms     |
| **-g**            | 8.3 ms         | 25.5 ms    |                 |             |
| **-g -O2**        | 10.6 ms        | 5.6 ms     |                 |             |

The x86_64 timings are from an Intel i7-12800H performance core; the
aarch64 timings are from an AWS t4g (Graviton2) instance running Alpine
Linux.

- **-O2**, the default, compiles about 30% slower than **-O1** and runs
  10–16% faster, which a *cdef* called more than a handful of times pays
  back. **-O3** costs more compile time for no measurable gain in
  typical code.
- **-O0** or **-O1** suit code that runs only once or a few times, such
  as one-off generated code where compile time dominates.
- **-g** without an explicit **-O** gives full variable inspection in
  gdb, by keeping every local variable in memory and disabling inlining.
  That makes the code several times slower (4.5× in the table above), so
  it is for interactive debugging, not production.
- **-g -O2**, or **-g** with any explicit level from **-O1** up, runs at
  full speed. The debug information costs compile time but nothing at
  run time. It keeps backtraces, line numbers and call frames, live in
  gdb or from a core file, but some locals can’t be printed, as with
  gcc’s **-g -O2**. This is the form to use for production code that
  wants debug information.
- Calls between functions in a *cdef* are cheap. Moving the lexer’s rule
  actions above into small helper functions costs under 1%, and calling
  them through function pointers about 2%.
- MIR inlines calls to small functions (up to about 50 MIR instructions;
  a helper like **replace_tclobj** is 37) defined in the same *cdef*,
  wherever they are called. Inlining a helper removes the call and,
  often more importantly, lets its body be optimized together with the
  caller’s code. A function making 48 **replace_tclobj** calls in a loop
  runs about 3.6× faster with them inlined than as calls. Inlining grows
  the caller, and so its compile time, by roughly 4 ms per 1000
  instructions added at **-O2** on x86_64. As a circuit breaker, each
  function stops inlining once it has grown by about 2000 instructions
  (and by 50% of its original size), so a function making hundreds of
  helper calls has only the first few dozen inlined.
- Functions declared **inline** (such as **static inline** helpers) may
  be larger: calls to them are inlined for bodies of up to about 200 MIR
  instructions, as long as the caller hasn’t grown by 50% through
  inlining. **inline** is only a hint, as in any C compiler, but it’s
  the way to ask for a mid-sized helper to be inlined.
- Calls through function pointers, to Tcl’s C API, to other *cdef*s
  (**use**, **symbols**) or to **library** code are never inlined. When
  **-g** is given without **-O**, nothing is inlined.

## BUGS

Please report any bugs to the github issue tracker:
https://github.com/cyanogilvie/jitc/issues

## SEE ALSO

critcl: https://wiki.tcl-lang.org/page/Critcl, tcc4tcl:
https://wiki.tcl-lang.org/page/tcc4tcl, slimcc:
https://github.com/fuhsnn/slimcc (libslimcc fork:
https://github.com/cyanogilvie/slimcc), MIR:
https://github.com/vnmakarov/mir, re2c:
https://en.wikipedia.org/wiki/Re2c, packcc:
https://en.wikipedia.org/wiki/PackCC, lemon:
https://sqlite.org/src/doc/trunk/doc/lemon.html.

## PROJECT STATUS

Versions up to 0.7 were in heavy production use with an embedded TinyCC
compiler. 0.8.0 replaces it with libslimcc and MIR: C23 with **defer**,
an optimizing code generator, multiple architectures (x86_64, aarch64),
and no temporary shared objects or **dlopen**.

## PLATFORMS

Supported: Linux on x86_64 (glibc, musl) and aarch64 (musl, glibc). cdef
code follows the host C ABI there, including structs passed or returned
by value (since 0.8.5: 0.8.0-0.8.4 miscompiled calls to host functions
returning small structs), with the few exceptions listed in the notes
below.

Not yet supported: macOS and Windows (the libslimcc build refuses them),
and Linux riscv64 (builds, but the C ABI isn’t followed for structs or
**va_list**). Remaining gaps on the supported targets, and what porting
to each of those needs, are tracked in libslimcc’s
[notes/mir-backend/platform-support.md](https://github.com/cyanogilvie/slimcc/blob/mir-backend/notes/mir-backend/platform-support.md).

## BUILDING

There are no external dependencies other than Tcl. The libslimcc and MIR
backends are built as meson subprojects, fetched from their pinned git
commits during `meson setup` (so the first setup needs network access).
Build from the release tarball:
https://github.com/cyanogilvie/jitc/releases/download/v0.8.5/jitc-v0.8.5.tar.gz
or recursively clone the git repo:

    git clone --recurse-submodules https://github.com/cyanogilvie/jitc

Building uses meson:

    meson setup build --buildtype=release
    meson compile -C build
    meson test -C build
    meson install -C build

Both Tcl 8.6 and 9.0 are supported. To build against a specific Tcl
installation, set `PKG_CONFIG_PATH`:

    PKG_CONFIG_PATH=/path/to/tcl/lib/pkgconfig meson setup build

JIT compiles find the C library’s headers (**stdio.h** and so on)
through the system include directories of the host they run on. By
default these are taken from the C compiler’s include search list when
jitc is configured. When the build compiler isn’t the target host’s
system compiler, set them explicitly with **-Dsys_includes**, for
example:

    meson setup build -Dsys_includes=/usr/local/include,/usr/include/x86_64-linux-gnu,/usr/include

**ci/xenial.Containerfile** is a build environment for hosts with an old
glibc (Ubuntu 16.04, glibc 2.23). It uses conda-forge’s gcc 14, which
links against a glibc 2.17 sysroot, so the built package runs on any
glibc from 2.17 on.

## TODO

- [x] Implement **init** and **release**
- [x] Implement **packageinclude**
- [x] Implement **filter**
- [x] Implement **jitc::re2c** wrapper
- [ ] Implement **jitc::packcc** wrapper
- [ ] Implement **jitc::lemon** wrapper
- [ ] Proper exceptions on compile errors
- [ ] More test coverage
- [ ] Document coverage, debugging

## LICENSE

This package is Copyright 2022-2026 Cyan Ogilvie, and is made available
under the same license terms as the Tcl Core. The compiler backends
linked into it are MIT licensed: libslimcc (slimcc, derived from
chibicc) and MIR. The bundled tools each have their own license: re2c is
public domain; packcc is MIT; lemon and sqlite are public domain.
