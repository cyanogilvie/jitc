# JITC

Just In Time C for Tcl

## SYNOPSIS

**package require jitc** ?0.5?

**jitc::capply** *cdef* *symbol* ?*arg* …?  
**jitc::bind** *name* *cdef* *symbol* ?*curryarg* …?  
**jitc::symbols** *cdef*  
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
related settings as a value stored in a Tcl\_Obj and doing the
compilation as needed, caching the result and freeing the memory when
the last reference goes away.

The generation of object code is done by an embedded TinyCC compiler.

The compiler supports the most of the C99 and C11 C language standards.

## COMMANDS

  - **jitc::capply** *cdef* *symbol* ?*arg* …?  
    Execute *symbol* in the compiled *cdef* as a Tcl\_ObjCmdProc. If
    *symbol* points to something other than a Tcl\_ObjCmdProc things are
    likely to get interesting quickly.
  - **jitc::bind** *name* *cdef* *symbol* ?*curryarg* …?  
    Register *name* as a command that invokes *symbol* in *def*.
    *symbol* must be a Tcl\_ObjCmdProc or you’re in for a bad time. If
    any *curryarg*s are supplied they are prepended to any args passed
    to *symbol* when *name* is invoked.
  - **jitc::symbols** *cdef*  
    Return a list of the symbols in *cdef*.
  - **jitc::packageinclude**  
    Return the path for the headers bundled with this package.
  - **jitc::re2c** ?*option* …? *source*  
    Process the C source code *source* through the bundled **re2c**.
    *option*s are as understood by **re2c** (see **re2c**(1)). The
    modified source code is returned. Useful as a filter (see the
    **filter** part) to implement very fast regular expression based
    lexers.

## CDEF FORMAT

The *cdef* argument to **jitc::capply** and **jitc::symbols** is a list
of pairs of elements: *part* and *value*. When compiling and linking the
code the parts are applied in sequence. *part* must be one of:

  - **code**  
    *value* is a chunk of C code as a string.

  - **file**  
    *value* names a file containing C code. Cannot currently refer to a
    path handled by a Tcl VFS plugin.

  - **mode**  
    Select the mode of operation *value*, which must be either **tcl**
    (the default), or **raw**. **tcl** mode automatically sets up
    include and library search paths to link to the running Tcl
    interpreter, and includes a header file which brings in **tcl.h**
    and defines a handful of convenience macros for implementing Tcl
    commands in C (see the **CONVENIENCE MACROS** section). Mode **raw**
    turns off this behaviour.

  - **debug**  
    *value* names a filesystem path (which must exist, and be a
    directory) into which to write copies of the code sections specified
    by **code** parts. This is useful when debugging the code, so that
    the debugger can find the source code. The files are removed when
    the *cdef* is freed (except if the program crashes, in which case
    having the files left behind is beneficial for examining the
    resulting core file in a debugger).

  - **options**  
    *value* contains an option string as would be passed to **tcc(1)**.
    For example, to turn on debugging and all warnings, and bounds
    checking: **-g -Wall -b**.

  - **include\_path**  
    Add the path in *value* to the paths searched for include files.

  - **sysinclude\_path**  
    Add the path in *value* to the paths searched for system include
    files.

  - **symbols**  
    Import symbols from the *cdef* given as the first element of
    *value*. The following (0 or more) elements of *value* name symbols
    to import from that cdef.

  - **library\_path**  
    Add the path in *value* to the list of paths searched for libraries.

  - **library**  
    Add the path in *value* to the libraries linked into the code.

  - **tccdir**  
    Set the default path searched for the built-in tcc libraries and
    headers. Defaults to the bundled files with this package.

  - **define**  
    Define a preprocessor symbol. *value* must be a 2 element list, the
    first of which is the name of the symbol and the second its value.

  - **undefine**  
    Undefine the preprocessor symbol *value*.

  - **package**  
    Load and link with the Tcl package *value*, which must be a list,
    the first element of which names the required package and the
    remaining elements are args as accepted by the **package require**
    Tcl command to constrain the version requirements. In addition to
    loading the package, if it exports build information via
    *package\_name***::pkgconfig** (Tip \#59, Tcl\_RegisterConfig) the
    exported configuration will be used to automatically extend the
    include and library paths searched, link in the library and
    automatically include the package’s header file in all **code**
    parts. The values used from the exported config are:
    
    | Key                | Effect                                       |
    | ------------------ | -------------------------------------------- |
    | header             | Added as an include in all **code** parts    |
    | includedir,runtime | Added as a search path for headers, ala -I   |
    | includedir,install | Added as a search path for headers, ala -I   |
    | libdir,runtime     | Added as a search path for libraries, ala -L |
    | libdir,install     | Added as a search path for libraries, ala -L |
    | library            | Linked into the compiled code, ala -l        |
    

    Any keys that aren’t defined are ignored.

  - **filter**  
    Pass the C source code through the filter specified by *value*,
    which must be a Tcl command prefix to which will be added an arg
    containing the C source code and which must return the modified
    source.

  - **export**  
    Declare the symbols exported from this object and the header text
    required to use them, for use by other cdefs as described by the
    **use** part below. *value* must be a dictionary with keys described
    below:
    
    | Key         | Description                                         |
    | ----------- | --------------------------------------------------- |
    | **symbols** | A list of the exported symbols. Optional            |
    | **header**  | The text of the header section to include. Optional |
    

  - **use**  
    Link with the cdef given in *value*. Any symbols and header text it
    declares in its **export** part are automatically imported.

## SPECIAL SYMBOLS

If the *cdef* exports a symbol **init** then that is called when the
compile is done. **init** must be a function taking a **Tcl\_Interp**
and returning **int**: TCL\_OK if the initialization succeeded or
TCL\_ERROR if it failed (an error message should be left in the
interpreter result as usual in this case). Any error thrown will
propagate to the command that caused the compilation.

If the *cdef* exports a symbol **release** then it is called when the
memory containing the compiled *cdef* is about to be freed. It must be a
function taking **Tcl\_Interp** and returning **void**. It should
reverse any side effects created in the interpreter by **init** or any
of the code run in the *cdef*, and free any memory it allocated.

## CONVENIENCE MACROS

In the default **tcl** mode (as selected by the **mode** part of the
*cdef*), some helpful macros and utilities are included:

| Macro                                                    | Description                                                                                                                                                                                                                                                                                   |
| -------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **INIT**                                                 | Expands to the standard initialization hook export: “int init(Tcl\_Interp\* interp)”.                                                                                                                                                                                                         |
| **RELEASE**                                              | Expands to the standard release hook export: “void release(Tcl\_Interp\* interp)”.                                                                                                                                                                                                            |
| **OBJCMD**(*name*)                                       | Defines a Tcl\_ObjCmdProc named *name*. **cdata** is the passed ClientData, **interp** is the Tcl\_Interp, **objc** holds the count of arguments and **objv** is an array of Tcl\_Obj pointers to the arguments.                                                                              |
| **CHECK\_ARGS**(*expecting*, *msg*)                      | Checks that the count of arguments passed to the command matches *expecting* (in addition to the command argument itself). If not, an error message is left in the **interp** containing *msg* and we return **TCL\_ERROR**.                                                                  |
| **TEST\_OK\_LABEL**(*label*, *code*, *checked\_command*) | Test the return code from *checked\_command* and store it in *code*. If it differs from **TCL\_OK** jump to the label *label*. Useful to implement exception handling that releases any allocated resources and returns *code* at the end of the function.                                    |
| **replace\_tclobj**(*varPtr*, *replacement*)             | Assign the Tcl\_Obj pointed to by *replacement* into the variable whose address is *varPtr*, managing the refCount for the Tcl\_Objs. If the variable being assigned to already pointed to a Tcl\_Obj, its refcount is decremented. If *replacement* is non-NULL its refcount is incremented. |

To make these available in source code referend in **file** parts, or
**code** parts in **raw** **mode**, include **tclstuff.h**, which is
installed in the package installation directory. This is in the default
include search path for **mode** **tcl**, but can be retrieved by the
command **jitc::packageinclude**

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
        int len;
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
            CHECK_ARGS(1, "str");
            int         len;
            const char* str = Tcl_GetStringFromObj(objv[1], &len);
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

Note the use of global Tcl\_Objs **g\_true** and **g\_false** to store
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
the Tcl\_RegisterConfig (TIP 59) mechanism, as described in the
**package** part:

``` tcl
package require jitc

set cdef {
    package     {dedup 0.9.3}
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
            CHECK_ARGS(1, "string");
            int len;
            const char* str = Tcl_GetStringFromObj(objv[1], &len);
            Tcl_SetObjResult(interp, Dedup_NewStringObj(g_dedup, str, len));
            return TCL_OK;
        }

        OBJCMD(stats) {
            Tcl_DString ds;

            CHECK_ARGS(0, "");
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

  - int **Jitc\_GetSymbolFromObj**(Tcl\_Interp\* *interp*, Tcl\_Obj\*
    *cdef*, Tcl\_Obj\* *symbol*, void\*\* *val*)  
    Retrieve the symbol *symbol* from *cdef*, compiling it if needed.
  - int **Jitc\_GetSymbolsFromObj**(Tcl\_Interp\* *interp*, Tcl\_Obj\*
    *cdef*, Tcl\_Obj\*\* *symbols*)  
    Retrieve a list of all symbols in *cdef*, compiling it if needed.
  - int **Jitc\_GetExportHeadersFromObj**(Tcl\_Interp\* *interp*,
    Tcl\_Obj\* *cdef*, Tcl\_Obj\*\* *headers*)  
    Retrieve the headers text exported from *cdef*, compiling it if
    needed. *headers* may be NULL if *cdef* doesn’t declare any exported
    header. Will still return **TCL\_OK** for this case.
  - int **Jitc\_GetExportSymbolsFromObj**(Tcl\_Interp\* *interp*,
    Tcl\_Obj\* *cdef*, Tcl\_Obj\*\* *symbols*)  
    Retrieve a list of the symbols declared for export from *cdef*,
    compiling it if needed. *symbols* may be NULL if *cdef* doesn’t
    declare any exported symbols. Will still return **TCL\_OK** for this
    case.

## BUGS

Please report any bugs to the github issue tracker:
https://github.com/cyanogilvie/jitc/issues

## SEE ALSO

critcl: https://wiki.tcl-lang.org/page/Critcl, tcc4tcl:
https://wiki.tcl-lang.org/page/tcc4tcl, tcc(1),
https://repo.or.cz/tinycc.git, re2c: https://en.wikipedia.org/wiki/Re2c,
packcc: https://en.wikipedia.org/wiki/PackCC, lemon:
https://sqlite.org/src/doc/trunk/doc/lemon.html.

## PROJECT STATUS

Work in progress, only very basic testing has been done. Using it
anywhere that matters would be very brave indeed.

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

This package is Copyright 2022 Cyan Ogilvie, and is made available under
the same license terms as the Tcl Core. The TCC compiler is LGPL. This
package does not distribute object code or source code from TCC and so
doesn’t trigger any GPL issues, but if you build this package and
distribute the result you will need to ensure that you are in compliance
with the terms of the TCC LGPL license. The git submodules for the
linked tools each have their own license: TinyCC is LGPL; re2c is public
domain; packcc is MIT; lemon and sqlite are public domain.
