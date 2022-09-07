namespace eval ::jitclib {
	variable parse_tcc_errors {
		filter	{jitc::re2c --case-ranges --tags --utf8}
		code {
			//@begin=c@
			#include <stdint.h>
			#include <stdio.h>

			OBJCMD(parse) {
				CHECK_ARGS(1, "errorstr");

				/*!types:re2c*/
				int							code = TCL_OK;
				int							str_len;
				const unsigned char*const	str = (const unsigned char*)Tcl_GetStringFromObj(objv[1], &str_len);
				const unsigned char*		s = str;
				const unsigned char*		m = NULL;
				Tcl_Obj*					errors = NULL;
				/*!stags:re2c format = "const unsigned char*		@@;\n"; */
				const unsigned char			*fn_s, *fn_e, *line_s, *line_e, *msg_s, *msg_e;

				replace_tclobj(&errors, Tcl_NewListObj(0, NULL));

			line:
				/*!re2c
					re2c:yyfill:enable			= 0;
					re2c:define:YYCTYPE			= "unsigned char";
					re2c:define:YYCURSOR		= "s";
					re2c:define:YYMARKER		= "m";

					end			= [\x00];
					ws			= [ \x09]+;
					fnchar		= [\x20-\u10ffff] \ ">";
					digit		= [0-9];
					errmsg		= ([\x20-\u10ffff] \ "\n")+;
					eol			= "\n";
					errline		= "<" @fn_s fnchar+ @fn_e ">:" @line_s digit+ @line_e ":" ws @msg_s errmsg @msg_e;

					eol {
						goto line;
					}

					errline {
						TEST_OK_LABEL(done, code, Tcl_ListObjAppendElement(interp, errors, Tcl_NewListObj(3, (Tcl_Obj*[]){
							Tcl_NewStringObj(fn_s, fn_e-fn_s),
							Tcl_NewStringObj(line_s, line_e-line_s),
							Tcl_NewStringObj(msg_s, msg_e-msg_s)
						})));
						goto line;
					}

					end {
						Tcl_SetObjResult(interp, errors);
						goto done;
					}

					* {
						const unsigned char*		err = s-1;
						const ptrdiff_t	remain = (str+str_len) - err;
						THROW_PRINTF_LABEL(done, code, "Error parsing tcc error message at %d: \"%.*s%s\"",
							err-str, remain < 16 ? remain : 15, err, remain < 16 ? "" : "\u2026");
					}
				*/

			done:
				replace_tclobj(&errors, NULL);
				return code;
			}
			//@end=c@
		}
	}
}
# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
