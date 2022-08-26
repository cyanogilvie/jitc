# NAME

tinycc - Just In Time C for Tcl

# SYNOPSIS

**package require tinycc** ?0.1?

**tinycc::capply** *cdef* *symbol* ?*arg* …?  
**tinycc::symbols** *cdef*

# DESCRIPTION

This package provides just-in-time compilation of c code to memory for
Tcl scripts, with the ability to call functions in that compiled object,
and reference-counted memory management for the compiled objects.

There are several other similar projects (critcl, tcc4tcl, etc.), but
this package takes a different design approach: treating c code and
related settings as a value stored in a Tcl\_Obj and doing the
compilation as needed, caching the result and freeing the memory when
the last reference goes away.

The generation of object code is done by an embedded TinyCC compiler.

# COMMANDS

  - **tinycc::capply** *cdef* *symbol* ?*arg* …?  
    Execute *symbol* in the compiled *cdef* as a Tcl\_ObjCmdProc. If
    *symbol* points to something other than a Tcl\_ObjCmdProc things are
    likely to get interesting quickly.
  - **tinycc::symbols** *cdef*  
    Return a list of the symbols in *cdef*.

# CDEF FORMAT

The *cdef* argument to **tinycc::capply** and **tinycc:symbols** is a
list of pairs of elements: *part* and *value*. When compiling and
linking the code the parts are applied in sequence. *part* must be one
of:

  - **code**  
    *value* is a chunk of c code as a string.
  - **file**  
    *value* names a file containing c code. Cannot currently refer to a
    path handled by a Tcl VFS plugin.
  - **debug**  
    *value* names a filesystem path (which must exist, and be a
    directory) in which to write copies of the code sections specified
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
  - **symbol**  
    Define a symbol: *value* is a two element list, the first of which
    is another *cdef* and the second the name of the symbol from that
    *cdef* to import.
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

# SPECIAL SYMBOLS

If the *cdef* exports a symbol **init** then that is called when the
compile is done. **init** must be a function taking a **Tcl\_Interp**
and returning **int**: TCL\_OK if the initialization succeeded or
TCL\_ERROR if it failed (an error message should be left in the
interpreter result as usual in this case). Any error thrown will
propagate to the command that caused the compilation.

If the *cdef* exports a symbol **release** then it is called when the
memory containing the compiled *cdef* is about to be freed. It must be a
function taking **Tcl\_Interp** and returning void. It should reverse
any side effects created in the interpreter by **init** or any of the
code run in the *cdef*, and free any memory it allocated.

# EXAMPLES

Hello, world:

``` tcl
package require tinycc

tinycc::capply {
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
} hello tinycc
```

**init** and **release** symbols for resource management:

``` tcl
package require tinycc

set cdef [list code {
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
    }

    void release(Tcl_Interp* interp)
    {
        Tcl_DStringFree(&g_str);
        printf("%s", "Freed g_str\n");
    }
}]

tinycc::capply $cdef accumulate "foo"
tinycc::capply $cdef accumulate "bar"

unset cdef
```

Produces:

    Initialized g_str
    Current value of the accumulated value: foo
    Current value of the accumulated value: foobar
    Freed g_str

# BUGS

Please report any bugs to the github issue tracker:
https://github.com/cyanogilvie/tinycc/issues

# SEE ALSO

tcc(1), https://repo.or.cz/tinycc.git

# PROJECT STATUS

Draft, never been built, let alone run and tested. Using it anywhere
that matters would be very brave indeed.

# TODO

  - [ ] Implement **init** and **release**
  - [ ] More test coverage
  - [ ] Document coverage, debugging

# LICENSE

This package is Copyright 2022 Cyan Ogilvie, and is made available under
the same license terms as the Tcl Core. The TCC compiler is LGPL. This
package does not distribute object code or source code from TCC and so
doesn’t trigger any GPL issues, but if you build this package and
distribute the result you will need to ensure that you are in compliance
with the terms of the TCC LGPL license.
